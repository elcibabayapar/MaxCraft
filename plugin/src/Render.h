#pragma once

namespace render
{
    // Hooks Direct3D 8 (Present, Reset, SetTransform) through a throwaway device's vtable.
    //
    // Minecraft's blocks are a WORLD render: drawn from the world camera, into MP2's own 3D scene,
    // while MP2's depth buffer still holds the level, so MP2's walls occlude them and the blocks
    // occlude MP2's walls. Where in the frame that happens is a choice (see Render.cpp's BlocksMode)
    // because MP2 wipes its depth buffer before drawing its weapon: at the end of the 3D scene and
    // immediately before that wipe both keep the world's depth, at Present there is none left and
    // the blocks can only be drawn over MP2's picture.
    //
    // In Present, before the frame goes out: whatever is left of that pass (at Present that is
    // always the overlay), then Minecraft's hand + GUI overlay on top.
    bool Install();
}
