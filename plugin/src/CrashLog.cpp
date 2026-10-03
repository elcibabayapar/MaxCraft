#include "CrashLog.h"

#include "Log.h"

#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <string>

namespace
{
    std::atomic<int> g_logged{ 0 };

    // "module+offset" for an address, or the bare address.
    std::string Describe(DWORD address)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address),
                               &module) &&
            module) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            std::wstring name = path;
            name = name.substr(name.find_last_of(L'\\') + 1);
            return std::format("{}+{:#x}", mclog::Utf8(name), address - reinterpret_cast<DWORD>(module));
        }
        return std::format("{:#010x}", address);
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
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
            return EXCEPTION_CONTINUE_SEARCH;
        if (g_logged.fetch_add(1) >= 8)
            return EXCEPTION_CONTINUE_SEARCH;

        const CONTEXT* c = info->ContextRecord;
        const auto*    record = info->ExceptionRecord;
        mclog::Info("EXCEPTION {:#x} at {} ({} {:#010x}) thread {}", code, Describe(c->Eip),
                    record->NumberParameters >= 2 ? (record->ExceptionInformation[0] ? "writing" : "reading") : "?",
                    record->NumberParameters >= 2 ? static_cast<DWORD>(record->ExceptionInformation[1]) : 0, GetCurrentThreadId());
        mclog::Info("  eax {:08x} ebx {:08x} ecx {:08x} edx {:08x} esi {:08x} edi {:08x} ebp {:08x} esp {:08x}", c->Eax, c->Ebx, c->Ecx, c->Edx,
                    c->Esi, c->Edi, c->Ebp, c->Esp);
        // Return addresses on the stack: which calls led here (ours show up as MaxCraft.asi+...).
        std::string trail;
        int         found = 0;
        const auto* stack = reinterpret_cast<const DWORD*>(c->Esp);
        for (int i = 0; i < 512 && found < 16; ++i) {
            DWORD value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), stack + i, &value, sizeof(value), nullptr))
                break;
            if (value > 0x10000 && IsCode(value)) {
                trail += "\n    " + Describe(value);
                ++found;
            }
        }
        mclog::Info("  stack:{}", trail);
        return EXCEPTION_CONTINUE_SEARCH;  // only record; MP2 and our own guards still handle it
    }
}

void crashlog::Install()
{
    AddVectoredExceptionHandler(1, Handler);
}
