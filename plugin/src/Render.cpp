#include "Render.h"

#include "Characters.h"
#include "Config.h"
#include "D3D8.h"
#include "Hook.h"
#include "Input.h"
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
    PresentFn      g_origPresent = nullptr;
    ResetFn        g_origReset = nullptr;
    SetTransformFn g_origSetTransform = nullptr;

    // MP2's own camera matrices, as it last set them: our blocks use exactly the same ones, so they
    // line up with its picture and depth buffer.
    Matrix g_view{}, g_proj{};
    bool   g_haveView = false, g_haveProj = false;

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
        if (FAILED((VCall<tex::LockRect>(texture, 0u, &locked, &rect, DWORD(0)))))
            return;
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
        if (FAILED((VCall<dev::CreateTexture>(device, w, h, 1u, DWORD(0), UINT(kFmtA8R8G8B8), UINT(kPoolManaged), &texture))))
            return nullptr;
        return texture;
    }

    void* CreateVb(void* device, const std::vector<Vertex>& vertices)
    {
        if (vertices.empty())
            return nullptr;
        void*      vb = nullptr;
        const UINT bytes = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        if (FAILED((VCall<dev::CreateVertexBuffer>(device, bytes, DWORD(kUsageWriteOnly), kFvf, UINT(kPoolManaged), &vb))))
            return nullptr;
        BYTE* data = nullptr;
        if (SUCCEEDED((VCall<vb::Lock>(vb, 0u, bytes, &data, DWORD(0))))) {
            std::memcpy(data, vertices.data(), bytes);
            VCall<vb::Unlock>(vb);
        }
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
        return ((rgba >> 24) << 24) | (channel(0) << 16) | (channel(8) << 8) | channel(16);
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
                section.solid = CreateVb(device, solid);
                section.solidCount = section.solid ? static_cast<UINT>(solid.size()) : 0;
                section.translucent = CreateVb(device, translucent);
                section.translucentCount = section.translucent ? static_cast<UINT>(translucent.size()) : 0;
                g_sections[key] = section;
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

    void DrawWorld(void* device, const Runtime& st, UINT w, UINT h)
    {
        if (!g_atlas || g_sections.empty() || !Mapping::Get().Calibrated())
            return;
        Matrix view = g_view, proj = g_proj;
        if (!g_haveView || !g_haveProj)
            ComputeCamera(st, view, proj, w, h);
        VCall<dev::SetTransform>(device, UINT(kTsView), static_cast<const Matrix*>(&view));
        VCall<dev::SetTransform>(device, UINT(kTsProjection), static_cast<const Matrix*>(&proj));
        SetCommonStates(device);
        VCall<dev::SetTexture>(device, 0u, g_atlas);
        VCall<dev::SetRenderState>(device, UINT(kRsZEnable), DWORD(TRUE));
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
                VCall<dev::SetStreamSource>(device, 0u, vb, UINT(sizeof(Vertex)));
                VCall<dev::DrawPrimitive>(device, UINT(kPtTriangleList), 0u, count / 3);
            }
        };

        // Solid and cutout blocks (leaves, glass panes, flowers: alpha tested).
        VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), DWORD(TRUE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), DWORD(FALSE));
        VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), DWORD(TRUE));
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
        if (!g_overlay || !g_haveOverlay || !st.mcInWorld)
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

    HRESULT __stdcall Present(void* device, const RECT* src, const RECT* dst, HWND window, const void* dirty)
    {
        if (device != g_device) {
            // A new device (first frame, or MP2 recreated it): our resources belonged to the old one.
            ReleaseAll();
            g_stateBlock = 0;
            g_device = device;
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
                    Guarded([&] {
                        if (st.cameraValid)
                            DrawWorld(device, st, w, h);
                        DrawOverlay(device, st, w, h);
                    });
                    VCall<dev::EndScene>(device);
                }
                VCall<dev::SetViewport>(device, &saved);
                if (g_stateBlock)
                    VCall<dev::ApplyStateBlock>(device, g_stateBlock);
            }
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

    HRESULT __stdcall SetTransform(void* device, UINT state, const Matrix* matrix)
    {
        // Keep the view that goes with the scene's perspective projection, not a HUD pass's
        // (2D passes set an orthographic projection and often an identity view).
        static Matrix pendingView{};
        static bool   perspective = false;
        if (matrix) {
            if (state == kTsView) {
                pendingView = *matrix;
                if (perspective) {
                    g_view = *matrix;
                    g_haveView = true;
                }
            } else if (state == kTsProjection) {
                perspective = matrix->m[2][3] != 0.0f && matrix->m[3][3] == 0.0f;
                if (perspective) {
                    g_proj = *matrix;
                    g_view = pendingView;
                    g_haveProj = g_haveView = true;
                }
            }
        }
        return g_origSetTransform(device, state, matrix);
    }
}

bool render::Install()
{
    HMODULE d3d8 = LoadLibraryW(L"d3d8.dll");
    using CreateFn = void*(__stdcall*)(UINT);
    auto create = d3d8 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d8, "Direct3DCreate8")) : nullptr;
    if (!create) {
        mclog::Info("render: Direct3DCreate8 not found");
        return false;
    }
    void* d3d = create(kSdkVersion);
    if (!d3d) {
        mclog::Info("render: Direct3DCreate8 failed");
        return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MaxCraftDummy";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DisplayMode mode{};
    VCall<d3d::GetAdapterDisplayMode>(d3d, 0u, &mode);
    PresentParameters pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = kSwapDiscard;
    pp.BackBufferFormat = mode.Format;
    pp.hDeviceWindow = window;
    void*   device = nullptr;
    HRESULT hr = VCall<d3d::CreateDevice>(d3d, 0u, UINT(kDevTypeHal), window, DWORD(kCreateSoftwareVp), &pp, &device);
    if (FAILED(hr) || !device) {
        mclog::Info("render: dummy device failed ({:#x})", static_cast<unsigned>(hr));
        Release(d3d);
        DestroyWindow(window);
        return false;
    }

    bool ok = true;
    ok &= InstallHook(VSlot(device, dev::Present), reinterpret_cast<void*>(&Present), g_origPresent, "IDirect3DDevice8::Present");
    ok &= InstallHook(VSlot(device, dev::Reset), reinterpret_cast<void*>(&Reset), g_origReset, "IDirect3DDevice8::Reset");
    ok &= InstallHook(VSlot(device, dev::SetTransform), reinterpret_cast<void*>(&SetTransform), g_origSetTransform, "IDirect3DDevice8::SetTransform");
    Release(device);
    Release(d3d);
    DestroyWindow(window);
    return ok;
}
