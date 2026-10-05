#include "Log.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <share.h>

namespace
{
    std::FILE* g_file      = nullptr;
    bool       g_noLogFile = false;  // already reported once; never try again
    std::mutex g_mutex;

    // Caller holds g_mutex.
    void WriteLocked(const char* line)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(g_file, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
        std::fflush(g_file);
    }
}

void mclog::Init()
{
    // Under the mutex and only once: crashlog's handler and the detours can call Write() from any
    // thread, and an unguarded g_file would let one of them read the pointer while fopen was still
    // writing it - or, worse, hand a half-initialised FILE* to fprintf.
    std::lock_guard lock(g_mutex);
    if (g_file || g_noLogFile)
        return;

    if (const char* value = std::getenv("MAXCRAFT_VERBOSE"))
        verbose.store(*value && *value != '0', std::memory_order_relaxed);

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"MaxCraft.log");
    g_file = _wfsopen(path, L"w", _SH_DENYNO);  // readable while the game runs
    if (!g_file) {
        // A game folder we may not write to (Program Files, or a read-only install) leaves every line
        // of this session invisible, so say so on the one channel that is still open and then stop
        // trying: a failed open per line would be a syscall storm on the game thread, and the mod has
        // to keep running either way. Windows Error Reporting's answer is "no faulting module".
        wchar_t reason[128]{};
        swprintf_s(reason, L"MaxCraft: MaxCraft.log could not be opened next to maxpayne2.exe (error %lu)\n", GetLastError());
        OutputDebugStringW(reason);
        g_noLogFile = true;
    }
}

void mclog::Write(const std::string& line)
{
    std::lock_guard lock(g_mutex);
    if (!g_file)
        return;
    WriteLocked(line.c_str());
}

void mclog::WriteRaw(const char* line)
{
    // try_lock, not lock_guard: this is the crash handler's path. A fault taken on a thread that is
    // already inside WriteLocked (a disk that went away mid-fprintf) must not deadlock the process on
    // a non-recursive mutex - and it must not spend the rest of the crash waiting for another
    // thread to finish a log line either. A lost crash line beats a hung game.
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !g_file)
        return;
    WriteLocked(line);
}

std::string mclog::Utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int   bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

bool mclog::Verbose()
{
    return verbose.load(std::memory_order_relaxed);
}
