#pragma once

#include <cstdint>

namespace characters
{
    // No player update for this long: MP2 is paused, in a menu or loading. One number for every
    // decision that depends on the game still ticking (who owns the input, what SkyState says, how
    // long MP2's own render camera stays usable), because two thresholds for one question make the
    // views disagree about whether the game is alive.
    inline constexpr std::uint32_t kPausedMs = 250;

    // Hooks X_Character's per-frame update, physics and damage.
    bool Install();
    // A level has started or gone away (camera.cpp calls this from initLevel and deinitLevel): forget
    // every character and stream collision from a new epoch.
    void OnLevelChange();
    // Present hook, every rendered frame (also while MP2 is paused): heartbeat + SkyState.
    void OnPresent(std::uint32_t width, std::uint32_t height);
}
