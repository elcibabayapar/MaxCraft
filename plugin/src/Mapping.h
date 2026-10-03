#pragma once

// Max Payne 2 space <-> Minecraft blocks.
//
// The mapping is a scaled axis permutation: mc = (mp * B^T) / unitsPerBlock, with B built from the
// up axis and the handedness flip. It is calibrated once from Max's collision capsule (a person is
// 1.8 blocks tall) unless MaxCraft.ini pins it, and recalibrating bumps the collision epoch.

#include "Engine.h"

#include <cmath>

struct McPoint
{
    double x, y, z;
};

class Mapping
{
public:
    static Mapping& Get();

    // From Max's capsule end points and radius (any frame: only its length and axis are used).
    void Calibrate(const mp2::Vec3& a, const mp2::Vec3& b, float radius);
    [[nodiscard]] bool Calibrated() const { return calibrated_; }

    [[nodiscard]] McPoint   ToMc(const mp2::Vec3& p) const;
    [[nodiscard]] mp2::Vec3 ToMp2(double x, double y, double z) const;
    // Directions (no scale).
    [[nodiscard]] mp2::Vec3 DirToMp2(double x, double y, double z) const;
    [[nodiscard]] McPoint   DirToMc(const mp2::Vec3& d) const;

    [[nodiscard]] float UnitsPerBlock() const { return units_; }
    [[nodiscard]] int   UpAxis() const { return up_; }

    // Row-vector 4x4 taking Minecraft block coordinates (relative to origin) to MP2 world space.
    void McToMp2Matrix(double originX, double originY, double originZ, float out[4][4]) const;

    // MP2 transform -> Minecraft yaw (degrees, 0 = south, clockwise seen from above).
    [[nodiscard]] float YawOf(const mp2::Matrix4x3& m, int forwardRow) const;
    // A rotation for a character facing Minecraft yaw `yawDeg`, standing upright.
    void FacingYaw(float yawDeg, int forwardRow, mp2::Matrix4x3& m) const;

private:
    void Rebuild();

    bool  calibrated_{ false };
    float units_{ 1.0f };
    int   up_{ 1 };
    bool  flip_{ true };
    // mc[i] = sum_j axis_[i][j] * mp[j]   (orthonormal, det = +-1)
    float axis_[3][3]{};
};
