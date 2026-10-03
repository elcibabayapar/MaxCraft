#include "Launcher.h"

#include "Config.h"
#include "Log.h"

#include <Windows.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    std::wstring ExpandEnv(const std::wstring& text)
    {
        wchar_t buffer[MAX_PATH * 2]{};
        return ExpandEnvironmentStringsW(text.c_str(), buffer, static_cast<DWORD>(std::size(buffer))) ? buffer : text;
    }

    // SkyCraft's Fabric mod holds this mutex while it runs (SkyLink.announceRunning).
    bool MinecraftRunning()
    {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\SkyCraft_v1_minecraft");
        if (mutex) {
            CloseHandle(mutex);
            return true;
        }
        return false;
    }

    fs::path Bundle() { return fs::path(Config::GameDir()) / L"MaxCraft" / L"SkyCraft-Minecraft.zip"; }
    fs::path InstallDir() { return ExpandEnv(L"%LOCALAPPDATA%\\SkyCraft"); }

    // Unpacks the bundle the first time and whenever a different one is shipped. Prism's own data
    // (the signed-in account, downloaded Minecraft and Java, the world) is kept.
    fs::path EnsureBundle()
    {
        const fs::path  dir = InstallDir();
        const fs::path  prism = dir / L"Prism" / L"prismlauncher.exe";
        std::error_code ec;
        const auto      stamp = std::format("{} {}", fs::file_size(Bundle(), ec), fs::last_write_time(Bundle(), ec).time_since_epoch().count());
        std::string     installed;
        if (std::ifstream in{ dir / L"bundle.stamp" }; in)
            std::getline(in, installed);
        if (installed == stamp && fs::exists(prism))
            return prism;

        mclog::Info("Minecraft: unpacking {} to {}", mclog::Utf8(Bundle().wstring()), mclog::Utf8(dir.wstring()));
        fs::create_directories(dir, ec);
        for (const auto& entry : fs::directory_iterator(dir / L"Prism" / L"instances" / L"SkyCraft" / L".minecraft" / L"mods", ec)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("skycraft-") || name.starts_with("fabric-api-") || name.starts_with("e4mc-"))
                fs::remove(entry.path(), ec);
        }
        std::wstring command = L"\"" + ExpandEnv(L"%SystemRoot%\\System32\\tar.exe") + L"\" -xf \"" + Bundle().wstring() + L"\" -C \"" + dir.wstring() + L"\"";
        STARTUPINFOW        si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        DWORD               code = 1;
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (code != 0 || !fs::exists(prism)) {
            mclog::Info("Minecraft: unpacking failed (tar exit code {})", code);
            return {};
        }
        const fs::path cfg = dir / L"Prism" / L"prismlauncher.cfg";
        if (!fs::exists(cfg))
            fs::copy_file(dir / L"defaults" / L"prismlauncher.cfg", cfg, ec);
        std::ofstream(dir / L"bundle.stamp") << stamp;
        return prism;
    }

    bool Start(const fs::path& program, const std::wstring& args)
    {
        const auto   ext = program.extension().wstring();
        const bool   script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
        std::wstring command = script ? L"cmd.exe /c \"\"" + program.wstring() + L"\" " + args + L"\"" : L"\"" + program.wstring() + L"\" " + args;
        const std::wstring  dir = program.parent_path().wstring();
        STARTUPINFOW        si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, script ? CREATE_NO_WINDOW : 0, nullptr, dir.c_str(), &si, &pi)) {
            mclog::Info("Minecraft: couldn't start {} (error {})", mclog::Utf8(program.wstring()), GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        mclog::Info("Minecraft: started {} {}", mclog::Utf8(program.wstring()), mclog::Utf8(args));
        return true;
    }
}

void launcher::StartMinecraft()
{
    const auto& cfg = Config::Get();
    if (!cfg.startWithGame) {
        mclog::Info("Minecraft: not started with the game (bStartWithGame = 0)");
        return;
    }
    if (MinecraftRunning()) {
        mclog::Info("Minecraft: already running");
        return;
    }
    const fs::path chosen = ExpandEnv(cfg.launcher);
    const bool     bundled = cfg.launcher.empty() && fs::exists(Bundle());
    if (cfg.launcher.empty() && !bundled) {
        mclog::Info("Minecraft: not started: no {} and no sLauncher in MaxCraft.ini", mclog::Utf8(Bundle().wstring()));
        return;
    }
    if (!cfg.launcher.empty() && !fs::exists(chosen)) {
        mclog::Info("Minecraft: not started: {} doesn't exist (sLauncher)", mclog::Utf8(chosen.wstring()));
        return;
    }
    std::thread([chosen, bundled, args = cfg.arguments] {
        fs::path program = chosen;
        if (bundled) {
            program = EnsureBundle();
            if (program.empty())
                return;
            if (!fs::exists(program.parent_path() / L"accounts.json"))
                mclog::Info("Minecraft: first start: sign in to your Microsoft account in the Prism window (Alt-Tab)");
        }
        Start(program, args);
    }).detach();
}
