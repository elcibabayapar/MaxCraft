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
    using ClearFn = HRESULT(__stdcall*)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
    ClearFn        g_origClear = nullptr;
    // This frame: how much of MP2's 3D scene went by (views replaced / world matrices moved) and
    // whether the blocks are already in. They go in at the last moment MP2's depth buffer still
    // holds the world: right before MP2 clears it mid-frame (for its weapon / HUD), else at the end
    // of its 3D scene, else (no occlusion then) at Present.
    UINT           g_frameSceneDraws = 0;
    bool           g_blocksDrawnThisFrame = false;
    UINT           g_drawnAtClear = 0, g_drawnAtEndScene = 0, g_drawnAtPresent = 0;
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
    // The projections MP2's world was drawn with this frame, and how many world draws used each:
    // blocks use the busiest one (the last projection set can be the weapon's or an effect's, which
    // drew the blocks skewed against the world).
    struct ProjUse
    {
        Matrix m;
        UINT   uses;
    };
    ProjUse g_frameProj[4]{};
    UINT    g_frameProjCount = 0;

    void NoteSceneProjection()
    {
        for (UINT i = 0; i < g_frameProjCount; ++i)
            if (std::memcmp(&g_frameProj[i].m, &g_mp2Proj, sizeof(Matrix)) == 0) {
                ++g_frameProj[i].uses;
                return;
            }
        if (g_frameProjCount < 4)
            g_frameProj[g_frameProjCount++] = { g_mp2Proj, 1 };
    }

    const Matrix* SceneProjection()
    {
        const ProjUse* best = nullptr;
        for (UINT i = 0; i < g_frameProjCount; ++i)
            if (!best || g_frameProj[i].uses > best->uses)
                best = &g_frameProj[i];
        return best ? &best->m : nullptr;
    }
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

    // Minecraft's entities (mobs, primed TNT, particles...) as its own renderer posed them this frame:
    // vertices relative to an origin, in batches by texture (0 = the block/item atlas, else an id
    // from kRenTexture: mob skins and the like).
    struct EntityTexture
    {
        void* texture = nullptr;
        UINT  width = 0, height = 0;
    };
    std::unordered_map<std::uint32_t, EntityTexture> g_entityTextures;
    struct SceneMesh
    {
        double                        origin[3]{};
        std::vector<proto::RenBatch>  batches;
        std::vector<struct Vertex>    vertices;
    };
    SceneMesh g_scene;
    UINT      g_entityTrianglesDrawn = 0;
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
        for (auto& [id, t] : g_entityTextures)
            Release(t.texture);
        g_entityTextures.clear();
        g_scene.batches.clear();
        g_scene.vertices.clear();
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
            g_scene.batches.clear();
            g_scene.vertices.clear();
            break;
        case proto::kRenTexture:
            {
                if (bytes < sizeof(proto::RenTexture))
                    return;
                const auto* hdr = reinterpret_cast<const proto::RenTexture*>(data);
                if (!hdr->width || !hdr->height || hdr->width > 4096 || hdr->height > 4096 ||
                    bytes < sizeof(*hdr) + std::uint64_t(hdr->width) * hdr->height * 4)
                    return;
                auto& t = g_entityTextures[hdr->id];
                if (!t.texture || t.width != hdr->width || t.height != hdr->height) {
                    Release(t.texture);
                    t.texture = CreateTexture(device, hdr->width, hdr->height);
                    t.width = hdr->width;
                    t.height = hdr->height;
                }
                if (t.texture)
                    UploadRgba(t.texture, 0, 0, hdr->width, hdr->height, data + sizeof(*hdr), hdr->width * 4);
                break;
            }
        case proto::kRenScene:
            {
                g_scene.batches.clear();
                g_scene.vertices.clear();
                if (bytes < sizeof(proto::RenScene))
                    return;
                const auto* hdr = reinterpret_cast<const proto::RenScene*>(data);
                const std::uint64_t need = sizeof(*hdr) + std::uint64_t(hdr->batchCount) * sizeof(proto::RenBatch) +
                                           std::uint64_t(hdr->vertexCount) * sizeof(proto::RenVertex);
                if (!hdr->batchCount || !hdr->vertexCount || bytes < need)
                    return;
                g_scene.origin[0] = hdr->originX;
                g_scene.origin[1] = hdr->originY;
                g_scene.origin[2] = hdr->originZ;
                const auto* batches = reinterpret_cast<const proto::RenBatch*>(data + sizeof(*hdr));
                const auto* verts = reinterpret_cast<const proto::RenVertex*>(batches + hdr->batchCount);
                g_scene.vertices.resize(hdr->vertexCount);
                for (std::uint32_t b = 0; b < hdr->batchCount; ++b) {
                    const auto& batch = batches[b];
                    if (batch.first + batch.count > hdr->vertexCount || batch.count < 3)
                        continue;
                    const bool blended = (batch.flags & 1) != 0;
                    for (std::uint32_t i = batch.first; i < batch.first + batch.count; ++i) {
                        const auto& v = verts[i];
                        DWORD       color = BakeColor(v.color, v.light, v.flags);
                        if (blended)  // keep the vertex's own alpha (ghosts, slime, particles fading out)
                            color = (color & 0x00FFFFFFu) | (v.color & 0xFF000000u);
                        g_scene.vertices[i] = { v.x, v.y, v.z, color, v.u, v.v };
                    }
                    g_scene.batches.push_back(batch);
                }
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    mclog::Info("entities: first scene from Minecraft, {} triangles in {} batches", hdr->vertexCount / 3, hdr->batchCount);
                }
                break;
            }
        default:
            break;  // avatar (MP2 shows Max), lights, NPC solids, dug bits: not drawn in MP2
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
    void   DrawEntities(void* device, const proto::WorldEntities* entities);  // defined below

    void XformPoint(const float p[3], const Matrix& m, float out[4])
    {
        for (int j = 0; j < 4; ++j)
            out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
    }

    void DrawWorld(void* device, const Runtime& st, UINT w, UINT h)
    {
        if (!g_atlas || !Mapping::Get().Calibrated())
            return;
        // Our blocks live in MP2 world space and our eye is st.camera, so the view is built from it
        // directly. MP2's own view matrices are rotation-only (its scene rides on identity views with
        // the camera baked into the world matrices — "world matrices moved" in the log), so they
        // carry no eye translation and would displace every block off screen.
        Matrix view{}, proj{};
        ComputeCamera(st, view, proj, w, h);
        if (const Matrix* scene = SceneProjection())
            proj = *scene;  // the projection MP2's world was drawn with this frame
        else if (g_haveProj)
            proj = g_proj;
        static DWORD lastProjLog = 0;
        if (g_frameProjCount > 1 && GetTickCount() - lastProjLog > 5000) {
            lastProjLog = GetTickCount();
            std::string all;
            for (UINT i = 0; i < g_frameProjCount; ++i)
                all += std::format(" [{:.3f}/{:.3f} near-ish {:.3f}: {} draws]", g_frameProj[i].m.m[0][0], g_frameProj[i].m.m[1][1],
                                   g_frameProj[i].m.m[3][2], g_frameProj[i].uses);
            mclog::Info("blocks: {} projections this frame, using the busiest:{}", g_frameProjCount, all);
        }

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

        // Minecraft's entities: mobs, TNT, particles, arrows, dropped items.
        static proto::WorldEntities entities;
        const bool                  haveEntities = Link::Get().ReadWorldEntities(entities);
        VCall<dev::SetRenderState>(device, UINT(kRsSrcBlend), DWORD(kBlendSrcAlpha));
        VCall<dev::SetRenderState>(device, UINT(kRsDestBlend), DWORD(kBlendInvSrcAlpha));
        DrawEntities(device, haveEntities ? &entities : nullptr);

        // The targeted block's outline.
        if (haveEntities && entities.hasSelection) {
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

    // ---- Minecraft's entities ------------------------------------------------------------------
    // A quad TL, TR, BR, BL with the atlas rect uv {u0, v0, u1, v1}, as two triangles.
    void PushQuad(std::vector<Vertex>& out, const float p[4][3], const float uv[4], DWORD color = 0xFFFFFFFFu)
    {
        const Vertex v[4] = { { p[0][0], p[0][1], p[0][2], color, uv[0], uv[1] },
                              { p[1][0], p[1][1], p[1][2], color, uv[2], uv[1] },
                              { p[2][0], p[2][1], p[2][2], color, uv[2], uv[3] },
                              { p[3][0], p[3][1], p[3][2], color, uv[0], uv[3] } };
        for (int i : { 0, 1, 2, 0, 2, 3 })
            out.push_back(v[i]);
    }

    // A box (dropped block, crack overlay), turned about the vertical by yaw.
    void PushBox(std::vector<Vertex>& out, const float mn[3], const float size[3], float yaw, const float* side, const float* top,
                 const float* bottom, bool shaded)
    {
        const float cx = mn[0] + size[0] * 0.5f, cz = mn[2] + size[2] * 0.5f;
        const float c = std::cos(yaw), sn = std::sin(yaw);
        auto        corner = [&](int i, float o[3]) {
            const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
            o[0] = cx + lx * c - lz * sn;
            o[1] = mn[1] + ((i & 2) ? size[1] : 0.0f);
            o[2] = cz + lx * sn + lz * c;
        };
        static constexpr int   kFaces[6][4] = { { 6, 7, 5, 4 }, { 3, 2, 0, 1 }, { 7, 3, 1, 5 }, { 2, 6, 4, 0 }, { 2, 3, 7, 6 }, { 4, 5, 1, 0 } };
        static constexpr float kShade[6] = { 0.8f, 0.8f, 0.6f, 0.6f, 1.0f, 0.5f };
        for (int f = 0; f < 6; ++f) {
            float p[4][3];
            for (int k = 0; k < 4; ++k)
                corner(kFaces[f][k], p[k]);
            const auto  level = static_cast<DWORD>(255.0f * (shaded ? kShade[f] : 1.0f));
            const DWORD color = 0xFF000000u | (level << 16) | (level << 8) | level;
            PushQuad(out, p, f == 4 ? top : f == 5 ? bottom : side, color);
        }
    }

    // Minecraft's arrow model (two crossed fins and a back plate), smaller like SkyCraft draws it.
    void PushArrow(std::vector<Vertex>& out, float px, float py, float pz, const float d[3], const float* uvSide, const float* uvBack)
    {
        float s[3] = { d[2], 0.0f, -d[0] };
        float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
        if (sl < 1e-3f)
            s[0] = 1.0f, s[2] = 0.0f, sl = 1.0f;
        s[0] /= sl, s[2] /= sl;
        const float     u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
        constexpr float r = 0.70710678f;
        const float     fins[2][3] = { { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r }, { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r } };
        constexpr float k = 0.9f / 16.0f * 0.55f;
        auto            at = [&](float along, const float* q, float side, const float* q2, float side2, float o[3]) {
            const float base[3] = { px, py, pz };
            for (int i = 0; i < 3; ++i)
                o[i] = base[i] + d[i] * along + q[i] * side + (q2 ? q2[i] * side2 : 0.0f);
        };
        for (const auto& q : fins) {
            float p[4][3];
            at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
            at(4 * k, q, -2 * k, nullptr, 0, p[1]);
            at(4 * k, q, 2 * k, nullptr, 0, p[2]);
            at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
            PushQuad(out, p, uvSide);
        }
        float p[4][3];
        at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
        at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
        at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
        at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
        PushQuad(out, p, uvBack);
    }

    // Arrows, dropped items and blocks, block-breaking cracks: built from Minecraft's entity list,
    // in absolute Minecraft coordinates. Cracks go to `cracks` (blended, drawn last).
    void BuildWorldEntities(const proto::WorldEntities& entities, std::vector<Vertex>& solid, std::vector<Vertex>& cracks)
    {
        constexpr float kPi = 3.14159265f;
        for (std::uint32_t i = 0; i < entities.count; ++i) {
            const auto& e = entities.entities[i];
            switch (e.kind) {
            case proto::kWeArrow:
            case proto::kWeTrident:
                {
                    const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
                    const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
                    PushArrow(solid, e.x, e.y, e.z, d, e.uv[0], e.uv[e.kind == proto::kWeArrow ? 1 : 0]);
                    break;
                }
            case proto::kWeItem:
                {
                    const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
                    const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
                    const float p[4][3] = { { e.x - rx, e.y + half, e.z - rz },
                                            { e.x + rx, e.y + half, e.z + rz },
                                            { e.x + rx, e.y - half, e.z + rz },
                                            { e.x - rx, e.y - half, e.z - rz } };
                    PushQuad(solid, p, e.uv[0]);
                    break;
                }
            case proto::kWeBlock:
                {
                    const float sz = e.scale;
                    const float mn[3] = { e.x - sz * 0.5f, e.y - sz * 0.5f, e.z - sz * 0.5f };
                    const float size[3] = { sz, sz, sz };
                    PushBox(solid, mn, size, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], true);
                    break;
                }
            case proto::kWeCrack:
                {
                    const float mn[3] = { e.x, e.y, e.z };
                    PushBox(cracks, mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], false);
                    break;
                }
            default:
                break;  // contact shadows: not drawn
            }
        }
    }

    void DrawEntities(void* device, const proto::WorldEntities* entities)
    {
        auto&  map = Mapping::Get();
        Matrix world{};

        // Mobs, primed TNT, particles: solid batches, then blended ones.
        if (!g_scene.batches.empty()) {
            map.McToMp2Matrix(g_scene.origin[0], g_scene.origin[1], g_scene.origin[2], world.m);
            VCall<dev::SetTransform>(device, UINT(kTsWorld), static_cast<const Matrix*>(&world));
            for (int pass = 0; pass < 2; ++pass) {
                const bool blended = pass == 1;
                VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), blended ? DWORD(FALSE) : DWORD(TRUE));
                VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), blended ? DWORD(FALSE) : DWORD(TRUE));
                VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), blended ? DWORD(TRUE) : DWORD(FALSE));
                for (const auto& b : g_scene.batches) {
                    if (((b.flags & 1) != 0) != blended)
                        continue;
                    void* texture = g_atlas;
                    if (b.texture != 0) {
                        const auto it = g_entityTextures.find(b.texture);
                        if (it == g_entityTextures.end() || !it->second.texture)
                            continue;
                        texture = it->second.texture;
                    }
                    VCall<dev::SetTexture>(device, 0u, texture);
                    Checked(VCall<dev::DrawPrimitiveUP>(device, UINT(kPtTriangleList), b.count / 3, static_cast<const void*>(&g_scene.vertices[b.first]),
                                                        UINT(sizeof(Vertex))));
                    g_entityTrianglesDrawn += b.count / 3;
                }
            }
        }

        // Arrows, dropped items and blocks, cracks.
        if (entities && entities->count) {
            static std::vector<Vertex> solid, cracks;
            solid.clear();
            cracks.clear();
            BuildWorldEntities(*entities, solid, cracks);
            map.McToMp2Matrix(0.0, 0.0, 0.0, world.m);
            VCall<dev::SetTransform>(device, UINT(kTsWorld), static_cast<const Matrix*>(&world));
            VCall<dev::SetTexture>(device, 0u, g_atlas);
            if (solid.size() >= 3) {
                VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), DWORD(TRUE));
                VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), DWORD(TRUE));
                VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), DWORD(FALSE));
                Checked(VCall<dev::DrawPrimitiveUP>(device, UINT(kPtTriangleList), static_cast<UINT>(solid.size() / 3), static_cast<const void*>(solid.data()),
                                                    UINT(sizeof(Vertex))));
                g_entityTrianglesDrawn += static_cast<UINT>(solid.size() / 3);
            }
            if (cracks.size() >= 3) {
                VCall<dev::SetRenderState>(device, UINT(kRsZWriteEnable), DWORD(FALSE));
                VCall<dev::SetRenderState>(device, UINT(kRsAlphaTestEnable), DWORD(FALSE));
                VCall<dev::SetRenderState>(device, UINT(kRsAlphaBlendEnable), DWORD(TRUE));
                Checked(VCall<dev::DrawPrimitiveUP>(device, UINT(kPtTriangleList), static_cast<UINT>(cracks.size() / 3), static_cast<const void*>(cracks.data()),
                                                    UINT(sizeof(Vertex))));
            }
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

    bool ReadyForBlocks()
    {
        const Runtime& st = State();
        return !g_ownDraw && !g_blocksDrawnThisFrame && g_frameSceneDraws >= 8 && st.cameraValid && st.mcInWorld && !Config::Get().blocksNoDepth;
    }

    HRESULT __stdcall EndSceneHook(void* device)
    {
        if (g_sceneHadPerspective && ReadyForBlocks()) {
            DrawWorldAtEndScene(device);
            g_blocksDrawnThisFrame = true;
            ++g_drawnAtEndScene;
        }
        return g_origEndScene(device);
    }

    // MP2 clearing its depth buffer after the world is drawn: the blocks go in first.
    HRESULT __stdcall ClearHook(void* device, DWORD count, const void* rects, DWORD flags, DWORD color, float z, DWORD stencil)
    {
        constexpr DWORD kClearZBuffer = 2;
        if ((flags & kClearZBuffer) && ReadyForBlocks()) {
            DrawWorldAtEndScene(device);
            g_blocksDrawnThisFrame = true;
            ++g_drawnAtClear;
        }
        return g_origClear(device, count, rects, flags, color, z, stencil);
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
                        if (st.cameraValid && !g_blocksDrawnThisFrame) {
                            DrawWorld(device, st, w, h);
                            ++g_drawnAtPresent;
                        }
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
                        "triangles drawn (frames drawn before depth clear {}, at scene end {}, at present {}), {} entity triangles, {} entity "
                        "textures, atlas {}, scene z mode {}, "
                        "shader constants moved {}, our draw failures {}",
                        g_viewSets, g_viewReplaced, g_worldCorrected, g_haveProj ? "seen" : "not seen", g_sections.size(), g_trianglesDrawn,
                        g_drawnAtClear, g_drawnAtEndScene, g_drawnAtPresent, g_entityTrianglesDrawn, g_entityTextures.size(),
                        g_atlas ? "yes" : "no", g_sceneZEnable, g_constantsMoved, g_drawFailures);
            g_viewSets = g_viewReplaced = g_worldCorrected = g_trianglesDrawn = g_constantsMoved = g_drawFailures = 0;
            g_drawnAtClear = g_drawnAtEndScene = g_drawnAtPresent = g_entityTrianglesDrawn = 0;
        }
        g_haveView = g_haveProj = false;  // MP2 sets them again each frame
        g_frameSceneDraws = 0;
        g_frameProjCount = 0;
        g_blocksDrawnThisFrame = false;
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
                ++g_frameSceneDraws;
                if (perspective)
                    NoteSceneProjection();
            }
        } else if (state == kTsWorld && viewIsIdentity && perspective && st.cameraValid && haveCorrection) {
            corrected = Multiply(*matrix, correction);
            matrix = &corrected;
            ++g_worldCorrected;
            ++g_frameSceneDraws;
            NoteSceneProjection();
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
            InstallHook(VSlot(device, dev::Clear), reinterpret_cast<void*>(&ClearHook), g_origClear, "IDirect3DDevice8::Clear");
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
