#pragma once

namespace launcher
{
    // Starts Minecraft with the game (per MaxCraft.ini) unless one with SkyCraft's mod is running.
    //
    // Default: the Minecraft SkyCraft ships (portable Prism Launcher + a ready "SkyCraft" instance),
    // from MaxCraft\SkyCraft-Minecraft.zip in the game folder, unpacked to %LOCALAPPDATA%\SkyCraft the
    // same way SkyCraft's own plugin does it (so a player who has both shares one install).
    void StartMinecraft();
}
