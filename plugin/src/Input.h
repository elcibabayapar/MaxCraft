#pragma once

#include <Windows.h>

namespace input
{
    // Hooks DirectInput (keyboard and mouse device reads) so that, while Minecraft drives Max, the
    // keys and mouse go to Minecraft and MP2 only sees the few it keeps.
    bool Install();
    // Text input for Minecraft's chat and sign screens: subclasses the game window.
    void AttachWindow(HWND window);
    // Tells Minecraft to release every held key/button (input focus moved to MP2).
    void ReleaseAll();
}
