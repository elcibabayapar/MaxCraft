#include "Render.h"

#include "Characters.h"
#include "Config.h"
#include "D3D8.h"
#include "Hook.h"
#include "Input.h"
#include "Launcher.h"
#include "Log.h"
#include "Mapping.h"
#include "Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace d3d8;

namespace
{
    using PresentFn = HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*);
    using ResetFn = HRESULT(__stdcall*)(void*, PresentParameters*);
    using SetTransformFn = HRESULT(__stdcall*)(void*, UINT, const Matrix*);
    using BeginSceneFn = HRESULT(__stdcall*)(void*);
    using EndSceneFn = HRESULT(__stdcall*)(void*);
    PresentFn      g_origPresent = nullptr;
    ResetFn        g_origReset = nullptr;
    SetTransformFn g_origSetTransform = nullptr;
    BeginSceneFn   g_origBeginScene = nullptr;
    EndSceneFn     g_origEndScene = nullptr;

    // MP2's own camera matrices, as it last set them: our blocks use exactly the same ones, so they
    // line up with its picture and depth buffer.
    Matrix g_view{}, g_proj{};
    bool   g_haveView = false, g_haveProj = false;
    UINT   g_viewSets = 0, g_viewReplaced = 0, g_worldCorrected = 0;  // per 5 s, for the log
    DWORD  g_sceneZEnable = 1;  // D3DRS_ZENABLE as MP2's 3D scene set it (1 z-buffer, 2 w-buffer)
    UINT   g_trianglesDrawn = 0;
    bool   g_ownDraw = false;  // our own SetTransform / SetRenderState calls pass straight through
    bool   g_perspectiveActive = false;
    // For vertex-shader draws (skinned characters): MP2's projection, its inverse, and the camera
    // correction inverse(mp2View) * ourView of this frame.
    Matrix g_mp2Proj{}, g_mp2ProjInverse{}, g_correction{};
    bool   g_haveProjInverse = false, g_haveCorrection = false;
    UINT   g_constantsMoved = 0;
    // Whether this scene (BeginScene..EndScene) saw a perspective projection: MP2's 3D world does,
    // its HUD scenes do not. Blocks drawn at EndScene only after a 3D scene.
    bool g_sceneHadPerspective = false;
    // Per 5 s, for the log: our own draw calls that failed, and the last failure code.
    UINT g_drawFailures = 0;
    HRESULT g_lastDrawFailure = 0;

    struct Vertex
    {
        float x, y, z;
        DWORD color;
        float u, v;
    };
    constexpr DWORD kFvf = kFvfXyz | kFvfDiffuse | kFvfTex1;

    struct ScreenVertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    constexpr DWORD kScreenFvf = kFvfXyzRhw | kFvfDiffuse | kFvfTex1;

    struct Section
    {
        int   sx, sy, sz;
        void* solid = nullptr;  // vertex buffers (managed pool)
        UINT  solidCount = 0;   // vertices
        void* translucent = nullptr;
        UINT  translucentCount = 0;
        float probeU = 0, probeV = 0;  // first vertex's atlas UV: what our texture probe samples
        float probeX = 0, probeY = 0, probeZ = 0;  // first vertex, MC blocks relative to the section
    };

    void*                                     g_device = nullptr;  // the device our resources belong to
    void*                                     g_atlas = nullptr;
    UINT                                      g_atlasW = 0, g_atlasH = 0;
    std::unordered_map<std::uint64_t, Section> g_sections;
    void*                                     g_overlay = nullptr;
    UINT                                      g_overlayW = 0, g_overlayH = 0;
    bool                                      g_haveOverlay = false;
    DWORD                                     g_stateBlock = 0;
    bool                                      g_windowAttached = false;

    std::uint64_t SectionKey(int x, int y, int z)
    {
        return (std::uint64_t(std::uint32_t(x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(y) & 0x1FFFFF) << 21) | (std::uint32_t(z) & 0x1FFFFF);
    }

    void ReleaseSection(Section& s)
    {
        Release(s.solid);
        Release(s.translucent);
        s.solid = s.translucent = nullptr;
        s.solidCount = s.translucentCount = 0;
    }

    void ReleaseAll()
    {
        for (auto& [key, s] : g_sections)
            ReleaseSection(s);
        g_sections.clear();
        Release(g_atlas);
        g_atlas = nullptr;
        g_atlasW = g_atlasH = 0;
        Release(g_overlay);
        g_overlay = nullptr;
        g_overlayW = g_overlayH = 0;
        g_haveOverlay = false;
    }

    // RGBA8 (red in the low byte) rows -> an A8R8G8B8 texture region.
    void UploadRgba(void* texture, UINT x, UINT y, UINT w, UINT h, const std::uint8_t* rgba, UINT srcPitch, bool flipRows = false)
    {
        LockedRect locked{};
        RECT       rect{ static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + w), static_cast<LONG>(y + h) };
        if (const HRESULT hr = VCall<tex::LockRect>(texture, 0u, &locked, &rect, DWORD(0)); FAILED(hr)) {
            mclog::Info("texture: lock {}x{}+{}x{} failed ({:08x})", x, y, w, h, static_cast<unsigned>(hr));
            return;
        }
        for (UINT row = 0; row < h; ++row) {
            const auto* src = reinterpret_cast<const std::uint32_t*>(rgba + std::size_t(flipRows ? h - 1 - row : row) * srcPitch);
            auto*       dst = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(locked.pBits) + std::size_t(row) * locked.Pitch);
            for (UINT i = 0; i < w; ++i) {
                const std::uint32_t c = src[i];
                dst[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
            }
        }
        VCall<tex::UnlockRect>(texture, 0u);
    }

    void* CreateTexture(void* device, UINT w, UINT h)
    {
        void* texture = nullptr;
        if (const HRESULT hr = VCall<dev::CreateTexture>(device, w, h, 1u, DWORD(0), UINT(kFmtA8R8G8B8), UINT(kPoolManaged), &texture); FAILED(hr)) {
            mclog::Info("texture: create {}x{} failed ({:08x})", w, h, static_cast<unsigned>(hr));
            return nullptr;
        }
        return texture;
    }

    void* CreateVb(void* device, const std::vector<Vertex>& vertices)
    {
        if (vertices.empty())
            return nullptr;
        void*      vb = nullptr;
        const UINT bytes = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        if (const HRESULT hr = VCall<dev::CreateVertexBuffer>(device, bytes, DWORD(kUsageWriteOnly), kFvf, UINT(kPoolManaged), &vb); FAILED(hr)) {
            mclog::Info("vertex buffer: create {} bytes failed ({:08x})", bytes, static_cast<unsigned>(hr));
            return nullptr;
        }
        BYTE* data = nullptr;
        if (const HRESULT hr = VCall<vb::Lock>(vb, 0u, bytes, &data, DWORD(0)); FAILED(hr)) {
            mclog::Info("vertex buffer: lock {} bytes failed ({:08x})", bytes, static_cast<unsigned>(hr));
            return vb;  // filled with garbage: blocks will look wrong, the log says why
        }
        std::memcpy(data, vertices.data(), bytes);
        VCall<vb::Unlock>(vb);
        return vb;
    }

    // Minecraft's block light/sky light and fixed face shading, baked into the vertex colour
    // (MP2's fixed-function pipeline draws them unlit).
    DWORD BakeColor(std::uint32_t rgba, std::uint32_t light, std::uint32_t flags)
    {
        const float level = static_cast<float>((std::max)(light & 0xFF, (light >> 8) & 0xFF)) / 15.0f;
        const float bright = 0.12f + 0.88f * (level / (4.0f - 3.0f * level));
        static constexpr float kShade[7] = { 1.0f, 0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f };  // none, down, up, N, S, W, E
        const float k = bright * kShade[std::clamp<std::uint32_t>((flags >> 4) & 7, 0, 6)];
        auto        channel = [&](int shift) { return static_cast<DWORD>(std::clamp(((rgba >> shift) & 0xFF) * k, 0.0f, 255.0f)); };
        return 0xFF000000u | (channel(0) << 16) | (channel(8) << 8) | channel(16);
    }

    void OnRenderMessage(void* device, std::uint32_t type, const std::uint8_t* data, std::uint32_t bytes)
    {
        switch (type) {
        case proto::kRenAtlas:
            {
                if (bytes < sizeof(proto::RenAtlas))
                    return;
                const auto* hdr = reinterpret_cast<const proto::RenAtlas*>(data);
                if (bytes < sizeof(*hdr) + std::uint64_t(hdr->width) * hdr->height * 4)
                    return;
                if (!g_atlas || g_atlasW != hdr->width || g_atlasH != hdr->height) {
                    Release(g_atlas);
                    g_atlas = CreateTexture(device, hdr->width, hdr->height);
                    g_atlasW = hdr->width;
                    g_atlasH = hdr->height;
                }
                if (g_atlas)
                    UploadRgba(g_atlas, 0, 0, hdr->width, hdr->height, data + sizeof(*hdr), hdr->width * 4);
                mclog::Info("block atlas {}x{}", hdr->width, hdr->height);
                break;
            }
        case proto::kRenAtlasRegion:
            {
                if (!g_atlas || bytes < sizeof(proto::RenAtlasRegion))
                    return;
                const auto* r = reinterpret_cast<const proto::RenAtlasRegion*>(data);
                if (r->x + r->width > g_atlasW || r->y + r->height > g_atlasH || bytes < sizeof(*r) + std::uint64_t(r->width) * r->height * 4)
                    return;
                UploadRgba(g_atlas, r->x, r->y, r->width, r->height, data + sizeof(*r), r->width * 4);
                break;
            }
        case proto::kRenSection:
            {
                if (bytes < sizeof(proto::RenSection))
                    return;
                const auto* s = reinterpret_cast<const proto::RenSection*>(data);
                const auto  key = SectionKey(s->sx, s->sy, s->sz);
                if (auto it = g_sections.find(key); it != g_sections.end()) {
                    ReleaseSection(it->second);
                    g_sections.erase(it);
                }
                if (s->vertexCount == 0 || bytes < sizeof(*s) + std::uint64_t(s->vertexCount) * sizeof(proto::RenVertex))
                    return;
                const auto*                verts = reinterpret_cast<const proto::RenVertex*>(data + sizeof(*s));
                static std::vector<Vertex> solid, translucent;
                solid.clear();
                translucent.clear();
                for (std::uint32_t i = 0; i + 2 < s->vertexCount; i += 3) {
                    auto& out = (verts[i].flags & 2) ? translucent : solid;
                    for (int k = 0; k < 3; ++k) {
                        const auto& v = verts[i + k];
                        out.push_back({ v.x, v.y, v.z, BakeColor(v.color, v.light, v.flags), v.u, v.v });
                    }
                }
                Section section{ s->sx, s->sy, s->sz };
                section.probeX = solid.empty() ? verts[0].x : solid[0].x;
                section.probeY = solid.empty() ? verts[0].y : solid[0].y;
                section.probeZ = solid.empty() ? verts[0].z : solid[0].z;
                section.probeU = solid.empty() ? verts[0].u : solid[0].u;
                section.probeV = solid.empty() ? verts[0].v : solid[0].v;
                section.solid = CreateVb(device, solid);
                section.solidCount = section.solid ? static_cast<UINT>(solid.size()) : 0;
                section.translucent = CreateVb(device, translucent);
                section.translucentCount = section.translucent ? static_cast<UINT>(translucent.size()) : 0;
                g_sections[key] = section;
                {
                    const mp2::Vec3 origin = Mapping::Get().ToMp2(s->sx * 16.0, s->sy * 16.0, s->sz * 16.0);
                    const auto&     eye = State().camera.row[3];
                    mclog::Info("blocks: section ({}, {}, {}) {} verts -> MP2 ({:.1f}, {:.1f}, {:.1f}); eye at ({:.1f}, {:.1f}, {:.1f}); first vertex ({:.2f}, {:.2f}, {:.2f}) colour {:08x}",
                                s->sx, s->sy, s->sz, s->vertexCount, origin.x, origin.y, origin.z, eye.x, eye.y, eye.z, verts[0].x, verts[0].y, verts[0].z,
                                verts[0].color);
                }
                break;
            }
        case proto::kRenClearAll:
            for (auto& [key, s] : g_sections)
                ReleaseSection(s);
            g_sections.clear();
            break;
        default:
            break;  // avatar, scene entities, lights, NPC solids, dug bits: not drawn in MP2 yet
        }
    }

    void SetCommonStates(void* device)
    {
        VCall<dev::SetVertexShader>(device, kFvf);
        VCall<dev::SetPixelShader>(device, DWORD(0));
        VCall<dev::SetRenderState>(device, UINT(kRsLighting), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsFogEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsSpecularEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsCullMode), DWORD(kCullNone));
        VCall<dev::SetRenderState>(device, UINT(kRsFillMode), DWORD(kFillSolid));
        VCall<dev::SetRenderState>(device, UINT(kRsShadeMode), DWORD(kShadeGouraud));
        VCall<dev::SetRenderState>(device, UINT(kRsStencilEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsColorWriteEnable), DWORD(0xF));
        VCall<dev::SetRenderState>(device, UINT(kRsClipping), DWORD(TRUE));
        VCall<dev::SetRenderState>(device, UINT(kRsBlendOp), DWORD(kBlendOpAdd));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorOp), DWORD(kTopModulate));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorArg1), DWORD(kTaTexture));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorArg2), DWORD(kTaDiffuse));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaOp), DWORD(kTopModulate));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaArg1), DWORD(kTaTexture));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaArg2), DWORD(kTaDiffuse));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssTexCoordIndex), DWORD(0));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssTextureTransformFlags), DWORD(0));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssMagFilter), DWORD(kTexfPoint));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssMinFilter), DWORD(kTexfPoint));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssMipFilter), DWORD(kTexfNone));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAddressU), DWORD(kTaddressClamp));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAddressV), DWORD(kTaddressClamp));
        VCall<dev::SetTextureStageState>(device, 1u, UINT(kTssColorOp), DWORD(kTopDisable));
        VCall<dev::SetTextureStageState>(device, 1u, UINT(kTssAlphaOp), DWORD(kTopDisable));
    }

    // Fallback camera when MP2 didn't hand its matrices to SetTransform (vertex shader path):
    // view from our own camera, projection from Minecraft's FOV.
    void ComputeCamera(const Runtime& st, Matrix& view, Matrix& proj, UINT w, UINT h)
    {
        const auto& c = st.camera;
        const int   fRow = std::clamp(Config::Get().forwardRow, 0, 2);
        const int   uRow = fRow == 1 ? 2 : 1;
        const int   rRow = 3 - fRow - uRow;
        const mp2::Vec3 r = c.row[rRow], u = c.row[uRow], f = c.row[fRow], p = c.row[3];
        view = {};
        view.m[0][0] = r.x, view.m[1][0] = r.y, view.m[2][0] = r.z;
        view.m[0][1] = u.x, view.m[1][1] = u.y, view.m[2][1] = u.z;
        view.m[0][2] = f.x, view.m[1][2] = f.y, view.m[2][2] = f.z;
        view.m[3][0] = -(p.x * r.x + p.y * r.y + p.z * r.z);
        view.m[3][1] = -(p.x * u.x + p.y * u.y + p.z * u.z);
        view.m[3][2] = -(p.x * f.x + p.y * f.y + p.z * f.z);
        view.m[3][3] = 1.0f;
        const float units = Mapping::Get().UnitsPerBlock();
        const float zn = 0.05f * units, zf = 512.0f * units;
        const float yScale = 1.0f / std::tan(st.fovDeg * 0.0087266462599716f);
        const float xScale = yScale * float(h) / float(w);
        proj = {};
        proj.m[0][0] = xScale;
        proj.m[1][1] = yScale;
        proj.m[2][2] = zf / (zf - zn);
        proj.m[2][3] = 1.0f;
        proj.m[3][2] = -zn * zf / (zf - zn);
    }

    void Checked(HRESULT hr)
    {
        if (SUCCEEDED(hr))
            return;
        if (!g_drawFailures) {
            g_lastDrawFailure = hr;
            mclog::Info("blocks: our draw call failed ({:08x})", static_cast<unsigned>(hr));
        }
        ++g_drawFailures;
    }

    Matrix Multiply(const Matrix& a, const Matrix& b);  // defined below, near its row-vector helpers

    void XformPoint(const float p[3], const Matrix& m, float out[4])
    {
        for (int j = 0; j < 4; ++j)
            out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
    }

    void DrawWorld(void* device, const Runtime& st, UINT w, UINT h)
    {
        if (!g_atlas || g_sections.empty() || !Mapping::Get().Calibrated())
            return;
        // Our blocks live in MP2 world space and our eye is st.camera, so the view is built from it
        // directly. MP2's own view matrices are rotation-only (its scene rides on identity views with
        // the camera baked into the world matrices — "world matrices moved" in the log), so they
        // carry no eye translation and would displace every block off screen.
        Matrix view{}, proj{};
        ComputeCamera(st, view, proj, w, h);
        if (g_haveProj)
            proj = g_proj;  // the projection the picture really uses

        // Where does the first block vertex actually land on the screen? Decides between "wrong
        // matrices" (off screen) and "pixels discarded" (on screen but invisible).
        static DWORD lastProbe = 0;
        const bool   probe = GetTickCount() - lastProbe > 5000;
        if (probe) {
            lastProbe = GetTickCount();
            const Section* first = nullptr;
            for (const auto& [key, s] : g_sections)
                if (s.solidCount >= 3) {
                    first = &s;
                    break;
                }
            if (first) {
                Matrix world{}, worldViewProj = Multiply(view, proj);
                Mapping::Get().McToMp2Matrix(first->sx * 16.0, first->sy * 16.0, first->sz * 16.0, world.m);
                worldViewProj = Multiply(world, worldViewProj);
                const float   p[3] = { first->probeX, first->probeY, first->probeZ };
                float         worldPos[4]{}, clip[4]{};
                XformPoint(p, world, worldPos);
                XformPoint(p, worldViewProj, clip);
                const float cw = clip[3] != 0.0f ? clip[3] : 1e-6f;
                mclog::Info("blocks probe: vertex world ({:.1f}, {:.1f}, {:.1f}) -> ndc ({:.2f}, {:.2f}, {:.3f}) w {:.1f} -> screen ({:.0f}, {:.0f}) of {}x{}; "
                            "eye view last row ({:.1f} {:.1f} {:.1f}), proj {:.2f}/{:.2f}{}, z {}{}",
                            worldPos[0], worldPos[1], worldPos[2], clip[0] / cw, clip[1] / cw, clip[2] / cw, clip[3], (clip[0] / cw * 0.5f + 0.5f) * w,
                            (0.5f - clip[1] / cw * 0.5f) * h, w, h, view.m[3][0], view.m[3][1], view.m[3][2], proj.m[0][0], proj.m[1][1],
                            g_haveProj ? " (game)" : " (own)", g_sceneZEnable, Config::Get().blocksNoDepth ? ", depth off" : "");
                // What does the atlas hold at that vertex's UV? (A8R8G8B8, read back as AARRGGBB.)
                const UINT tx = (std::min)(static_cast<UINT>(first->probeU * g_atlasW), g_atlasW - 1);
                const UINT ty = (std::min)(static_cast<UINT>(first->probeV * g_atlasH), g_atlasH - 1);
                LockedRect locked{};
                RECT       rect{ static_cast<LONG>(tx), static_cast<LONG>(ty), static_cast<LONG>(tx + 1), static_cast<LONG>(ty + 1) };
                if (SUCCEEDED(VCall<tex::LockRect>(g_atlas, 0u, &locked, &rect, DWORD(0)))) {
                    const std::uint32_t texel = *reinterpret_cast<const std::uint32_t*>(locked.pBits);
                    VCall<tex::UnlockRect>(g_atlas, 0u);
                    mclog::Info("blocks probe: atlas texel ({}, {}) for uv ({:.3f}, {:.3f}) = {:08x} AARRGGBB", tx, ty, first->probeU, first->probeV, texel);
                }
            }
        }

        VCall<dev::SetTransform>(device, UINT(kTsView), static_cast<const Matrix*>(&view));
        VCall<dev::SetTransform>(device, UINT(kTsProjection), static_cast<const Matrix*>(&proj));
        SetCommonStates(device);
        const bool untextured = Config::Get().blocksNoTexture;
        VCall<dev::SetTexture>(device, 0u, untextured ? static_cast<void*>(nullptr) : g_atlas);
        if (untextured) {  // no texture, no alpha test: pure vertex colour — isolates texture problems
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorOp), DWORD(kTopSelectArg1));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorArg1), DWORD(kTaDiffuse));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaOp), DWORD(kTopSelectArg1));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaArg1), DWORD(kTaDiffuse));
        }
        VCall<dev::SetRenderState>(device, UINT(kRsZEnable), Config::Get().blocksNoDepth ? DWORD(FALSE) : (g_sceneZEnable ? g_sceneZEnable : DWORD(TRUE)));
        VCall<dev::SetRenderState>(device, UINT(kRsZFunc), DWORD(kCmpLessEqual));

        auto&  map = Mapping::Get();
        Matrix world{};
        auto   drawPass = [&](bool translucent) {
            for (const auto& [key, s] : g_sections) {
                void* vb = translucent ? s.translucent : s.solid;
                UINT  count = translucent ? s.translucentCount : s.solidCount;
                if (!vb || count < 3)
                    continue;
                map.McToMp2Matrix(s.sx * 16.0, s.sy * 16.0, s.sz * 16.0, world.m);
                VCall<dev::SetTransform>(device, UINT(kTsWorld), static_cast<const Matrix*>(&world));
                Checked(VCall<dev::SetStreamSource>(device, 0u, vb, UINT(sizeof(Vertex))));
                Checked(VCall<dev::DrawPrimitive>(device, UINT(kPtTriangleList), 0u, count / 3));
                g_trianglesDrawn += count / 3;
            }
        };

        // Solid and cutout blocks (leaves, glass panes, flowers: alpha tested).
        VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), DWORD(TRUE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), untextured ? DWORD(FALSE) : DWORD(TRUE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaRef), DWORD(0x80));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaFunc), DWORD(kCmpGreaterEqual));
        drawPass(false);

        // Water, stained glass, ice.
        VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), DWORD(TRUE));
        VCall<dev::SetRenderState>(device, UINT(kRsSrcBlend), DWORD(kBlendSrcAlpha));
        VCall<dev::SetRenderState>(device, UINT(kRsDestBlend), DWORD(kBlendInvSrcAlpha));
        drawPass(true);

        // The targeted block's outline.
        static proto::WorldEntities entities;
        if (Link::Get().ReadWorldEntities(entities) && entities.hasSelection) {
            const float* lo = entities.selMin;
            const float* hi = entities.selMax;
            const float  e = 0.002f;
            const float  x0 = lo[0] - e, y0 = lo[1] - e, z0 = lo[2] - e, x1 = hi[0] + e, y1 = hi[1] + e, z1 = hi[2] + e;
            const DWORD  black = 0x66000000;
            const Vertex corners[8] = { { x0, y0, z0, black }, { x1, y0, z0, black }, { x1, y0, z1, black }, { x0, y0, z1, black },
                                        { x0, y1, z0, black }, { x1, y1, z0, black }, { x1, y1, z1, black }, { x0, y1, z1, black } };
            static constexpr int kEdges[24] = { 0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7 };
            Vertex               lines[24];
            for (int i = 0; i < 24; ++i)
                lines[i] = corners[kEdges[i]];
            map.McToMp2Matrix(0.0, 0.0, 0.0, world.m);
            VCall<dev::SetTransform>(device, UINT(kTsWorld), static_cast<const Matrix*>(&world));
            VCall<dev::SetTexture>(device, 0u, static_cast<void*>(nullptr));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorOp), DWORD(kTopSelectArg1));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorArg1), DWORD(kTaDiffuse));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaOp), DWORD(kTopSelectArg1));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaArg1), DWORD(kTaDiffuse));
            VCall<dev::DrawPrimitiveUP>(device, UINT(kPtLineList), 12u, static_cast<const void*>(lines), UINT(sizeof(Vertex)));
        }
    }

    void DrawOverlay(void* device, const Runtime& st, UINT w, UINT h)
    {
        auto& link = Link::Get();
        if (link.AcquireOverlayFrame()) {
            const auto*   hdr = link.FrontHeader();
            const UINT    ow = (std::min)(hdr->width, proto::kMaxOverlayW), oh = (std::min)(hdr->height, proto::kMaxOverlayH);
            const auto*   pixels = ow && oh ? link.FrontPixels(ow, oh) : nullptr;
            if (pixels) {
                if (!g_overlay || g_overlayW != ow || g_overlayH != oh) {
                    Release(g_overlay);
                    g_overlay = CreateTexture(device, ow, oh);
                    g_overlayW = ow;
                    g_overlayH = oh;
                }
                if (g_overlay) {
                    UploadRgba(g_overlay, 0, 0, ow, oh, pixels, ow * 4, (hdr->flags & 1) != 0);
                    g_haveOverlay = true;
                }
            }
        }
        if (!g_overlay || !g_haveOverlay || !st.mcInWorld || !st.minecraftOwnsPlayer || st.gameOwnsInput)
            return;

        SetCommonStates(device);
        VCall<dev::SetVertexShader>(device, kScreenFvf);
        VCall<dev::SetRenderState>(device, UINT(kRsZEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), DWORD(TRUE));
        VCall<dev::SetRenderState>(device, UINT(kRsSrcBlend), DWORD(kBlendOne));  // premultiplied alpha
        VCall<dev::SetRenderState>(device, UINT(kRsDestBlend), DWORD(kBlendInvSrcAlpha));
        VCall<dev::SetTexture>(device, 0u, g_overlay);
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorOp), DWORD(kTopSelectArg1));
        VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaOp), DWORD(kTopSelectArg1));

        const float        x1 = float(w) - 0.5f, y1 = float(h) - 0.5f;
        const ScreenVertex quad[4] = { { -0.5f, -0.5f, 0, 1, 0xFFFFFFFF, 0, 0 },
                                       { x1, -0.5f, 0, 1, 0xFFFFFFFF, 1, 0 },
                                       { -0.5f, y1, 0, 1, 0xFFFFFFFF, 0, 1 },
                                       { x1, y1, 0, 1, 0xFFFFFFFF, 1, 1 } };
        VCall<dev::DrawPrimitiveUP>(device, UINT(kPtTriangleStrip), 2u, static_cast<const void*>(quad), UINT(sizeof(ScreenVertex)));

        // The mouse cursor over Minecraft's screens (its own window is hidden, so it draws none).
        if (st.mcScreenOpen) {
            VCall<dev::SetTexture>(device, 0u, static_cast<void*>(nullptr));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssColorArg1), DWORD(kTaDiffuse));
            VCall<dev::SetTextureStageState>(device, 0u, UINT(kTssAlphaArg1), DWORD(kTaDiffuse));
            VCall<dev::SetRenderState>(device, UINT(kRsSrcBlend), DWORD(kBlendSrcAlpha));
            const float cx = float(st.cursorX.load()) * w / (std::max)(1, st.viewportW.load());
            const float cy = float(st.cursorY.load()) * h / (std::max)(1, st.viewportH.load());
            auto        tri = [&](float s, DWORD color) {
                const ScreenVertex v[3] = { { cx - s * 0.1f, cy - s * 0.15f, 0, 1, color, 0, 0 },
                                            { cx - s * 0.1f, cy + s * 1.05f, 0, 1, color, 0, 0 },
                                            { cx + s * 0.8f, cy + s * 0.75f, 0, 1, color, 0, 0 } };
                VCall<dev::DrawPrimitiveUP>(device, UINT(kPtTriangleList), 1u, static_cast<const void*>(v), UINT(sizeof(ScreenVertex)));
            };
            tri(18.0f, 0xFF000000);
            tri(14.0f, 0xFFFFFFFF);
        }
    }

    // Called inside MP2's own scene (our EndScene hook): its depth buffer still belongs to the 3D
    // scene it just drew, so blocks keep their occlusion.
    void DrawWorldAtEndScene(void* device)
    {
        const Runtime& st = State();
        if (!g_stateBlock)
            VCall<dev::CreateStateBlock>(device, UINT(kStateBlockAll), &g_stateBlock);
        if (g_stateBlock)
            VCall<dev::CaptureStateBlock>(device, g_stateBlock);
        Viewport saved{};
        VCall<dev::GetViewport>(device, &saved);
        const Viewport full{ 0, 0, saved.Width ? saved.Width : 1280u, saved.Height ? saved.Height : 720u, 0.0f, 1.0f };
        VCall<dev::SetViewport>(device, &full);
        g_ownDraw = true;
        Guarded([&] {
            if (st.cameraValid)
                DrawWorld(device, st, full.Width, full.Height);
        });
        g_ownDraw = false;
        VCall<dev::SetViewport>(device, &saved);
        if (g_stateBlock)
            VCall<dev::ApplyStateBlock>(device, g_stateBlock);
    }

    HRESULT __stdcall BeginSceneHook(void* device)
    {
        g_sceneHadPerspective = false;
        return g_origBeginScene(device);
    }

    HRESULT __stdcall EndSceneHook(void* device)
    {
        if (Config::Get().drawAtEndScene && g_sceneHadPerspective && State().mcInWorld)
            DrawWorldAtEndScene(device);
        return g_origEndScene(device);
    }

    HRESULT __stdcall Present(void* device, const RECT* src, const RECT* dst, HWND window, const void* dirty)
    {
        if (device != g_device) {
            // A new device (first frame, or MP2 recreated it): our resources belonged to the old one.
            ReleaseAll();
            g_stateBlock = 0;
            g_device = device;
        }
        // Shared memory, once MP2 is up (see main.cpp).
        static bool linkTried = false;
        if (!linkTried) {
            linkTried = true;
            if (!Link::Get().Create())
                mclog::Info("no shared memory: Minecraft can't connect this session");
        }
        // Minecraft starts once a level is actually running (the player updating), not during MP2's
        // loading: its boot storm (JVM, disk, the launcher's window) while a level loads crashed MP2
        // inside its own loader twice now. A 60 s fallback keeps it starting even if the player is
        // never detected (or the user sits at the menu, which is a safe time too).
        static bool  minecraftStarted = false;
        static DWORD firstPresent = 0;
        if (!minecraftStarted) {
            if (!firstPresent)
                firstPresent = GetTickCount();
            const Runtime& rst = State();
            const bool     playing = rst.player && GetTickCount64() - rst.lastPlayerFrameMs < 500;
            if (playing || GetTickCount() - firstPresent > 60000) {
                minecraftStarted = true;
                launcher::StartMinecraft();
            }
        }
        if (!g_windowAttached) {
            DeviceCreationParameters params{};
            if (SUCCEEDED((VCall<dev::GetCreationParameters>(device, &params)))) {
                input::AttachWindow(params.hFocusWindow);
                g_windowAttached = true;
            }
        }

        UINT  w = 0, h = 0;
        void* backBuffer = nullptr;
        if (SUCCEEDED((VCall<dev::GetBackBuffer>(device, 0u, UINT(kBackBufferMono), &backBuffer))) && backBuffer) {
            SurfaceDesc desc{};
            if (SUCCEEDED((VCall<surf::GetDesc>(backBuffer, &desc)))) {
                w = desc.Width;
                h = desc.Height;
            }
            Release(backBuffer);
        }
        if (w && h) {
            characters::OnPresent(w, h);
            Link::Get().DrainRender([&](std::uint32_t type, const std::uint8_t* data, std::uint32_t bytes) { OnRenderMessage(device, type, data, bytes); },
                                    8ull << 20);

            // Once per device: does one exist, and what is it?
            static void* depthLoggedFor = nullptr;
            if (device != depthLoggedFor) {
                depthLoggedFor = device;
                void* depth = nullptr;
                if (SUCCEEDED(VCall<dev::GetDepthStencilSurface>(device, &depth))) {
                    SurfaceDesc desc{};
                    if (SUCCEEDED(VCall<surf::GetDesc>(depth, &desc)))
                        mclog::Info("render: depth stencil {}x{} format {} multisample {}", desc.Width, desc.Height, desc.Format, desc.MultiSampleType);
                    Release(depth);
                } else {
                    mclog::Info("render: no depth stencil attached");
                }
            }

            const Runtime& st = State();
            if (st.mcInWorld) {
                if (!g_stateBlock)
                    VCall<dev::CreateStateBlock>(device, UINT(kStateBlockAll), &g_stateBlock);
                if (g_stateBlock)
                    VCall<dev::CaptureStateBlock>(device, g_stateBlock);
                Viewport saved{};
                VCall<dev::GetViewport>(device, &saved);
                const Viewport full{ 0, 0, w, h, 0.0f, 1.0f };
                VCall<dev::SetViewport>(device, &full);
                if (SUCCEEDED((VCall<dev::BeginScene>(device)))) {
                    g_ownDraw = true;
                    Guarded([&] {
                        if (st.cameraValid && !Config::Get().drawAtEndScene)
                            DrawWorld(device, st, w, h);
                        DrawOverlay(device, st, w, h);
                    });
                    g_ownDraw = false;
                    VCall<dev::EndScene>(device);
                }
                VCall<dev::SetViewport>(device, &saved);
                if (g_stateBlock)
                    VCall<dev::ApplyStateBlock>(device, g_stateBlock);
            }
        }
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 5000) {
            lastLog = GetTickCount();
            mclog::Info("render: view set {} times, replaced {}, world matrices moved {}; scene projection {}; {} block sections, {} block "
                        "triangles drawn, atlas {}, scene z mode {}, shader constants moved {}, our draw failures {}",
                        g_viewSets, g_viewReplaced, g_worldCorrected, g_haveProj ? "seen" : "not seen", g_sections.size(), g_trianglesDrawn,
                        g_atlas ? "yes" : "no", g_sceneZEnable, g_constantsMoved, g_drawFailures);
            g_viewSets = g_viewReplaced = g_worldCorrected = g_trianglesDrawn = g_constantsMoved = g_drawFailures = 0;
        }
        g_haveView = g_haveProj = false;  // MP2 sets them again each frame
        return g_origPresent(device, src, dst, window, dirty);
    }

    HRESULT __stdcall Reset(void* device, PresentParameters* params)
    {
        if (g_stateBlock) {
            VCall<dev::DeleteStateBlock>(device, g_stateBlock);
            g_stateBlock = 0;
        }
        return g_origReset(device, params);
    }

    // Row-vector 4x4 helpers for rigid transforms.
    Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        return r;
    }

    Matrix RigidInverse(const Matrix& a)
    {
        Matrix r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a.m[j][i];
        for (int j = 0; j < 3; ++j)
            r.m[3][j] = -(a.m[3][0] * r.m[0][j] + a.m[3][1] * r.m[1][j] + a.m[3][2] * r.m[2][j]);
        r.m[3][3] = 1.0f;
        return r;
    }

    bool IsIdentity(const Matrix& m)
    {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (std::fabs(m.m[i][j] - (i == j ? 1.0f : 0.0f)) > 1e-5f)
                    return false;
        return true;
    }


    using SetRenderStateFn = HRESULT(__stdcall*)(void*, UINT, DWORD);
    SetRenderStateFn g_origSetRenderState = nullptr;

    HRESULT __stdcall SetRenderState(void* device, UINT state, DWORD value)
    {
        if (!g_ownDraw && state == kRsZEnable && g_perspectiveActive && value)
            g_sceneZEnable = value;
        return g_origSetRenderState(device, state, value);
    }

    bool Inverse4(const Matrix& a, Matrix& out)
    {
        float m[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                m[i][j] = a.m[i][j];
                m[i][j + 4] = i == j ? 1.0f : 0.0f;
            }
        for (int c = 0; c < 4; ++c) {
            int pivot = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(m[r][c]) > std::fabs(m[pivot][c]))
                    pivot = r;
            if (std::fabs(m[pivot][c]) < 1e-12f)
                return false;
            for (int j = 0; j < 8; ++j)
                std::swap(m[c][j], m[pivot][j]);
            const float inv = 1.0f / m[c][c];
            for (int j = 0; j < 8; ++j)
                m[c][j] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c) {
                    const float f = m[r][c];
                    for (int j = 0; j < 8; ++j)
                        m[r][j] -= f * m[c][j];
                }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out.m[i][j] = m[i][j + 4];
        return true;
    }

    Matrix Transpose(const Matrix& a)
    {
        Matrix r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[j][i];
        return r;
    }

    // M = world * view * proj  =>  M * inverse(proj) is affine (last column 0, 0, 0, 1).
    bool LooksLikeWorldViewProj(const Matrix& m, Matrix& worldView)
    {
        worldView = Multiply(m, g_mp2ProjInverse);
        return std::fabs(worldView.m[0][3]) < 1e-3f && std::fabs(worldView.m[1][3]) < 1e-3f && std::fabs(worldView.m[2][3]) < 1e-3f &&
               std::fabs(worldView.m[3][3] - 1.0f) < 1e-3f;
    }

    using SetVsConstantFn = HRESULT(__stdcall*)(void*, DWORD, const void*, DWORD);
    SetVsConstantFn g_origSetVsConstant = nullptr;

    HRESULT __stdcall SetVertexShaderConstant(void* device, DWORD reg, const void* data, DWORD count)
    {
        const Runtime& st = State();
        if (g_ownDraw || !data || count < 4 || count > 96 || !st.cameraValid || !g_haveCorrection || !g_haveProjInverse)
            return g_origSetVsConstant(device, reg, data, count);
        static float buffer[96 * 4];
        std::memcpy(buffer, data, count * 16);
        bool changed = false;
        for (DWORD i = 0; i + 4 <= count;) {
            Matrix regs{};
            std::memcpy(regs.m, buffer + i * 4, 64);
            Matrix worldView{};
            // Shaders usually get the matrix transposed (one row per register); try both.
            const Matrix asRows = Transpose(regs);
            if (LooksLikeWorldViewProj(asRows, worldView)) {
                const Matrix fixed = Transpose(Multiply(Multiply(worldView, g_correction), g_mp2Proj));
                std::memcpy(buffer + i * 4, fixed.m, 64);
                changed = true;
                i += 4;
            } else if (LooksLikeWorldViewProj(regs, worldView)) {
                const Matrix fixed = Multiply(Multiply(worldView, g_correction), g_mp2Proj);
                std::memcpy(buffer + i * 4, fixed.m, 64);
                changed = true;
                i += 4;
            } else {
                ++i;
            }
        }
        if (changed)
            ++g_constantsMoved;
        return g_origSetVsConstant(device, reg, changed ? buffer : data, count);
    }

    HRESULT __stdcall SetTransform(void* device, UINT state, const Matrix* matrix)
    {
        if (g_ownDraw || !matrix)
            return g_origSetTransform(device, state, matrix);

        // MP2 draws in two ways: most of the scene with a real view matrix, and some objects
        // (doors, props...) with an identity view and the camera baked into their world matrix.
        // The first gets Minecraft's eye as its view; the second gets its world matrix moved from
        // MP2's camera to ours: world * inverse(mp2View) * ourView.
        static Matrix pendingView{};
        static bool   perspective = false;
        static bool   viewIsIdentity = false;
        static Matrix ours{}, correction{}, corrected{};
        static bool   haveCorrection = false;
        const Runtime& st = State();

        if (state == kTsView) {
            ++g_viewSets;
            viewIsIdentity = IsIdentity(*matrix);
            if (st.cameraValid && !viewIsIdentity) {
                Matrix unusedProj{};
                ComputeCamera(st, ours, unusedProj, 16, 9);
                correction = Multiply(RigidInverse(*matrix), ours);
                haveCorrection = true;
                g_correction = correction;
                g_haveCorrection = true;
                matrix = &ours;
                ++g_viewReplaced;
            }
        } else if (state == kTsWorld && viewIsIdentity && perspective && st.cameraValid && haveCorrection) {
            corrected = Multiply(*matrix, correction);
            matrix = &corrected;
            ++g_worldCorrected;
        }
        if (!st.cameraValid)
            haveCorrection = g_haveCorrection = false;

        if (state == kTsView) {
            pendingView = *matrix;
            if (perspective) {
                g_view = *matrix;
                g_haveView = true;
            }
            // Also capture views set before the projection (or after it, which MP2 does): the eye
            // translation is what our blocks need, and identity views (the UI) must not provide it.
            if (!viewIsIdentity) {
                g_view = *matrix;
                g_haveView = true;
            }
        } else if (state == kTsProjection) {
            perspective = matrix->m[2][3] != 0.0f && matrix->m[3][3] == 0.0f;
            g_perspectiveActive = perspective;
            if (perspective)
                g_sceneHadPerspective = true;
            if (perspective && std::memcmp(&g_mp2Proj, matrix, sizeof(Matrix)) != 0) {
                g_mp2Proj = *matrix;
                g_haveProjInverse = Inverse4(g_mp2Proj, g_mp2ProjInverse);
            }
            if (perspective) {
                g_proj = *matrix;
                g_view = pendingView;
                g_haveProj = g_haveView = true;
            }
        }
        return g_origSetTransform(device, state, matrix);
    }
}

namespace
{
    // No throwaway device of our own: creating one while MP2's launcher checks the display
    // adapter makes that check fail. Instead the game's own objects are hooked as they appear:
    // Direct3DCreate8 -> IDirect3D8::CreateDevice -> the device's vtable.
    using Create8Fn = void*(__stdcall*)(UINT);
    using CreateDeviceFn = HRESULT(__stdcall*)(void*, UINT, UINT, HWND, DWORD, PresentParameters*, void**);
    Create8Fn      g_origCreate8 = nullptr;
    CreateDeviceFn g_origCreateDevice = nullptr;
    bool           g_deviceHooked = false;

    HRESULT __stdcall CreateDevice(void* d3d, UINT adapter, UINT type, HWND window, DWORD flags, PresentParameters* params, void** out)
    {
        const HRESULT hr = g_origCreateDevice(d3d, adapter, type, window, flags, params, out);
        if (SUCCEEDED(hr) && out && *out && !g_deviceHooked) {
            g_deviceHooked = true;
            void* device = *out;
            InstallHook(VSlot(device, dev::Present), reinterpret_cast<void*>(&Present), g_origPresent, "IDirect3DDevice8::Present");
            InstallHook(VSlot(device, dev::Reset), reinterpret_cast<void*>(&Reset), g_origReset, "IDirect3DDevice8::Reset");
            InstallHook(VSlot(device, dev::BeginScene), reinterpret_cast<void*>(&BeginSceneHook), g_origBeginScene, "IDirect3DDevice8::BeginScene");
            InstallHook(VSlot(device, dev::EndScene), reinterpret_cast<void*>(&EndSceneHook), g_origEndScene, "IDirect3DDevice8::EndScene");
            InstallHook(VSlot(device, dev::SetTransform), reinterpret_cast<void*>(&SetTransform), g_origSetTransform, "IDirect3DDevice8::SetTransform");
            InstallHook(VSlot(device, dev::SetRenderState), reinterpret_cast<void*>(&SetRenderState), g_origSetRenderState, "IDirect3DDevice8::SetRenderState");
            InstallHook(VSlot(device, dev::SetVertexShaderConstant), reinterpret_cast<void*>(&SetVertexShaderConstant), g_origSetVsConstant,
                        "IDirect3DDevice8::SetVertexShaderConstant");
        }
        return hr;
    }

    void* __stdcall Create8(UINT version)
    {
        void* d3d = g_origCreate8(version);
        if (d3d && !g_origCreateDevice)
            InstallHook(VSlot(d3d, d3d::CreateDevice), reinterpret_cast<void*>(&CreateDevice), g_origCreateDevice, "IDirect3D8::CreateDevice");
        return d3d;
    }
}

bool render::Install()
{
    HMODULE d3d8 = LoadLibraryW(L"d3d8.dll");
    void*   create = d3d8 ? reinterpret_cast<void*>(GetProcAddress(d3d8, "Direct3DCreate8")) : nullptr;
    if (!create) {
        mclog::Info("render: Direct3DCreate8 not found");
        return false;
    }
    return InstallHook(create, reinterpret_cast<void*>(&Create8), g_origCreate8, "Direct3DCreate8");
}
