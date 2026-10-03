#pragma once

#include <cstdint>

namespace characters
{
    // Hooks X_Character's per-frame update, physics and damage.
    bool Install();
    // A level is going away: forget every character and stream collision from a new epoch.
    void OnLevelChange();
    // Present hook, every rendered frame (also while MP2 is paused): heartbeat + SkyState.
    void OnPresent(std::uint32_t width, std::uint32_t height);
}
