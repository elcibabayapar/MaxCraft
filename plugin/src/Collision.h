#pragma once

#include "Mapping.h"

#include <cstdint>

// Streams Max Payne 2's level collision to Minecraft.
//
// MaxFX builds every room's collision as an X_HavokGeometry handed to
// X_RigidBodyRoom::allocateRigidBodyRoom(geometry, roomToWorld) while a level loads. We copy the
// triangles out of each one there (world space). Around the player, a worker thread then sends
// 8x8x8-block regions the way SkyCraft's mod expects them: the exact triangles (kColTris) for its
// smooth collider, then 1/8-block occupancy masks (kColRegion) for Minecraft's own collision.
namespace collision
{
    bool Install();
    // New epoch: Minecraft drops what it has; everything is sent again.
    void Reset(std::uint32_t epoch);
    // The level is unloading: forget its geometry.
    void ClearGeometry();
    // Game thread, every frame: queue the regions around the player that Minecraft doesn't have yet.
    void Update(const McPoint& player);
    // Whether a Minecraft position is inside the level: some of MP2's geometry stands in its column
    // (or a neighbouring one). Outside it MP2 kills Max, so Minecraft may not take him there.
    // True while the level's geometry isn't known yet.
    bool InsideLevel(const McPoint& p);
}
