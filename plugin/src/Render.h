#pragma once

namespace render
{
    // Hooks Direct3D 8 (Present, Reset, SetTransform) through a throwaway device's vtable.
    // In Present, before the frame goes out: Minecraft's blocks drawn into MP2's scene against its
    // depth buffer, the targeted block's outline, then Minecraft's hand + GUI overlay on top.
    bool Install();
}
