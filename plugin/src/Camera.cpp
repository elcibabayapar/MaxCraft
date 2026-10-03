#include "Camera.h"

#include "Characters.h"
#include "Config.h"
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

    enum class ViewconeMode
    {
        kUnknown,
        kCameraToWorld,  // rows: right, up, forward, position
        kNone,           // not a camera frame we recognise: left alone
    };
    ViewconeMode g_viewconeMode = ViewconeMode::kUnknown;
    // Where the render camera keeps its rotation, relative to its position field (in floats):
    // -9 when the object holds a 4x3 frame (rotation rows, then the position). 0: not found.
    int  g_rotationOffset = 0;
    bool g_rotationProbed = false;

    bool Orthonormal(const float* r)
    {
        for (int i = 0; i < 3; ++i) {
            const float len = r[i * 3] * r[i * 3] + r[i * 3 + 1] * r[i * 3 + 1] + r[i * 3 + 2] * r[i * 3 + 2];
            if (std::fabs(len - 1.0f) > 0.02f)
                return false;
            for (int j = i + 1; j < 3; ++j)
                if (std::fabs(r[i * 3] * r[j * 3] + r[i * 3 + 1] * r[j * 3 + 1] + r[i * 3 + 2] * r[j * 3 + 2]) > 0.02f)
                    return false;
        }
        return true;
    }

    bool              g_fovInRadians = true;

    void __fastcall Update(X_CameraImplementation* self, void*, const X_CameraTarget* target, const X_TimeUpdate& time)
    {
        g_origUpdate(self, target, time);
        g_target = target;
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
        g_viewconeMode = ViewconeMode::kUnknown;
        g_rotationProbed = false;
        g_rotationOffset = 0;
        collision::ClearGeometry();
        g_origDeinit(self);
    }

    // Minecraft's FOV is vertical; MaxFX's is taken as horizontal, in whatever unit it reports.
    float Distance(const Vec3& a, const Vec3& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    void MoveRenderCamera(X_LevelRuntimeCamera* self, const Matrix4x3& ours, bool log)
    {
        P_Camera* camera = nullptr;
        Guarded([&] { camera = api.getLevelCamera(self); });
        if (!camera)
            return;
        Guarded([&] {
            const Vec3&      pos = api.getObjectPosition(camera);
            const Matrix4x3& cone = api.getViewconeMatrix(camera);
            if (log) {
                mclog::Info("render camera: position ({:.2f}, {:.2f}, {:.2f}); viewcone rows ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f}) "
                            "({:.2f} {:.2f} {:.2f}); our eye ({:.2f}, {:.2f}, {:.2f}); mode {}",
                            pos.x, pos.y, pos.z, cone.row[0].x, cone.row[0].y, cone.row[0].z, cone.row[1].x, cone.row[1].y, cone.row[1].z, cone.row[2].x,
                            cone.row[2].y, cone.row[2].z, cone.row[3].x, cone.row[3].y, cone.row[3].z, ours.row[3].x, ours.row[3].y, ours.row[3].z,
                            static_cast<int>(g_viewconeMode));
            }
            if (g_viewconeMode == ViewconeMode::kUnknown) {
                // Recognise a camera-to-world frame by its translation being the camera's position.
                const bool camToWorld = Distance(cone.row[3], pos) < 0.05f && std::fabs(cone.row[0].x * cone.row[0].x + cone.row[0].y * cone.row[0].y +
                                                                                         cone.row[0].z * cone.row[0].z - 1.0f) < 0.05f;
                g_viewconeMode = camToWorld ? ViewconeMode::kCameraToWorld : ViewconeMode::kNone;
                mclog::Info("render camera: viewcone is {}", camToWorld ? "camera-to-world: following Minecraft's eye" : "not recognised: position only");
            }
            // The rotation: probe the floats around the position field once per level.
            auto* posField = const_cast<float*>(&pos.x);
            if (!g_rotationProbed) {
                g_rotationProbed = true;
                for (int offset : { -9, 3, -12, -13 }) {
                    if (Orthonormal(posField + offset)) {
                        g_rotationOffset = offset;
                        break;
                    }
                }
                const float* r = posField - 9;
                mclog::Info("render camera: floats before position ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}); after "
                            "({:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}); rotation {}",
                            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], posField[3], posField[4], posField[5], posField[6], posField[7],
                            posField[8], g_rotationOffset ? std::format("found at {:+d} floats: following Minecraft's look", g_rotationOffset)
                                                          : std::string("not found: position only"));
            }
            if (g_rotationOffset) {
                float* r = posField + g_rotationOffset;
                for (int i = 0; i < 3; ++i) {
                    r[i * 3 + 0] = ours.row[i].x;
                    r[i * 3 + 1] = ours.row[i].y;
                    r[i * 3 + 2] = ours.row[i].z;
                }
            }
            api.setObjectPosition(camera, ours.row[3]);
            // Everything MP2 derives from rotation + position (the copy right after the position, the
            // world-to-camera matrices) is rebuilt by MP2 itself, so no part of it keeps the old camera.
            api.calculateObjectToWorld(camera);
            if (g_viewconeMode == ViewconeMode::kCameraToWorld)
                const_cast<Matrix4x3&>(cone) = ours;
        });
    }

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
            static DWORD lastLog = 0;
            const bool   log = GetTickCount() - lastLog > 5000;
            if (log)
                lastLog = GetTickCount();
            if (Config::Get().moveRenderCamera)
                MoveRenderCamera(self, st.camera, log);
        } else if (g_savedFov > 0.0f) {
            const float saved = g_savedFov;
            Guarded([&] { api.setFOV(self, saved); });
            g_savedFov = -1.0f;
        }
        g_origVisibility(self);
        if (st.cameraValid && !g_pathActive && Config::Get().moveRenderCamera)
            MoveRenderCamera(self, st.camera, false);  // in case the call above refreshed it from MP2's own camera
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
