#include "Camera.h"

#include "Characters.h"
#include "Collision.h"
#include "Hook.h"
#include "Log.h"
#include "Runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace mp2;

namespace
{
    using UpdateFn = void(__thiscall*)(X_CameraImplementation*, const X_CameraTarget*, const X_TimeUpdate&);
    using MatrixFn = const Matrix4x3&(__thiscall*)(const X_CameraImplementation*);
    using InitLevelFn = void(__thiscall*)(X_CameraImplementation*, const X_LevelRuntimeRoomContainer*);
    using DeinitLevelFn = void(__thiscall*)(X_CameraImplementation*);
    using VisibilityFn = void(__thiscall*)(X_LevelRuntimeCamera*);

    UpdateFn      g_origUpdate = nullptr;
    MatrixFn      g_origMatrix = nullptr;
    InitLevelFn   g_origInit = nullptr;
    DeinitLevelFn g_origDeinit = nullptr;
    VisibilityFn  g_origVisibility = nullptr;

    std::atomic<bool>        g_pathActive{ false };
    std::atomic<const void*> g_target{ nullptr };
    float             g_savedFov = -1.0f;

    // MP2's own camera this frame (captured before we replace it), and every place in its camera
    // objects that holds a copy of it: MP2's renderer reads the camera from there, not from
    // X_CameraImplementation, so those copies get Minecraft's eye written over them.
    Matrix4x3 g_mp2Camera{};
    bool      g_haveMp2Camera = false;

    enum PatchKind
    {
        kRot3,      // 9 floats: rows 0-2
        kMat43,     // 12 floats: rows 0-3
        kRot4,      // rows 0-2 with a 4-float stride (4x4 matrix)
        kMat44,     // rows 0-3 with a 4-float stride
        kPosition,  // 3 floats: row 3
    };
    struct Patch
    {
        std::uint8_t* at;
        PatchKind     kind;
        bool          inverse;  // holds world-to-camera instead of camera-to-world
    };
    std::vector<Patch> g_patches;
    const void*        g_scannedCamera = nullptr;
    int                g_rescanFrames = 0;
    bool               g_reportedNone = false;

    Matrix4x3 Inverse(const Matrix4x3& m)
    {
        Matrix4x3 r{};
        const float a[3][3] = { { m.row[0].x, m.row[0].y, m.row[0].z }, { m.row[1].x, m.row[1].y, m.row[1].z }, { m.row[2].x, m.row[2].y, m.row[2].z } };
        float* out[3] = { &r.row[0].x, &r.row[1].x, &r.row[2].x };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i][j] = a[j][i];
        const float t[3] = { m.row[3].x, m.row[3].y, m.row[3].z };
        float*      ti = &r.row[3].x;
        for (int j = 0; j < 3; ++j)
            ti[j] = -(t[0] * out[0][j] + t[1] * out[1][j] + t[2] * out[2][j]);
        return r;
    }

    bool Near(float a, float b)
    {
        return std::fabs(a - b) <= 2e-3f * (std::max)(1.0f, std::fabs(b));
    }

    void ScanObject(std::uint8_t* base, std::size_t bytes, const Matrix4x3& camera, bool inverse)
    {
        const float* ref = &camera.row[0].x;  // 12 floats
        const bool   ok = Guarded([&] {
            std::size_t skipUntil = 0;
            for (std::size_t o = 0; o + 64 <= bytes; o += 4) {
                const float* f = reinterpret_cast<const float*>(base + o);
                bool rot3 = true, rot4 = true;
                for (int i = 0; i < 9 && rot3; ++i)
                    rot3 = Near(f[i], ref[i]);
                for (int r = 0; r < 3 && rot4; ++r)
                    for (int c = 0; c < 3 && rot4; ++c)
                        rot4 = Near(f[r * 4 + c], ref[r * 3 + c]);
                if (rot3) {
                    const bool full = Near(f[9], ref[9]) && Near(f[10], ref[10]) && Near(f[11], ref[11]);
                    g_patches.push_back({ base + o, full ? kMat43 : kRot3, inverse });
                    skipUntil = o + (full ? 48 : 36);
                } else if (rot4) {
                    const bool full = Near(f[12], ref[9]) && Near(f[13], ref[10]) && Near(f[14], ref[11]);
                    g_patches.push_back({ base + o, full ? kMat44 : kRot4, inverse });
                    skipUntil = o + (full ? 60 : 44);
                } else if (o >= skipUntil && !inverse && Near(f[0], ref[9]) && Near(f[1], ref[10]) && Near(f[2], ref[11]) &&
                           (std::fabs(ref[9]) + std::fabs(ref[10]) + std::fabs(ref[11])) > 0.5f) {
                    g_patches.push_back({ base + o, kPosition, false });
                    skipUntil = o + 12;
                }
            }
        });
        if (!ok)
            mclog::Info("camera scan: memory at {:p} unreadable", static_cast<void*>(base));
    }

    void Scan(X_LevelRuntimeCamera* levelCamera)
    {
        P_Camera* camera = nullptr;
        Guarded([&] { camera = api.getLevelCamera(levelCamera); });
        if (!camera || !g_haveMp2Camera)
            return;
        if (camera == g_scannedCamera && !g_patches.empty())
            return;
        if (camera == g_scannedCamera && g_rescanFrames-- > 0)
            return;
        g_scannedCamera = camera;
        g_rescanFrames = 120;
        g_patches.clear();

        const Matrix4x3 inverse = Inverse(g_mp2Camera);
        for (auto* base : { reinterpret_cast<std::uint8_t*>(camera), reinterpret_cast<std::uint8_t*>(levelCamera) }) {
            ScanObject(base, 0x600, g_mp2Camera, false);
            ScanObject(base, 0x600, inverse, true);
        }
        const auto& m = g_mp2Camera;
        if (g_patches.empty()) {
            if (!g_reportedNone)
                mclog::Info("camera scan: MP2's camera matrix not found in P_Camera {:p} / X_LevelRuntimeCamera {:p}; MP2 camera rows "
                            "({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) pos ({:.2f} {:.2f} {:.2f})",
                            static_cast<void*>(camera), static_cast<void*>(levelCamera), m.row[0].x, m.row[0].y, m.row[0].z, m.row[1].x,
                            m.row[1].y, m.row[1].z, m.row[2].x, m.row[2].y, m.row[2].z, m.row[3].x, m.row[3].y, m.row[3].z);
            g_reportedNone = true;
            return;
        }
        for (const auto& p : g_patches) {
            const bool inCamera = p.at >= reinterpret_cast<std::uint8_t*>(camera) && p.at < reinterpret_cast<std::uint8_t*>(camera) + 0x600;
            static constexpr const char* kKinds[] = { "rot3", "mat4x3", "rot4", "mat4x4", "position" };
            mclog::Info("camera scan: {} +{:#x}: {}{}", inCamera ? "P_Camera" : "X_LevelRuntimeCamera",
                        p.at - (inCamera ? reinterpret_cast<std::uint8_t*>(camera) : reinterpret_cast<std::uint8_t*>(levelCamera)), kKinds[p.kind],
                        p.inverse ? " (world-to-camera)" : "");
        }
    }

    void ApplyPatches(const Matrix4x3& ours)
    {
        if (g_patches.empty())
            return;
        const Matrix4x3 inverse = Inverse(ours);
        Guarded([&] {
            for (const auto& p : g_patches) {
                const float* src = p.inverse ? &inverse.row[0].x : &ours.row[0].x;
                auto*        dst = reinterpret_cast<float*>(p.at);
                switch (p.kind) {
                case kRot3:
                    std::copy(src, src + 9, dst);
                    break;
                case kMat43:
                    std::copy(src, src + 12, dst);
                    break;
                case kRot4:
                case kMat44:
                    for (int r = 0; r < (p.kind == kMat44 ? 4 : 3); ++r)
                        std::copy(src + r * 3, src + r * 3 + 3, dst + r * 4);
                    break;
                case kPosition:
                    std::copy(src + 9, src + 12, dst);
                    break;
                }
            }
        });
    }
    bool              g_fovInRadians = true;

    void __fastcall Update(X_CameraImplementation* self, void*, const X_CameraTarget* target, const X_TimeUpdate& time)
    {
        g_origUpdate(self, target, time);
        g_target = target;
        g_haveMp2Camera = Guarded([&] { g_mp2Camera = g_origMatrix(self); });
        bool path = false;
        Guarded([&] { path = api.isCameraPathActive(self); });
        g_pathActive = path;

        // Overwrite the matrix MP2 just computed, so whatever reads it (renderer, sound listener,
        // visibility) sees Minecraft's eye.
        auto& st = State();
        if (st.cameraValid && !path) {
            Guarded([&] { const_cast<Matrix4x3&>(g_origMatrix(self)) = st.camera; });
        }
    }

    const Matrix4x3& __fastcall CurrentMatrix(const X_CameraImplementation* self, void*)
    {
        auto& st = State();
        if (st.cameraValid && !g_pathActive)
            return st.camera;
        return g_origMatrix(self);
    }

    void __fastcall InitLevel(X_CameraImplementation* self, void*, const X_LevelRuntimeRoomContainer* rooms)
    {
        mclog::Info("level init");
        g_origInit(self, rooms);
    }

    void __fastcall DeinitLevel(X_CameraImplementation* self, void*)
    {
        mclog::Info("level deinit");
        characters::OnLevelChange();
        g_patches.clear();
        g_scannedCamera = nullptr;
        g_reportedNone = false;
        collision::ClearGeometry();
        g_origDeinit(self);
    }

    // Minecraft's FOV is vertical; MaxFX's is taken as horizontal, in whatever unit it reports.
    void __fastcall UpdateVisibility(X_LevelRuntimeCamera* self, void*)
    {
        auto& st = State();
        if (st.cameraValid && !g_pathActive) {
            if (g_savedFov < 0.0f) {
                Guarded([&] { g_savedFov = api.getFOV(self); });
                g_fovInRadians = g_savedFov > 0.0f && g_savedFov < 3.2f;
                mclog::Info("MP2 FOV {:.3f} ({})", g_savedFov, g_fovInRadians ? "radians" : "degrees");
            }
            const double aspect = st.viewportH > 0 ? double(st.viewportW) / st.viewportH : 16.0 / 9.0;
            const double v = st.fovDeg * 0.017453292519943295;
            const double h = 2.0 * std::atan(std::tan(v / 2.0) * aspect);
            const float  fov = static_cast<float>(g_fovInRadians ? h : h * 57.29577951308232);
            Guarded([&] { api.setFOV(self, fov); });
            Scan(self);
            ApplyPatches(st.camera);
        } else if (g_savedFov > 0.0f) {
            const float saved = g_savedFov;
            Guarded([&] { api.setFOV(self, saved); });
            g_savedFov = -1.0f;
        }
        g_origVisibility(self);
        if (st.cameraValid && !g_pathActive)
            ApplyPatches(st.camera);  // in case the call above refreshed them from MP2's own camera
    }
}

bool camera::Install()
{
    bool ok = true;
    ok &= InstallHook(api.cameraUpdate, reinterpret_cast<void*>(&Update), g_origUpdate, "X_CameraImplementation::update");
    ok &= InstallHook(api.cameraGetCurrentMatrix, reinterpret_cast<void*>(&CurrentMatrix), g_origMatrix, "X_CameraImplementation::getCurrentMatrix");
    ok &= InstallHook(api.cameraInitLevel, reinterpret_cast<void*>(&InitLevel), g_origInit, "X_CameraImplementation::initLevel");
    ok &= InstallHook(api.cameraDeinitLevel, reinterpret_cast<void*>(&DeinitLevel), g_origDeinit, "X_CameraImplementation::deinitLevel");
    ok &= InstallHook(api.levelCameraUpdateVisibility, reinterpret_cast<void*>(&UpdateVisibility), g_origVisibility, "X_LevelRuntimeCamera::updateVisibility");
    return ok;
}

bool camera::PathActive()
{
    return g_pathActive;
}

const void* camera::Target()
{
    return g_target;
}
