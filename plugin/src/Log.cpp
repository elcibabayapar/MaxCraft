#include "Log.h"

#include <Windows.h>

#include <cstdio>
#include <mutex>

namespace
{
    std::FILE* g_file = nullptr;
    std::mutex g_mutex;
}

void mclog::Init()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"MaxCraft.log");
    _wfopen_s(&g_file, path, L"w");
}

void mclog::Write(const std::string& line)
{
    std::lock_guard lock(g_mutex);
    if (!g_file)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line.c_str());
    std::fflush(g_file);
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
