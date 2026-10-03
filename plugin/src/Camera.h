#pragma once

#include "Engine.h"

namespace camera
{
    // Hooks MP2's camera so it sits at Minecraft's eye while Minecraft drives Max, and the level
    // init/deinit calls that tell us when a level comes and goes.
    bool Install();
    // A scripted camera path (cut-scene, level intro) is running: MP2 keeps the camera and Max.
    bool PathActive();
    // What MP2's camera follows this frame (X_CameraImplementation::update's target).
    const void* Target();
    // MP2's own render camera (camera-to-world, MP2 space) as of this frame, if known: blocks and
    // entities are drawn from it whenever Minecraft's eye isn't the camera (cut-scenes, MP2's view).
    bool RenderCamera(mp2::Matrix4x3& out);
}
