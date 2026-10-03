#pragma once

#include <string>

// MaxCraft.ini, next to maxpayne2.exe. Read once at startup; every value has a default.
struct Config
{
    // [Minecraft]
    bool         startWithGame = true;
    std::wstring launcher;                         // empty: the bundled SkyCraft Minecraft
    std::wstring arguments = L"--launch SkyCraft";

    // [World] how Max Payne 2's space maps onto Minecraft's.
    float unitsPerBlock = 0.0f;  // 0: measured from Max's collision capsule (1.8 blocks tall)
    int   upAxis = -1;           // 0 x, 1 y, 2 z; -1: from the capsule
    bool  flipZ = true;          // MaxFX is left-handed (Direct3D), Minecraft right-handed
    float feetOffset = 0.0f;     // MP2 units from the character origin down to the feet
    int   forwardRow = 2;        // which row of an MP2 transform is "forward"
    float gameHour = 12.0f;      // Minecraft's time of day (MP2 has no clock)

    // [Combat]
    float enemyDamageScale = 1.5f;   // Minecraft damage / 20 of an enemy's full health, times this
    float playerDamageScale = 1.0f;  // MP2 damage to Max as a fraction of his health -> Minecraft hearts

    // [Controls]
    int bulletTimeKey = 0x30;  // DIK_B: held -> MP2's bullet time (right mouse button)
    int quickSaveKey = 0x40;   // DIK_F6: -> MP2's quicksave (F5, which is Minecraft's camera key)

    // [Debug]
    bool diagnostics = false;
    bool blocksNoDepth = false;  // [Debug] bBlocksNoDepth: draw Minecraft's blocks over everything

    static const Config& Get();
    static std::wstring GameDir();
};
