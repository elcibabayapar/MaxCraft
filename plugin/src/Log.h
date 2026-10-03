#pragma once

#include <format>
#include <string>

namespace mclog
{
    // Opens MaxCraft.log next to maxpayne2.exe (truncated each launch).
    void Init();
    void Write(const std::string& line);
    // UTF-8 for log lines (std::filesystem::path::string() throws on characters outside the ANSI code page).
    std::string Utf8(const std::wstring& text);

    template <class... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        Write(std::format(fmt, std::forward<Args>(args)...));
    }
}
