#include "Collision.h"

#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace mp2;

namespace
{
    constexpr int kRegion = 8;        // blocks per region edge (SkyCollision.REGION_SIZE)
    constexpr int kSub = 8;           // sub-voxels per block edge
    constexpr int kRadiusXZ = 4;      // regions around the player, horizontally
    constexpr int kRadiusY = 2;       // and vertically
    constexpr int kMaxQueued = 2048;

    using AllocRoomFn = X_RigidBodyRoom*(__fastcall*)(X_HavokGeometry*, const Matrix4x3&);
    AllocRoomFn g_origAllocRoom = nullptr;

    struct Tri
    {
        float v[9];
    };

    // Captured level geometry (MP2 world space), guarded by g_geoMutex.
    std::mutex                        g_geoMutex;
    std::vector<std::array<Vec3, 3>>  g_mpTris;
    std::unordered_set<std::uint64_t> g_seenRooms;
    std::atomic<std::uint32_t>        g_geoVersion{ 0 };

    // Region jobs for the worker.
    struct Job
    {
        int           rx, ry, rz;
        std::uint32_t epoch;
        bool          clear;
    };
    std::mutex                        g_jobMutex;
    std::condition_variable           g_jobCv;
    std::deque<Job>                   g_jobs;
    std::unordered_set<std::uint64_t> g_queued;  // regions queued or sent this epoch (game thread)
    std::uint32_t                     g_queuedVersion = 0;
    std::atomic<std::uint32_t>        g_epoch{ 1 };
    std::thread                       g_worker;

    std::uint64_t RegionKey(int x, int y, int z)
    {
        return (std::uint64_t(std::uint32_t(x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(y) & 0x1FFFFF) << 21) | (std::uint32_t(z) & 0x1FFFFF);
    }

    Vec3 Transform(const Vec3& p, const Matrix4x3& m)
    {
        return { p.x * m.row[0].x + p.y * m.row[1].x + p.z * m.row[2].x + m.row[3].x,
                 p.x * m.row[0].y + p.y * m.row[1].y + p.z * m.row[2].y + m.row[3].y,
                 p.x * m.row[0].z + p.y * m.row[1].z + p.z * m.row[2].z + m.row[3].z };
    }

    void Capture(X_HavokGeometry* geometry, const Matrix4x3& m)
    {
        std::uint64_t key = reinterpret_cast<std::uintptr_t>(geometry);
        for (int i = 0; i < 4; ++i)
            key = key * 1000003u ^ static_cast<std::uint64_t>(m.row[i].x * 64.0f + m.row[i].y * 4096.0f + m.row[i].z * 262144.0f);

        std::vector<std::array<Vec3, 3>> tris;
        int vertices = 0, triangles = 0;
        const bool ok = Guarded([&] {
            vertices = api.getVertexCount(geometry);
            triangles = api.getTriangleCount(geometry);
            if (vertices <= 0 || triangles <= 0 || vertices > 4'000'000 || triangles > 4'000'000)
                return;
            tris.reserve(triangles);
            for (int t = 0; t < triangles; ++t) {
                std::array<Vec3, 3> tri;
                bool                valid = true;
                for (int k = 0; k < 3 && valid; ++k) {
                    const int index = api.getVertexIndex(geometry, t, k);
                    valid = index >= 0 && index < vertices;
                    if (valid)
                        tri[k] = Transform(api.getVertex(geometry, index), m);
                }
                if (valid)
                    tris.push_back(tri);
            }
        });
        if (!ok || tris.empty()) {
            mclog::Info("room geometry {:p}: unreadable ({} vertices, {} triangles)", static_cast<void*>(geometry), vertices, triangles);
            return;
        }

        std::lock_guard lock(g_geoMutex);
        if (!g_seenRooms.insert(key).second)
            return;
        g_mpTris.insert(g_mpTris.end(), tris.begin(), tris.end());
        ++g_geoVersion;
        if (Config::Get().diagnostics)
            mclog::Info("room geometry {:p}: {} triangles (total {})", static_cast<void*>(geometry), tris.size(), g_mpTris.size());
    }

    X_RigidBodyRoom* __fastcall AllocRoom(X_HavokGeometry* geometry, const Matrix4x3& m)
    {
        X_RigidBodyRoom* room = g_origAllocRoom(geometry, m);
        if (geometry)
            Capture(geometry, m);
        return room;
    }

    // ---- triangle / box overlap (Akenine-Möller SAT) -----------------------------------------
    bool AxisTest(const float a[3], const float v0[3], const float v1[3], const float v2[3], const float h[3])
    {
        const float p0 = a[0] * v0[0] + a[1] * v0[1] + a[2] * v0[2];
        const float p1 = a[0] * v1[0] + a[1] * v1[1] + a[2] * v1[2];
        const float p2 = a[0] * v2[0] + a[1] * v2[1] + a[2] * v2[2];
        const float r = h[0] * std::fabs(a[0]) + h[1] * std::fabs(a[1]) + h[2] * std::fabs(a[2]);
        return !((std::min)({ p0, p1, p2 }) > r || (std::max)({ p0, p1, p2 }) < -r);
    }

    bool TriBoxOverlap(const float c[3], const float h[3], const Tri& t)
    {
        float v[3][3];
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 3; ++i)
                v[k][i] = t.v[k * 3 + i] - c[i];
        for (int i = 0; i < 3; ++i) {
            const float lo = (std::min)({ v[0][i], v[1][i], v[2][i] }), hi = (std::max)({ v[0][i], v[1][i], v[2][i] });
            if (lo > h[i] || hi < -h[i])
                return false;
        }
        const float e[3][3] = { { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] },
                                { v[2][0] - v[1][0], v[2][1] - v[1][1], v[2][2] - v[1][2] },
                                { v[0][0] - v[2][0], v[0][1] - v[2][1], v[0][2] - v[2][2] } };
        const float n[3] = { e[0][1] * e[1][2] - e[0][2] * e[1][1], e[0][2] * e[1][0] - e[0][0] * e[1][2], e[0][0] * e[1][1] - e[0][1] * e[1][0] };
        if (!AxisTest(n, v[0], v[1], v[2], h))
            return false;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const float unit[3] = { float(j == 0), float(j == 1), float(j == 2) };
                const float a[3] = { unit[1] * e[i][2] - unit[2] * e[i][1], unit[2] * e[i][0] - unit[0] * e[i][2], unit[0] * e[i][1] - unit[1] * e[i][0] };
                if (!AxisTest(a, v[0], v[1], v[2], h))
                    return false;
            }
        }
        return true;
    }

    // ---- worker ------------------------------------------------------------------------------
    struct Index
    {
        std::uint32_t                                        version = ~0u;
        std::uint32_t                                        epoch = ~0u;
        std::vector<Tri>                                     tris;
        std::unordered_map<std::uint64_t, std::vector<int>> regions;
    };

    void Rebuild(Index& index, std::uint32_t epoch)
    {
        std::vector<std::array<Vec3, 3>> copy;
        {
            std::lock_guard lock(g_geoMutex);
            copy = g_mpTris;
            index.version = g_geoVersion;
        }
        index.epoch = epoch;
        index.tris.clear();
        index.regions.clear();
        auto& map = Mapping::Get();
        for (const auto& mp : copy) {
            Tri t;
            for (int k = 0; k < 3; ++k) {
                const McPoint p = map.ToMc(mp[k]);
                t.v[k * 3 + 0] = static_cast<float>(p.x);
                t.v[k * 3 + 1] = static_cast<float>(p.y);
                t.v[k * 3 + 2] = static_cast<float>(p.z);
            }
            const int id = static_cast<int>(index.tris.size());
            index.tris.push_back(t);
            int lo[3], hi[3];
            for (int i = 0; i < 3; ++i) {
                const float a = (std::min)({ t.v[i], t.v[3 + i], t.v[6 + i] }), b = (std::max)({ t.v[i], t.v[3 + i], t.v[6 + i] });
                lo[i] = static_cast<int>(std::floor(a / kRegion));
                hi[i] = static_cast<int>(std::floor(b / kRegion));
            }
            if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 4096)
                continue;  // absurdly large (bad scale guess): skip rather than flood
            for (int x = lo[0]; x <= hi[0]; ++x)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int z = lo[2]; z <= hi[2]; ++z)
                        index.regions[RegionKey(x, y, z)].push_back(id);
        }
        mclog::Info("collision index: {} triangles in {} regions (epoch {})", index.tris.size(), index.regions.size(), epoch);
    }

    void SendBlocking(proto::ColType type, const std::vector<std::uint8_t>& payload)
    {
        for (int attempt = 0; attempt < 400; ++attempt) {
            if (Link::Get().WriteCollision(type, payload.data(), static_cast<std::uint32_t>(payload.size())))
                return;
            Sleep(5);  // Minecraft hasn't caught up with the ring yet
        }
        mclog::Info("collision ring stayed full; dropped a message");
    }

    void SendRegion(const Index& index, const Job& job)
    {
        const int ox = job.rx * kRegion, oy = job.ry * kRegion, oz = job.rz * kRegion;
        proto::ColRegion header{ ox, oy, oz, ox + kRegion - 1, oy + kRegion - 1, oz + kRegion - 1, job.epoch, 0 };

        static const std::vector<int> none;
        const auto                    found = index.regions.find(RegionKey(job.rx, job.ry, job.rz));
        const std::vector<int>&       ids = found != index.regions.end() ? found->second : none;

        // 1. The exact triangles.
        {
            std::vector<std::uint8_t> payload(sizeof(header) + ids.size() * sizeof(proto::ColTri));
            header.count = static_cast<std::uint32_t>(ids.size());
            std::memcpy(payload.data(), &header, sizeof(header));
            auto* out = reinterpret_cast<proto::ColTri*>(payload.data() + sizeof(header));
            for (std::size_t i = 0; i < ids.size(); ++i) {
                std::memcpy(out[i].v, index.tris[ids[i]].v, sizeof(out[i].v));
                out[i].flags = 0;
            }
            SendBlocking(proto::kColTris, payload);
        }

        // 2. Occupancy: every 1/8-block voxel a triangle passes through.
        static thread_local std::vector<proto::ColBlock> blocks(kRegion * kRegion * kRegion);
        for (int i = 0; i < kRegion * kRegion * kRegion; ++i) {
            blocks[i] = {};
            blocks[i].x = ox + i % kRegion;
            blocks[i].y = oy + (i / kRegion) % kRegion;
            blocks[i].z = oz + i / (kRegion * kRegion);
        }
        constexpr float step = 1.0f / kSub;
        const float     half[3] = { step * 0.5f, step * 0.5f, step * 0.5f };
        constexpr int   n = kRegion * kSub;  // voxels per region edge
        for (int id : ids) {
            const Tri& t = index.tris[id];
            int        lo[3], hi[3];
            const int  origin[3] = { ox, oy, oz };
            for (int i = 0; i < 3; ++i) {
                const float a = (std::min)({ t.v[i], t.v[3 + i], t.v[6 + i] }) - origin[i];
                const float b = (std::max)({ t.v[i], t.v[3 + i], t.v[6 + i] }) - origin[i];
                lo[i] = std::clamp(static_cast<int>(std::floor(a * kSub)) - 1, 0, n - 1);
                hi[i] = std::clamp(static_cast<int>(std::floor(b * kSub)) + 1, 0, n - 1);
            }
            for (int vx = lo[0]; vx <= hi[0]; ++vx)
                for (int vy = lo[1]; vy <= hi[1]; ++vy)
                    for (int vz = lo[2]; vz <= hi[2]; ++vz) {
                        const float c[3] = { ox + (vx + 0.5f) * step, oy + (vy + 0.5f) * step, oz + (vz + 0.5f) * step };
                        if (!TriBoxOverlap(c, half, t))
                            continue;
                        auto& block = blocks[(vx / kSub) + kRegion * ((vy / kSub) + kRegion * (vz / kSub))];
                        block.bits[vy % kSub] |= 1ull << ((vz % kSub) * 8 + (vx % kSub));
                    }
        }

        std::vector<std::uint8_t> payload(sizeof(header));
        std::uint32_t             count = 0;
        for (const auto& b : blocks) {
            bool any = false;
            for (auto bits : b.bits)
                any |= bits != 0;
            if (!any)
                continue;
            const auto at = payload.size();
            payload.resize(at + sizeof(proto::ColBlock));
            std::memcpy(payload.data() + at, &b, sizeof(b));
            ++count;
        }
        header.count = count;
        std::memcpy(payload.data(), &header, sizeof(header));
        SendBlocking(proto::kColRegion, payload);
    }

    void WorkerLoop()
    {
        Index index;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(g_jobMutex);
                g_jobCv.wait(lock, [] { return !g_jobs.empty(); });
                job = g_jobs.front();
                g_jobs.pop_front();
            }
            if (job.clear) {
                const std::uint32_t epoch = job.epoch;
                std::vector<std::uint8_t> payload(4);
                std::memcpy(payload.data(), &epoch, 4);
                SendBlocking(proto::kColClear, payload);
                continue;
            }
            if (job.epoch != g_epoch)
                continue;  // stale
            if (index.version != g_geoVersion || index.epoch != job.epoch)
                Rebuild(index, job.epoch);
            SendRegion(index, job);
        }
    }
}

bool collision::Install()
{
    g_worker = std::thread(WorkerLoop);
    g_worker.detach();
    return InstallHook(api.allocateRigidBodyRoom, reinterpret_cast<void*>(&AllocRoom), g_origAllocRoom, "X_RigidBodyRoom::allocateRigidBodyRoom");
}

void collision::Reset(std::uint32_t epoch)
{
    g_epoch = epoch;
    g_queued.clear();
    std::lock_guard lock(g_jobMutex);
    g_jobs.clear();
    g_jobs.push_back({ 0, 0, 0, epoch, true });
    g_jobCv.notify_one();
}

void collision::ClearGeometry()
{
    std::lock_guard lock(g_geoMutex);
    g_mpTris.clear();
    g_seenRooms.clear();
    ++g_geoVersion;
}

void collision::Update(const McPoint& player)
{
    // New geometry since we queued (rooms streamed in): send everything again.
    if (g_queuedVersion != g_geoVersion) {
        g_queuedVersion = g_geoVersion;
        g_queued.clear();
    }

    const int px = static_cast<int>(std::floor(player.x / kRegion));
    const int py = static_cast<int>(std::floor(player.y / kRegion));
    const int pz = static_cast<int>(std::floor(player.z / kRegion));
    std::vector<std::array<int, 4>> wanted;
    for (int dx = -kRadiusXZ; dx <= kRadiusXZ; ++dx)
        for (int dy = -kRadiusY; dy <= kRadiusY; ++dy)
            for (int dz = -kRadiusXZ; dz <= kRadiusXZ; ++dz) {
                const auto key = RegionKey(px + dx, py + dy, pz + dz);
                if (!g_queued.contains(key))
                    wanted.push_back({ px + dx, py + dy, pz + dz, dx * dx + 2 * dy * dy + dz * dz });
            }
    if (wanted.empty())
        return;
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a[3] < b[3]; });

    std::lock_guard lock(g_jobMutex);
    for (const auto& w : wanted) {
        if (g_jobs.size() >= kMaxQueued)
            break;
        g_queued.insert(RegionKey(w[0], w[1], w[2]));
        g_jobs.push_back({ w[0], w[1], w[2], g_epoch, false });
    }
    g_jobCv.notify_one();
}
