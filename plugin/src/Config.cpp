#include "Config.h"

#include <Windows.h>

#include <cwchar>

namespace
{
    std::wstring Read(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
    {
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
        return text.empty() ? fallback : static_cast<float>(_wtof(text.c_str()));
    }

    int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback)
    {
        const auto text = Read(ini, section, key, L"");
        return text.empty() ? fallback : static_cast<int>(wcstol(text.c_str(), nullptr, 0));
    }
}

std::wstring Config::GameDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

const Config& Config::Get()
{
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
        return c;
    }();
    return config;
}
