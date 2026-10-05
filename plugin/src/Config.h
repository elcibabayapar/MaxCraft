#pragma once

#include <string>

// MaxCraft.ini, next to maxpayne2.exe. Every value has a default, so a missing key or a value that
// does not parse still loads. Read exactly once per process (a function-local static, hence the
// "editing this needs a restart"): nothing else reads the file, and nothing writes to this struct.
struct Config
{
    // [Minecraft]
    bool         startWithGame = true;
    std::wstring launcher;                         // empty: the bundled SkyCraft Minecraft
    std::wstring arguments = L"--launch SkyCraft";

    // [World] how Max Payne 2's space maps onto Minecraft's.
    // > 0: MP2 units per Minecraft block, pinned by the ini. <= 0: measured from Max's collision
    // capsule, which is taken to be 1.8 blocks tall. Mapping.h spells the precedence out in full.
    float unitsPerBlock = 0.0f;
    int   upAxis = -1;           // 0 x, 1 y, 2 z: pinned by the ini; -1: measured from the capsule
    bool  flipZ = true;          // MaxFX is left-handed (Direct3D), Minecraft right-handed
    float feetOffset = 0.0f;     // MP2 units from the character origin down to the feet
    int   forwardRow = 2;        // which row of an MP2 transform is "forward"
    float gameHour = 12.0f;      // Minecraft's time of day (MP2 has no clock)
    float bodyBehind = 0.7f;     // first person: Max's body this many blocks behind the eye (out of view)
    bool  moveRenderCamera = true;  // move MP2's own render camera to Minecraft's eye (room visibility, effects)

    // [Combat]
    float enemyDamageScale = 1.5f;   // Minecraft damage / 20 of an enemy's full health, times this
    float playerDamageScale = 1.0f;  // MP2 damage to Max as a fraction of his health -> Minecraft hearts

    // [Controls]
    int bulletTimeKey = 0x30;  // DIK_B: held -> MP2's bullet time (right mouse button)
    int quickSaveKey = 0x40;   // DIK_F6: -> MP2's quicksave (F5, which is Minecraft's camera key)
    int weaponModeKey = 0x2F;  // DIK_V: toggles MP2 weapon mode (mouse buttons, wheel, 1-9 and R go to MP2's guns)
    int useKey = 0x22;         // DIK_G: MP2 receives it as E (its action key: doors, switches); E is Minecraft's inventory

    // [Debug]
    bool diagnostics = false;
    bool blocksNoDepth = false;    // draw Minecraft's blocks over everything
    bool blocksNoTexture = false;  // draw them untextured (vertex colour only): texture or transform?
    // TEMPORARY diagnostics for the menu-to-3D crash (Windows logs it as "unknown module,
    // 0x001aface"; nothing throws, so the vectored handler sees nothing). Each stops one render
    // path that hands MP2 a matrix of ours, so a user can tell which one is at fault:
    //   noViewFixup - pass SetTransform through (camera no longer follows Minecraft's eye)
    //   noWvpFixup  - pass SetVertexShaderConstant through (skinned characters keep MP2's own matrices)
    //   noBlockDraw - draw no Minecraft blocks at all
    // All three off = the mod is fully active. Each says itself once in MaxCraft.log.
    bool noViewFixup = false;
    bool noWvpFixup = false;
    bool noBlockDraw = false;
    // Which moment of MP2's frame the blocks go in (Render.cpp's BlocksDrawMode() is the only reader).
    // Every mode draws the same world pass from the same world camera; this only picks when, and the
    // depth buffer it lands in is what decides occlusion:
    //   0 (default) - the last moment MP2's depth buffer still holds the level: just before MP2
    //                 clears it for its weapon, else at the end of its 3D scene, else at Present.
    //   1           - only at the end of MP2's 3D scene, so the pass never waits on MP2 drawing a
    //                 weapon that frame. Correct occlusion either way.
    // BlocksNoDepth picks the third moment (always at Present, no occlusion) and stays a debug aid.
    bool drawAtEndScene = false;

    static const Config& Get();
    static std::wstring GameDir();
};
