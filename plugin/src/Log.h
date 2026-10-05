#pragma once

#include <atomic>
#include <format>
#include <string>

namespace mclog
{
    // Opens MaxCraft.log next to maxpayne2.exe (truncated each launch).
    void Init();
    void Write(const std::string& line);
    // One line that is already formatted and not owned: the crash handler's path, which cannot build
    // a std::string. Never blocks (it gives up rather than wait on the mutex - a fault taken while
    // this same thread is inside Write() would otherwise deadlock the game inside its own crash
    // logger), never allocates, never throws.
    void WriteRaw(const char* line);
    // UTF-8 for log lines (std::filesystem::path::string() throws on characters outside the ANSI code page).
    std::string Utf8(const std::wstring& text);

    // Periodic status lines only matter while a bug is being chased, so they are off unless
    // MAXCRAFT_VERBOSE is set in the environment (any value but 0). Init() reads it once per session,
    // so a disabled switch costs one getenv and a relaxed atomic load per line. MaxCraft.ini cannot
    // switch it: Config owns that file and parses it once per process.
    inline std::atomic<bool> verbose{ false };
    bool                    Verbose();

    // Info is the only variadic entry point that has to be checked, and it is: std::format_string
    // rejects a literal whose braces don't match its arguments at compile time. WriteRaw takes a
    // finished string and Verbose below takes the same checked format string, so every entry point
    // that formats is checked.
    template <class... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        Write(std::format(fmt, std::forward<Args>(args)...));
    }

    // A line that is only worth having while something is wrong. The arguments are not evaluated
    // when the switch is off, which is what makes this free rather than cheap: a status line every
    // frame would otherwise cost a std::format and a heap block each time.
    template <class... Args>
    void Verbose(std::format_string<Args...> fmt, Args&&... args)
    {
        if (verbose.load(std::memory_order_relaxed))
            Write(std::format(fmt, std::forward<Args>(args)...));
    }
}
