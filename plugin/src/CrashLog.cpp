#include "CrashLog.h"

#include "Log.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace
{
    // Eight reports, then silence: one crashing launch is enough to see what happened, and a loop
    // that faults thousands of times a second would turn the log into the thing that makes the
    // machine unresponsive.
    constexpr int kMaxReports = 8;

    std::atomic<int> g_logged{ 0 };

    // A vectored exception handler must not allocate and must not throw: an allocation failure here
    // unwinds out of a VEH, which is fatal, and a full or already-torn-down heap is one of the
    // reasons a game crashes in the first place. So the report is assembled in a fixed buffer and
    // handed to mclog::WriteRaw(), which cannot allocate either - nothing below uses std::format,
    // std::string or a container.
    //
    // The buffer is one line at a time rather than one big report, which keeps the handler's stack
    // use at about a kilobyte all told (512 bytes here, MAX_PATH in Describe): EXCEPTION_STACK_OVERFLOW
    // is one of the three codes handled here and it arrives with the stack already exhausted.
    constexpr std::size_t kLineBytes = 512;

    struct Reporter
    {
        char   buffer[kLineBytes];
        char*  p   = buffer;
        char*  end = buffer + kLineBytes - 1;  // one byte is kept back for the terminator

        void Text(const char* text)
        {
            while (*text && p < end)
                *p++ = *text++;
        }

        void Char(char c)
        {
            if (p < end)
                *p++ = c;
        }

        // {:#x}, {:08x} and {:02x} without std::format.
        void Hex(std::uint32_t value, int digits, bool alt = false)
        {
            static const char kHex[] = "0123456789abcdef";
            char              out[8];
            int               n = 0;
            do {
                out[n++] = kHex[value & 0xf];
                value >>= 4;
            } while (value && n < 8);
            if (alt)
                Text("0x");
            if (digits > 8)
                digits = 8;  // out[] holds eight: padding further would write past it
            // The padding counter is its own variable on purpose. `n` holds how many digits the value
            // actually has and does not change here, so a `while (n < digits ...)` test would stay true
            // and pad until the buffer filled - which is exactly what it used to do: any value with
            // fewer significant digits than `digits` buried the rest of the report under hundreds of
            // '0' characters, so a crash log read as
            //   eax 000000000000... (511 characters)
            // and every register, the stack words and the code bytes were gone. Only values with at
            // least `digits` significant digits came out right, which is why the exception code (8
            // digits, asked for 1) survived and nothing after it did.
            for (int pad = n; pad < digits && p < end; ++pad)
                *p++ = '0';
            while (n && p < end)
                *p++ = out[--n];
        }

        void Dec(std::uint32_t value)
        {
            char out[10];
            int  n = 0;
            do {
                out[n++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value && n < 10);
            while (n && p < end)
                *p++ = out[--n];
        }

        void Line()
        {
            if (p > buffer) {
                *p = '\0';
                mclog::WriteRaw(buffer);
            }
            p = buffer;
        }
    };

    // "module+offset" for an address, or the bare address.
    //
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS is what makes the parameter an address instead of a
    // module name, and GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT is not optional next to it:
    // without it the lookup raises the module's reference count, which would pin every module it
    // names for the rest of the session. Both are right.
    //
    // This is the one call here that can block, because it takes the loader lock: an exception raised
    // on a thread that was itself inside LoadLibrary leaves us waiting for a lock that thread holds.
    // Nothing below can fault instead - every path writes through a cursor that stops at the end of
    // the buffer, an AV inside a VEH is fatal - and the address-only fallback still leaves the rest
    // of the report, so the price of that one case is a stalled log on a game that is already dying.
    void Describe(Reporter& r, DWORD address)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(address)), &module) ||
            !module) {
            r.Hex(address, 8, true);
            return;
        }
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(module, path, MAX_PATH)) {
            r.Hex(address, 8, true);
            return;
        }
        const wchar_t* name = path;
        for (DWORD i = MAX_PATH; i > 0; --i) {
            if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
                name = path + i;
                break;
            }
        }
        // The file name only, ASCII: the game and its engine DLLs are ASCII, and converting the rest
        // of the path would mean a buffer the handler cannot own. Anything else prints as '?'.
        for (const wchar_t* c = name; *c; ++c)
            r.Char(*c < 0x20 || *c > 0x7e ? '?' : static_cast<char>(*c));
        r.Text("+");
        r.Hex(address - static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(module)), 1, true);
    }

    bool IsCode(DWORD address)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        // Deliberately narrow. Guarded() in Hook.h provokes access violations on purpose on engine
        // calls made on guesses and swallows them, so recording every first-chance fault would bury
        // the one that mattered under the ones we caused; these three are the ones that end a game.
        const auto* record = info ? info->ExceptionRecord : nullptr;
        if (!record)
            return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = record->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
            return EXCEPTION_CONTINUE_SEARCH;
        // A vectored handler gets the context of the faulting thread; without it there is nothing to
        // describe, and reading through a null pointer here would be the fatal kind of mistake.
        const CONTEXT* c = info->ContextRecord;
        if (!c)
            return EXCEPTION_CONTINUE_SEARCH;

        const int seen = g_logged.fetch_add(1);
        if (seen >= kMaxReports) {
            // Say it once, or the tail of a crashy session just stops and reads like a log that lost
            // the interesting part.
            if (seen == kMaxReports)
                mclog::WriteRaw("further exceptions are not recorded (8 per process is the limit)");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        Reporter r;
        r.Text("EXCEPTION ");
        r.Hex(code, 1, true);
        r.Text(" at ");
        Describe(r, c->Eip);
        // ExceptionInformation[0] is what the access was: 0 read, 1 write, 8 instruction fetch.
        // Calling anything non-zero a write reported the +0x1b80 shape - a call through a null
        // pointer - as a write, which points the reader at the wrong line of the disassembly.
        const char* kind = "?";
        if (record->NumberParameters >= 2) {
            if (record->ExceptionInformation[0] == 0)
                kind = "reading";
            else if (record->ExceptionInformation[0] == 1)
                kind = "writing";
            else if (record->ExceptionInformation[0] == 8)
                kind = "executing";
        }
        const ULONG_PTR faulted = record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0;
        r.Text(" (");
        r.Text(kind);
        r.Text(" ");
        r.Hex(static_cast<std::uint32_t>(faulted), 8, true);
        r.Text(") thread ");
        r.Dec(GetCurrentThreadId());
        r.Line();

        r.Text("  eax ");
        r.Hex(c->Eax, 8);
        r.Text(" ebx ");
        r.Hex(c->Ebx, 8);
        r.Text(" ecx ");
        r.Hex(c->Ecx, 8);
        r.Text(" edx ");
        r.Hex(c->Edx, 8);
        r.Text(" esi ");
        r.Hex(c->Esi, 8);
        r.Text(" edi ");
        r.Hex(c->Edi, 8);
        r.Text(" ebp ");
        r.Hex(c->Ebp, 8);
        r.Text(" esp ");
        r.Hex(c->Esp, 8);
        r.Line();

        // Return addresses on the stack: which calls led here (ours show up as MaxCraft.asi+...).
        // Bounded by the committed region Esp sits in, so the walk cannot read past the top of the
        // stack: VirtualQuery answers with that region, and a hole inside it - a /GS page, a large
        // frame - is stepped over instead of ending the walk, because one unreadable word with a
        // return address above it is normal in the middle of a frame.
        MEMORY_BASIC_INFORMATION stackRegion{};
        DWORD                   stackLimit = 0x400;  // no region to bound it by: 1 KB of stack
        if (VirtualQuery(reinterpret_cast<LPCVOID>(c->Esp), &stackRegion, sizeof(stackRegion)) && stackRegion.State == MEM_COMMIT &&
            stackRegion.RegionSize) {
            const auto regionEnd = reinterpret_cast<ULONG_PTR>(stackRegion.BaseAddress) + stackRegion.RegionSize;
            if (regionEnd > c->Esp)
                stackLimit = static_cast<DWORD>(regionEnd - c->Esp);
        }
        r.Text("  stack:");
        int found = 0;
        int holes = 0;
        for (std::uint32_t offset = 0; offset + 4 <= stackLimit && found < 16 && holes < 8; offset += 4) {
            DWORD value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c->Esp + offset), &value, sizeof(value), nullptr)) {
                ++holes;
                continue;
            }
            holes = 0;
            if (value > 0x10000 && IsCode(value)) {
                r.Text("\n    ");
                Describe(r, value);
                ++found;
            }
        }
        r.Line();

        // Raw bytes for offline disassembly: the code around the fault, and the top of the stack
        // (a window procedure's hwnd / message / wParam / lParam sit just above its return address).
        // Eip - 0x40 cannot wrap for a real code address, but a fault at a low address would have
        // wrapped to nearly 4 GB and printed a base address that has nothing to do with the crash, so
        // the window is clamped rather than assumed.
        const DWORD base = c->Eip > 0x40 ? c->Eip - 0x40 : 0;
        r.Text("  code ");
        Describe(r, base);
        r.Text(" @");
        r.Hex(base, 8, true);
        r.Text(":");
        BYTE bytes[0x80];
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base), bytes, sizeof(bytes), nullptr)) {
            for (BYTE b : bytes) {
                r.Hex(b, 2);
                r.Char(' ');
            }
        } else {
            // The window straddles a page boundary, which is ordinary near the end of a function: one
            // read for the whole window fails, so fall back to per byte and keep what is there.
            for (int i = 0; i < 0x80; ++i) {
                BYTE b = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base + i), &b, 1, nullptr)) {
                    r.Hex(b, 2);
                    r.Char(' ');
                } else {
                    r.Text("?? ");
                }
            }
        }
        r.Line();

        r.Text("  esp words:");
        for (int i = 0; i < 32; ++i) {
            DWORD value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c->Esp + i * 4), &value, sizeof(value), nullptr))
                break;
            r.Text(" ");
            r.Hex(value, 8);
        }
        r.Line();

        // One frame, and it is a guess: [ebp] is a frame pointer only in code compiled that way, and
        // with MP2's own frames and /GS in the picture [ebp+4] is as often a local as a return
        // address. An unreadable Ebp (0, or a page we cannot read) leaves this line as zeros.
        DWORD frame[4]{};
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c->Ebp), frame, sizeof(frame), nullptr);
        r.Text("  [ebp] ");
        r.Hex(frame[0], 8);
        r.Text(" ret ");
        Describe(r, frame[1]);
        r.Text(" args ");
        r.Hex(frame[2], 8);
        r.Text(" ");
        r.Hex(frame[3], 8);
        r.Line();
        return EXCEPTION_CONTINUE_SEARCH;  // only record; MP2 and our own guards still handle it
    }
}

void crashlog::Install()
{
    AddVectoredExceptionHandler(1, Handler);  // first in chain: see it before anything can handle it
}
