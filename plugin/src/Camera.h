#pragma once

namespace camera
{
    // Hooks MP2's camera so it sits at Minecraft's eye while Minecraft drives Max, and the level
    // init/deinit calls that tell us when a level comes and goes.
    bool Install();
    // A scripted camera path (cut-scene, level intro) is running: MP2 keeps the camera and Max.
    bool PathActive();
    // What MP2's camera follows this frame (X_CameraImplementation::update's target).
    const void* Target();
}
