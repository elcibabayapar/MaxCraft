#include "Mapping.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>

namespace
{
    constexpr float kPlayerHeightBlocks = 1.8f;
    constexpr double kRadToDeg = 57.29577951308232;
    constexpr double kDegToRad = 0.017453292519943295;
}

Mapping& Mapping::Get()
{
    static Mapping mapping = [] {
        Mapping m;
        const auto& cfg = Config::Get();
        m.flip_ = cfg.flipZ;
        if (cfg.upAxis >= 0 && cfg.upAxis <= 2)
            m.up_ = cfg.upAxis;
        m.units_ = cfg.unitsPerBlock > 0.0f ? cfg.unitsPerBlock : 1.0f;
        m.calibrated_ = cfg.upAxis >= 0;
        m.Rebuild();
        return m;
    }();
    return mapping;
}

void Mapping::Rebuild()
{
    // A cyclic permutation (det +1) that puts MP2's up axis on Minecraft Y: identity for Y up,
    // (y, z, x) for Z up, (z, x, y) for X up. Minecraft Z is then negated when MP2 is left-handed,
    // so a Direct3D-style Y-up left-handed world maps to (x, y, -z).
    std::fill(&axis_[0][0], &axis_[0][0] + 9, 0.0f);
    axis_[0][(up_ + 2) % 3] = 1.0f;
    axis_[1][up_] = 1.0f;
    axis_[2][(up_ + 1) % 3] = flip_ ? -1.0f : 1.0f;
}

void Mapping::Calibrate(const mp2::Vec3& a, const mp2::Vec3& b, float radius)
{
    if (calibrated_)
        return;
    const auto& cfg = Config::Get();
    const float d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float height = length + 2.0f * radius;
    if (!(height > 0.0f) || !std::isfinite(height)) {
        mclog::Info("calibration: capsule unusable (len {}, r {})", length, radius);
        return;
    }
    if (cfg.upAxis < 0) {
        up_ = 0;
        for (int i = 1; i < 3; ++i)
            if (std::fabs(d[i]) > std::fabs(d[up_]))
                up_ = i;
    }
    units_ = cfg.unitsPerBlock > 0.0f ? cfg.unitsPerBlock : 1.0f;  // MaxFX units are metres
    (void)kPlayerHeightBlocks;
    calibrated_ = true;
    Rebuild();
    mclog::Info("calibration: capsule {:.3f} tall (len {:.3f}, r {:.3f}) -> {:.4f} units per block, up axis {}, flipZ {}",
                height, length, radius, units_, "xyz"[up_], flip_);
}

McPoint Mapping::ToMc(const mp2::Vec3& p) const
{
    const float v[3] = { p.x, p.y, p.z };
    double      r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = (axis_[i][0] * v[0] + axis_[i][1] * v[1] + axis_[i][2] * v[2]) / units_;
    return { r[0], r[1], r[2] };
}

mp2::Vec3 Mapping::ToMp2(double x, double y, double z) const
{
    const mp2::Vec3 d = DirToMp2(x, y, z);
    return { d.x * units_, d.y * units_, d.z * units_ };
}

mp2::Vec3 Mapping::DirToMp2(double x, double y, double z) const
{
    // Inverse of an orthonormal axis matrix is its transpose.
    const double v[3] = { x, y, z };
    float        r[3];
    for (int j = 0; j < 3; ++j)
        r[j] = static_cast<float>(axis_[0][j] * v[0] + axis_[1][j] * v[1] + axis_[2][j] * v[2]);
    return { r[0], r[1], r[2] };
}

McPoint Mapping::DirToMc(const mp2::Vec3& d) const
{
    McPoint p = ToMc(d);
    return { p.x * units_, p.y * units_, p.z * units_ };
}

void Mapping::McToMp2Matrix(double originX, double originY, double originZ, float out[4][4]) const
{
    // Row i is the MP2 image of Minecraft basis vector i, scaled to MP2 units.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            out[i][j] = axis_[i][j] * units_;
        out[i][3] = 0.0f;
    }
    const mp2::Vec3 o = ToMp2(originX, originY, originZ);
    out[3][0] = o.x;
    out[3][1] = o.y;
    out[3][2] = o.z;
    out[3][3] = 1.0f;
}

float Mapping::YawOf(const mp2::Matrix4x3& m, int forwardRow) const
{
    const McPoint f = DirToMc(m.row[std::clamp(forwardRow, 0, 2)]);
    // Minecraft: yaw 0 faces +Z, 90 faces -X.
    return static_cast<float>(std::atan2(-f.x, f.z) * kRadToDeg);
}

void Mapping::FacingYaw(float yawDeg, int forwardRow, mp2::Matrix4x3& m) const
{
    const double    yaw = yawDeg * kDegToRad;
    const mp2::Vec3 forward = DirToMp2(-std::sin(yaw), 0.0, std::cos(yaw));
    const mp2::Vec3 up = DirToMp2(0.0, 1.0, 0.0);
    // Minecraft's right-hand side for that facing, mapped across: correct in either handedness.
    const mp2::Vec3 right = DirToMp2(-std::cos(yaw), 0.0, -std::sin(yaw));

    // MaxFX transforms are (right, up, forward) by default; iForwardRow moves "forward".
    const int fRow = std::clamp(forwardRow, 0, 2);
    const int uRow = fRow == 1 ? 2 : 1;
    const int rRow = 3 - fRow - uRow;
    m.row[fRow] = forward;
    m.row[uRow] = up;
    m.row[rRow] = right;
}
