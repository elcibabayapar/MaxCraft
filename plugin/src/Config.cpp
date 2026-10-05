#include "Config.h"

#include <Windows.h>

#include <cmath>
#include <cwchar>

namespace
{
    std::wstring Read(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
    {
        // nSize counts the terminator, so this holds 1023 characters at most and is always terminated.
        wchar_t buffer[1024]{};
        GetPrivateProfileStringW(section, key, fallback, buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
        // Allow trailing "; comment".
        std::wstring value = buffer;
        if (auto semi = value.find(L';'); semi != std::wstring::npos)
            value.erase(semi);
        while (!value.empty() && iswspace(value.back()))
            value.pop_back();
        return value;
    }

    float ReadFloat(const std::wstring& ini, const wchar_t* section, const wchar_t* key, float fallback)
    {
        const auto text = Read(ini, section, key, L"");
        if (text.empty())
            return fallback;
        // _wtof answers 0 for anything it cannot read, which would turn a typo into "no enemy damage"
        // or "no body behind the camera". Only a value that really parses may replace the default.
        wchar_t*       end    = nullptr;
        const double   value  = std::wcstod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(value))
            return fallback;
        return static_cast<float>(value);
    }

    int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback)
    {
        const auto text = Read(ini, section, key, L"");
        if (text.empty())
            return fallback;
        // Base 0 would read "0x30" as hex, but it also reads "030" as octal: in an ini full of
        // DirectInput scancodes nobody means octal, so hex is spelled 0x.. and the rest decimal.
        const bool hex = text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
        wchar_t*     end = nullptr;
        const long   value = wcstol(text.c_str(), &end, hex ? 16 : 10);
        // wcstol answers 0 for a string it cannot read at all, which would turn a typo'd scancode into
        // "key 0". Only a value that really starts with a digit may replace the default.
        if (end == text.c_str())
            return fallback;
        return static_cast<int>(value);
    }
}

std::wstring Config::GameDir()
{
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    // A path too long for MAX_PATH is truncated with no terminator, which would otherwise hand
    // substr() a path that runs into the rest of the buffer.
    path[len < MAX_PATH ? len : MAX_PATH - 1] = L'\0';
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

const Config& Config::Get()
{
    // A function-local static: the file is read once per process and the struct is const, so editing
    // MaxCraft.ini while MaxCraft is loaded does nothing until the next launch. That is deliberate.
    static const Config config = [] {
        Config c;
        const std::wstring ini = GameDir() + L"MaxCraft.ini";

        c.startWithGame = ReadInt(ini, L"Minecraft", L"bStartWithGame", 1) != 0;
        c.launcher = Read(ini, L"Minecraft", L"sLauncher", L"");
        c.arguments = Read(ini, L"Minecraft", L"sArguments", L"--launch SkyCraft");

        c.unitsPerBlock = ReadFloat(ini, L"World", L"fUnitsPerBlock", c.unitsPerBlock);
        c.upAxis = ReadInt(ini, L"World", L"iUpAxis", c.upAxis);
        c.flipZ = ReadInt(ini, L"World", L"bFlipZ", 1) != 0;
        c.feetOffset = ReadFloat(ini, L"World", L"fFeetOffset", c.feetOffset);
        c.forwardRow = ReadInt(ini, L"World", L"iForwardRow", c.forwardRow);
        c.gameHour = ReadFloat(ini, L"World", L"fGameHour", c.gameHour);
        c.bodyBehind = ReadFloat(ini, L"World", L"fBodyBehind", c.bodyBehind);
        c.moveRenderCamera = ReadInt(ini, L"World", L"bMoveRenderCamera", 1) != 0;

        c.enemyDamageScale = ReadFloat(ini, L"Combat", L"fEnemyDamageScale", c.enemyDamageScale);
        c.playerDamageScale = ReadFloat(ini, L"Combat", L"fPlayerDamageScale", c.playerDamageScale);

        c.bulletTimeKey = ReadInt(ini, L"Controls", L"iBulletTimeKey", c.bulletTimeKey);
        c.quickSaveKey = ReadInt(ini, L"Controls", L"iQuickSaveKey", c.quickSaveKey);
        c.weaponModeKey = ReadInt(ini, L"Controls", L"iWeaponModeKey", c.weaponModeKey);
        c.useKey = ReadInt(ini, L"Controls", L"iUseKey", c.useKey);

        c.diagnostics = ReadInt(ini, L"Debug", L"bDiagnostics", 0) != 0;
        c.blocksNoDepth = ReadInt(ini, L"Debug", L"bBlocksNoDepth", 0) != 0;
        c.blocksNoTexture = ReadInt(ini, L"Debug", L"bBlocksNoTexture", 0) != 0;
        c.drawAtEndScene = ReadInt(ini, L"Debug", L"bDrawAtEndScene", 0) != 0;
        c.noViewFixup = ReadInt(ini, L"Debug", L"bNoViewFixup", 0) != 0;
        c.noWvpFixup = ReadInt(ini, L"Debug", L"bNoWvpFixup", 0) != 0;
        c.noBlockDraw = ReadInt(ini, L"Debug", L"bNoBlockDraw", 0) != 0;
        return c;
    }();
    return config;
}
