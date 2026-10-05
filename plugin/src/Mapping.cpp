#include "Mapping.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Max's collision capsule is the only ruler MaxCraft has, so the scale it measures is anchored to
    // how tall a person is in Minecraft.
    constexpr float kPlayerHeightBlocks = 1.8f;
    // One unit per block is "MaxFX units are metres", which is what MP2 really is and what anything
    // unusable falls back to. Outside this range Max is 0.45 or 7.2 blocks tall, which is a broken
    // capsule read rather than a world worth streaming: say so loudly instead of building one.
    constexpr float kMinUnitsPerBlock = 0.25f;
    constexpr float kMaxUnitsPerBlock = 4.0f;
    constexpr double kRadToDeg = 57.29577951308232;
    constexpr double kDegToRad = 0.017453292519943295;

    // iUpAxis: 0 x, 1 y, 2 z pin the axis, -1 measures it. 0 is a real axis (X up), not "unset".
    bool UpAxisPinned(int cfgAxis)
    {
        return cfgAxis >= 0 && cfgAxis <= 2;
    }

    bool SaneUnitsPerBlock(float units)
    {
        return std::isfinite(units) && units >= kMinUnitsPerBlock && units <= kMaxUnitsPerBlock;
    }
}

Mapping& Mapping::Get()
{
    static Mapping mapping = [] {
        Mapping m;
        const auto& cfg = Config::Get();
        m.flip_ = cfg.flipZ;

        // Out of range has to behave like a negative value and be logged, not slip through as a
        // decision: only 0, 1 or 2 may pin the axis, and calibrated_ below asks that question twice.
        m.upPinned_ = UpAxisPinned(cfg.upAxis);
        if (m.upPinned_) {
            m.up_ = cfg.upAxis;
        } else if (cfg.upAxis > 2) {
            mclog::Info("config: iUpAxis {} is not 0, 1 or 2; measuring it from Max's capsule", cfg.upAxis);
        }

        // fUnitsPerBlock > 0 is an explicit override and outranks the capsule; <= 0 asks for the
        // measurement. units_ holds the fallback until Calibrate() has a capsule to measure with.
        m.unitsPinned_ = cfg.unitsPerBlock > 0.0f;
        m.units_ = m.unitsPinned_ ? cfg.unitsPerBlock : 1.0f;  // MaxFX units are metres
        if (m.unitsPinned_ && !SaneUnitsPerBlock(m.units_))
            mclog::Info("config: fUnitsPerBlock {:.4f} is outside the {:.2f}..{:.2f} a person can give; using it anyway",
                        m.units_, kMinUnitsPerBlock, kMaxUnitsPerBlock);

        // Only a mapping with both halves decided needs no capsule. Anything else waits for one, and
        // the collision epoch the caller bumps after it covers the scale moving under it.
        m.calibrated_ = m.upPinned_ && m.unitsPinned_;
        m.Rebuild();
        if (m.calibrated_)
            mclog::Info("mapping: pinned by MaxCraft.ini (up axis {}, {:.4f} MP2 units per block, flipZ {})",
                        "xyz"[m.up_], m.units_, m.flip_);
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

bool Mapping::Calibrate(const mp2::Vec3& a, const mp2::Vec3& b, float radius)
{
    if (calibrated_)
        return true;
    const float d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float height = length + 2.0f * radius;
    if (!std::isfinite(height) || !(height > 0.0f)) {
        // A rigid body that is not built yet looks exactly like this, so it is not fatal and the
        // caller comes back next frame. Report it once, or a Max without a capsule floods the log.
        static bool reported = false;
        if (!reported) {
            reported = true;
            mclog::Info("calibration: capsule unusable (len {}, r {}); retrying", length, radius);
        }
        return false;
    }
    if (!upPinned_) {
        up_ = 0;
        for (int i = 1; i < 3; ++i)
            if (std::fabs(d[i]) > std::fabs(d[up_]))
                up_ = i;
    }

    // The capsule is the ruler: one MP2 unit is its height / 1.8 blocks, which is the whole point of
    // reading it. Only fUnitsPerBlock > 0 outranks that, and a capsule that cannot be a person is
    // thrown away rather than allowed to move the world.
    const float  measured = height / kPlayerHeightBlocks;
    const char* source = "auto, measured from the capsule";
    if (unitsPinned_) {
        source = "manual, pinned by fUnitsPerBlock";
    } else if (!SaneUnitsPerBlock(measured)) {
        source = "auto, capsule rejected: kept MaxFX's metres";
    } else {
        units_ = measured;
    }

    calibrated_ = true;
    Rebuild();
    mclog::Info("calibration: capsule {:.3f} tall (len {:.3f}, r {:.3f}), up axis {}, {:.4f} MP2 units per block measured",
                height, length, radius, "xyz"[up_], measured);
    mclog::Info("calibration: scale in use {:.4f} MP2 units per block ({})", units_, source);
    return true;
}

McPoint Mapping::ToMc(const mp2::Vec3& p) const
{
    // Accumulated in double: MP2 coordinates run to thousands of metres and units_ divides them, so
    // rounding a three-term dot product in float shows up as block jitter.
    const double v[3] = { p.x, p.y, p.z };
    double       r[3];
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
    // Directions (no scale): the same matrix, applied straight. ToMc() would divide by units_ and
    // this would have to multiply it back, rounding a unit vector twice for an answer we already have.
    const double v[3] = { d.x, d.y, d.z };
    double       r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = axis_[i][0] * v[0] + axis_[i][1] * v[1] + axis_[i][2] * v[2];
    return { r[0], r[1], r[2] };
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
