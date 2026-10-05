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
#include <exception>
#include <memory>
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
    // 60 x 5 ms. A full ring means Minecraft is a frame or two behind, and the resend path below
    // covers the rest, so this only has to outlast a couple of frames; a long block here holds up
    // every other region queued behind it, and there are up to 405 of them.
    constexpr int kSendTries = 60;
    // A triangle covering more regions than this is a bad scale guess, not a real wall.
    constexpr int kMaxTriRegions = 4096;

    // Region keys give 21 bits per axis, offset binary: regions [-1048576, 1048575], i.e. blocks
    // +/-8.4 million, each get their own key. A coordinate outside that would alias onto a real
    // region's key and take its geometry, so it is reported and pinned to the nearest packable one.
    constexpr std::int64_t kRegionBias = 1ll << 20;
    // Link::WriteCollision refuses anything past half the ring, on the first attempt as much as on
    // the hundredth: the same arithmetic, so the worker can tell a message that will never fit from
    // one that is merely early.
    constexpr std::uint64_t kColRingLimit = proto::kColRingDataBytes / 2;

    using AllocRoomFn = X_RigidBodyRoom*(__fastcall*)(X_HavokGeometry*, const Matrix4x3&);
    AllocRoomFn g_origAllocRoom = nullptr;

    struct Tri
    {
        float v[9];
    };

    // Captured level geometry (MP2 world space). Capture appends to g_mpTris and the worker takes
    // that buffer with an O(1) swap: copying millions of triangles under the lock would block the
    // game thread inside allocateRigidBodyRoom while a level loads, exactly when it must not stall.
    std::mutex                        g_geoMutex;
    std::vector<std::array<Vec3, 3>>  g_mpTris;      // rooms captured since the worker last took it
    std::unordered_set<std::uint64_t> g_seenRooms;
    std::size_t                       g_mpTotal = 0;  // every room of this level, for the log
    std::atomic<std::uint32_t>        g_geoVersion{ 0 };
    // Bumped when the level unloads: the worker's accumulated rooms belong to the one that is leaving.
    std::atomic<std::uint32_t>        g_levelVersion{ 0 };

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

    // Set by the worker when a message didn't make it. Update reads and clears it on the game
    // thread: g_queued already holds the failed region (its key is inserted at QUEUE time), so
    // without this it would be called "sent" for the rest of the epoch and Minecraft would keep the
    // hole until the next epoch or geometry change.
    std::atomic<bool> g_resendNeeded{ false };
    // Regions the full ring refused since the last resend, and how many resends it took: the user's
    // log has to be able to say whether collision is keeping up.
    std::atomic<std::uint32_t> g_dropped{ 0 };
    std::uint32_t              g_resends = 0;

    // Regions whose payload can never fit the ring. They are reported once and never queued again:
    // retrying cannot help, and asking for them every frame would spin the worker for the rest of
    // the epoch. Cleared wherever g_queued is.
    std::mutex                        g_oversizeMutex;
    std::unordered_set<std::uint64_t> g_oversize;

    // Set when the module is unloaded: the worker polls it and returns.
    std::atomic<bool> g_stop{ false };

    // Why a message did not reach Minecraft. Only the transient reasons are worth another attempt.
    enum class SendResult
    {
        sent,
        deferred,  // the ring stayed full for the whole budget: Minecraft is behind
        tooLarge,  // past half the ring: WriteCollision rejects it now and always
        noLink,    // Link::Create waits for MP2's first Present, so there is no ring yet
    };

    // splitmix64's finalizer: every input gives a different output, so two rooms that differ in any
    // matrix component cannot land on one key. The old key summed three scaled floats into a uint64
    // (negative floats became enormous numbers) and threw away the precision that separates two
    // rooms a metre apart, so a false positive threw away a whole room's triangles.
    std::uint64_t Mix(std::uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    // 21 bits per axis, offset binary. Room-to-world transforms are exact translations of a few
    // metres, so rounding to 1/64 unit separates distinct rooms without splitting one room in two.
    std::uint64_t RoomKey(X_HavokGeometry* geometry, const Matrix4x3& m)
    {
        constexpr double kGrid = 64.0;
        std::uint64_t   h = 0;
        for (int i = 0; i < 4; ++i) {
            const float component[3] = { m.row[i].x, m.row[i].y, m.row[i].z };
            for (int k = 0; k < 3; ++k) {
                const double        quantized = std::nearbyint(static_cast<double>(component[k]) * kGrid);
                const std::int64_t  cell = static_cast<std::int64_t>(quantized);
                h = Mix(h ^ static_cast<std::uint64_t>(cell));
            }
        }
        return Mix(h ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(geometry)));
    }

    // Once per level, say that a coordinate could not be packed instead of letting it alias onto
    // another region's key: this line is the only way the user learns their mapping or world
    // position went outside what the keys can hold.
    void NoteRange(const char* what, double value, double limit)
    {
        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true, std::memory_order_relaxed))
            mclog::Info("collision: {} {:.0f} is outside the +/-{:.0f} these keys can hold; collision there is misplaced",
                        what, value, limit);
    }

    std::uint64_t RegionKey(int x, int y, int z)
    {
        const int      axis[3] = { x, y, z };
        std::uint64_t key = 0;
        for (int i = 0; i < 3; ++i) {
            const std::int64_t raw = std::int64_t(axis[i]);
            if (raw < -kRegionBias || raw >= kRegionBias)
                NoteRange("region coordinate", double(raw), double(kRegionBias));
            const std::int64_t packed = std::clamp<std::int64_t>(raw + kRegionBias, 0, 2ll * kRegionBias - 1);
            key = (key << 21) | static_cast<std::uint64_t>(packed);
        }
        return key;
    }

    // Both axes get all 32 bits, and the cast from a negative int keeps the sign in the top bit, so
    // every block column in the int range has its own key: -1 and 4294967295 are two billion blocks
    // apart rather than the same column.
    std::uint64_t ColumnKey(int x, int z)
    {
        return (std::uint64_t(std::uint32_t(x)) << 32) | std::uint32_t(z);
    }

    // std::floor of a coordinate, with the cast to int guarded: it is undefined outside the int
    // range, and a coordinate that big cannot come out of a Minecraft world anyway, so it is reported
    // and pinned to the nearest value the keys can still hold.
    int FloorCoord(const char* what, double value, double limit)
    {
        if (!(value >= -limit && value <= limit)) {
            NoteRange(what, value, limit);
            return std::isnan(value) ? 0 : (value < 0 ? -int(limit) : int(limit));
        }
        return static_cast<int>(std::floor(value));
    }

    // The region a Minecraft coordinate falls in. Dividing before flooring is what puts a negative
    // coordinate in the region below it, exactly as the ring always has; only the cast is new.
    int RegionCoord(double value)
    {
        return FloorCoord("region coordinate", value / double(kRegion), double(kRegionBias) - 1.0);
    }

    int BlockCoord(double value)
    {
        return FloorCoord("block coordinate", value, 2147483000.0);
    }

    // proto::ColTri::flags. SkyCraft decides these from Skyrim's Havok collidables: the collision
    // layer of the collidable, the TESObjectREFR that owns it (the land has none), the shape's
    // material id, and for the land the material painted into the terrain texture. MP2's
    // X_HavokGeometry is a flat vertex/index mesh handed to allocateRigidBodyRoom with nothing but a
    // room-to-world matrix - no collision layer, no owner, no per-triangle material - and none of
    // its material accessors are bound, so there is no honest way to set any of the four bits.
    // Guessing would be worse than leaving them clear: kTriDiggable is what lets Minecraft dig a
    // wall, so a wrong guess here hands it the level. They stay 0 until the engine can answer.
    constexpr std::uint32_t kTriFlagsNone = 0;

    void LogTriangleFlags()
    {
        mclog::Info("collision: every triangle is sent with no flags: MP2's X_HavokGeometry has no collision "
                    "layer, no owning object and no per-triangle material, so kTriStairHelper, kTriDiggable "
                    "(+ DigMaterial), kTriGhost and kTriTerrain all stay clear (Minecraft may not dig MP2's "
                    "geometry)");
    }

    Vec3 Transform(const Vec3& p, const Matrix4x3& m)
    {
        return { p.x * m.row[0].x + p.y * m.row[1].x + p.z * m.row[2].x + m.row[3].x,
                 p.x * m.row[0].y + p.y * m.row[1].y + p.z * m.row[2].y + m.row[3].y,
                 p.x * m.row[0].z + p.y * m.row[1].z + p.z * m.row[2].z + m.row[3].z };
    }

    void Capture(X_HavokGeometry* geometry, const Matrix4x3& m)
    {
        const std::uint64_t key = RoomKey(geometry, m);

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
        if (g_seenRooms.size() == 1)
            LogTriangleFlags();  // once per level
        g_mpTris.insert(g_mpTris.end(), tris.begin(), tris.end());
        g_mpTotal += tris.size();
        g_geoVersion.fetch_add(1, std::memory_order_relaxed);
        if (Config::Get().diagnostics)
            mclog::Info("room geometry {:p}: {} triangles (total {})", static_cast<void*>(geometry), tris.size(), g_mpTotal);
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

    // ---- block columns -----------------------------------------------------------------------
    // The block columns MP2's geometry stands in. Built by the worker and published as a finished
    // set: InsideLevel asks every frame on the game thread, which must not rasterise a level's
    // triangles there. The game thread only ever copies the shared_ptr and reads it lock free.
    struct Columns
    {
        std::uint32_t                     level = 0;
        std::unordered_set<std::uint64_t> keys;
    };
    std::mutex                     g_columnsMutex;
    std::shared_ptr<const Columns> g_columns;

    // ---- worker ------------------------------------------------------------------------------
    struct Index
    {
        std::uint32_t                                        version = ~0u;
        std::uint32_t                                        epoch = ~0u;
        std::uint32_t                                        level = ~0u;
        std::vector<Tri>                                     tris;
        std::unordered_map<std::uint64_t, std::vector<int>> regions;
    };

    // The worker's own copy of every room's triangles. Capture hands over one generation at a time,
    // so the rooms from before the last rebuild live here: otherwise a rebuild triggered by a single
    // streamed room would lose every room the previous rebuild had indexed.
    std::vector<std::array<Vec3, 3>> g_workerTris;

    void BuildColumns(const Index& index, std::uint32_t level)
    {
        auto columns = std::make_shared<Columns>();
        columns->level = level;
        for (const auto& tri : index.tris) {
            double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
            for (int k = 0; k < 3; ++k) {
                lo[0] = (std::min)(lo[0], double(tri.v[k * 3 + 0])), hi[0] = (std::max)(hi[0], double(tri.v[k * 3 + 0]));
                lo[1] = (std::min)(lo[1], double(tri.v[k * 3 + 2])), hi[1] = (std::max)(hi[1], double(tri.v[k * 3 + 2]));
            }
            const int x0 = BlockCoord(lo[0]), x1 = BlockCoord(hi[0]);
            const int z0 = BlockCoord(lo[1]), z1 = BlockCoord(hi[1]);
            const std::int64_t wide = std::int64_t(x1) - x0 + 1, deep = std::int64_t(z1) - z0 + 1;
            if (wide <= 0 || deep <= 0 || wide * deep > 65536)
                continue;  // absurdly wide (bad scale guess): skip rather than flood
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z)
                    columns->keys.insert(ColumnKey(x, z));
        }
        {
            std::lock_guard lock(g_columnsMutex);
            g_columns = columns;
        }
        mclog::Info("level footprint: {} block columns", columns->keys.size());
    }

    void Rebuild(Index& index, std::uint32_t epoch)
    {
        // Take the buffer the game thread has been filling, don't copy it: it is the only moment the
        // two threads meet, and it must stay O(1) or level loading stalls on it.
        std::vector<std::array<Vec3, 3>> fresh;
        std::uint32_t                     version = 0, level = 0;
        {
            std::lock_guard lock(g_geoMutex);
            fresh.swap(g_mpTris);
            version = g_geoVersion.load(std::memory_order_relaxed);
            level = g_levelVersion.load(std::memory_order_relaxed);
        }
        index.version = version;
        index.epoch = epoch;
        if (index.level != level) {
            g_workerTris.clear();  // the level those rooms came from is unloading
            index.level = level;
        }
        g_workerTris.insert(g_workerTris.end(), fresh.begin(), fresh.end());

        index.tris.clear();
        index.regions.clear();
        int skipped = 0;
        auto&       map = Mapping::Get();
        for (const auto& mp : g_workerTris) {
            Tri t;
            for (int k = 0; k < 3; ++k) {
                const McPoint p = map.ToMc(mp[k]);
                t.v[k * 3 + 0] = static_cast<float>(p.x);
                t.v[k * 3 + 1] = static_cast<float>(p.y);
                t.v[k * 3 + 2] = static_cast<float>(p.z);
            }
            int  lo[3], hi[3];
            bool usable = true;
            for (int i = 0; i < 3 && usable; ++i) {
                usable = std::isfinite(t.v[i]) && std::isfinite(t.v[3 + i]) && std::isfinite(t.v[6 + i]);
                if (!usable)
                    break;
                lo[i] = RegionCoord((std::min)({ t.v[i], t.v[3 + i], t.v[6 + i] }));
                hi[i] = RegionCoord((std::max)({ t.v[i], t.v[3 + i], t.v[6 + i] }));
            }
            // Checked before the triangle is stored: one that covers thousands of regions is a bad
            // scale guess, and keeping it would cost memory for the whole epoch without any region
            // ever referring to it.
            std::int64_t span = 1;
            bool         tooWide = false;
            for (int i = 0; i < 3 && usable && !tooWide; ++i)
                tooWide = (span *= std::int64_t(hi[i]) - lo[i] + 1) > kMaxTriRegions;
            if (!usable || tooWide) {
                ++skipped;
                continue;
            }
            const int id = static_cast<int>(index.tris.size());
            index.tris.push_back(t);
            for (int x = lo[0]; x <= hi[0]; ++x)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int z = lo[2]; z <= hi[2]; ++z)
                        index.regions[RegionKey(x, y, z)].push_back(id);
        }
        // Give the big buffer back so the next room's capture reuses its memory instead of
        // reallocating a fresh one for every streamed room.
        //
        // Only when nothing was captured while we were indexing. Capture() appends to g_mpTris under
        // this same mutex, and it runs on the game thread during level loading, which is exactly when
        // a rebuild happens - so the vector is usually NOT empty here. Swapping a populated g_mpTris
        // would move those just-captured triangles into `fresh`, which dies with this scope: whole
        // rooms would vanish from the index, and the batch we just indexed would sit in g_mpTris to
        // be taken and appended to g_workerTris a second time on the next rebuild. Both are silent,
        // and the duplication compounds once per streamed room.
        {
            std::lock_guard lock(g_geoMutex);
            if (g_mpTris.empty() && g_mpTris.capacity() < fresh.capacity())
                g_mpTris.swap(fresh);
        }
        mclog::Info("collision index: {} triangles in {} regions (epoch {})", index.tris.size(), index.regions.size(), epoch);
        if (skipped)
            mclog::Info("collision index: {} triangles skipped (unusable, or spanning more than {} regions)", skipped, kMaxTriRegions);
        BuildColumns(index, level);
    }

    SendResult SendBlocking(proto::ColType type, const std::vector<std::uint8_t>& payload)
    {
        auto& link = Link::Get();
        if (!link.Valid()) {
            // Link::Create is deferred to MP2's first Present, and Update can reach the worker long
            // before that: burning the whole budget here would only log a misleading "ring stayed
            // full", so leave immediately and let the resend flag bring the region back.
            static std::atomic<bool> logged{ false };
            if (!logged.exchange(true, std::memory_order_relaxed))
                mclog::Info("collision: shared memory link not mapped yet (it is mapped at MP2's first frame); collision waits");
            return SendResult::noLink;
        }
        const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + payload.size() + 7) & ~7ull;
        if (msgBytes > kColRingLimit) {
            // Reported by the caller, which also knows the region: from here on this message would
            // fail identically on every attempt, so the retry loop must not be entered.
            return SendResult::tooLarge;
        }
        for (int attempt = 0; attempt < kSendTries; ++attempt) {
            if (g_stop.load(std::memory_order_relaxed))
                return SendResult::deferred;  // the module is going away: don't hold teardown up
            if (link.WriteCollision(type, payload.data(), static_cast<std::uint32_t>(payload.size())))
                return SendResult::sent;
            Sleep(5);  // Minecraft hasn't caught up with the ring yet
        }
        return SendResult::deferred;
    }

    // One message of one region. A full ring is worth retrying (the whole ring goes back into the
    // queue); a message the ring can never hold, or one written before the link existed, is not.
    bool SendRegionMessage(proto::ColType type, const std::vector<std::uint8_t>& payload, int rx, int ry, int rz)
    {
        switch (SendBlocking(type, payload)) {
        case SendResult::sent:
            return true;
        case SendResult::tooLarge: {
            const std::uint64_t key = RegionKey(rx, ry, rz);
            std::lock_guard     lock(g_oversizeMutex);
            if (g_oversize.insert(key).second)
                mclog::Info("collision: region ({}, {}, {}) needs {} bytes, past the ring's {} byte limit: it can never be "
                            "sent and is left out",
                            rx, ry, rz, payload.size(), kColRingLimit);
            return false;
        }
        case SendResult::noLink:
            g_resendNeeded.store(true, std::memory_order_relaxed);
            return false;
        default: {
            const std::uint32_t dropped = g_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
            g_resendNeeded.store(true, std::memory_order_relaxed);
            if (dropped <= 3 || dropped % 64 == 0)
                mclog::Info("collision: region ({}, {}, {}) didn't fit the ring ({} dropped since the last resend)",
                            rx, ry, rz, dropped);
            return false;
        }
        }
    }

    void SendRegion(const Index& index, const Job& job)
    {
        const int ox = job.rx * kRegion, oy = job.ry * kRegion, oz = job.rz * kRegion;
        proto::ColRegion header{ ox, oy, oz, ox + kRegion - 1, oy + kRegion - 1, oz + kRegion - 1, job.epoch, 0 };

        static const std::vector<int> none;
        const auto                    found = index.regions.find(RegionKey(job.rx, job.ry, job.rz));
        const std::vector<int>&       ids = found != index.regions.end() ? found->second : none;

        // 1. The exact triangles. A region is one unit on the Java side (a kColRegion replaces the
        // shapes inside its box), so the two halves go together: giving up after the first failure
        // is cheaper and never leaves Minecraft's block masks describing a different world than its
        // triangles. Update re-queues the whole ring afterwards.
        {
            std::vector<std::uint8_t> payload(sizeof(header) + ids.size() * sizeof(proto::ColTri));
            header.count = static_cast<std::uint32_t>(ids.size());
            std::memcpy(payload.data(), &header, sizeof(header));
            auto* out = reinterpret_cast<proto::ColTri*>(payload.data() + sizeof(header));
            for (std::size_t i = 0; i < ids.size(); ++i) {
                std::memcpy(out[i].v, index.tris[ids[i]].v, sizeof(out[i].v));
                out[i].flags = kTriFlagsNone;
            }
            if (!SendRegionMessage(proto::kColTris, payload, job.rx, job.ry, job.rz))
                return;
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

        std::vector<std::uint8_t> payload;
        payload.reserve(sizeof(header) + blocks.size() * sizeof(proto::ColBlock));
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
        SendRegionMessage(proto::kColRegion, payload, job.rx, job.ry, job.rz);
    }

    void WorkerLoop()
    {
        Index index;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(g_jobMutex);
                g_jobCv.wait(lock, [] { return !g_jobs.empty() || g_stop.load(std::memory_order_relaxed); });
                if (g_stop.load(std::memory_order_relaxed))
                    return;  // asked to stop: the module is being unloaded
                job = g_jobs.front();
                g_jobs.pop_front();
            }
            // A std::bad_alloc from a region's payload would reach std::terminate and take the game
            // process with it, so every job is attempted inside the loop.
            try {
                if (job.clear) {
                    std::vector<std::uint8_t> payload(4);
                    std::memcpy(payload.data(), &job.epoch, 4);
                    if (SendBlocking(proto::kColClear, payload) != SendResult::sent) {
                        // Minecraft resets everything on this epoch number, and it never arrived: put
                        // it back at the head of the queue, or the regions resent below would be
                        // applied to the epoch they were just cleared out of.
                        Sleep(5);
                        std::lock_guard lock(g_jobMutex);
                        g_jobs.push_front(job);
                        continue;
                    }
                    continue;
                }
                if (job.epoch != g_epoch)
                    continue;  // stale
                if (index.version != g_geoVersion || index.epoch != job.epoch)
                    Rebuild(index, job.epoch);
                SendRegion(index, job);
            } catch (const std::bad_alloc&) {
                // An out-of-memory region must not take the game's process down, and the ring has
                // to be rebuilt anyway: another pass over the same geometry may fit.
                mclog::Info("collision worker: out of memory sending a region; the ring will be sent again");
                g_resendNeeded.store(true, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                mclog::Info("collision worker: {}", e.what());
                g_resendNeeded.store(true, std::memory_order_relaxed);
            } catch (...) {
                mclog::Info("collision worker: unknown error");
                g_resendNeeded.store(true, std::memory_order_relaxed);
            }
        }
    }

    // Asks the worker to stop when the module is unloaded. DllMain only ever hears
    // DLL_PROCESS_ATTACH, and main.cpp is not ours to change, so a static destructor is the only
    // hook we have. It only sets a flag: joining a thread under the loader lock is not allowed, and
    // SendBlocking polls the flag, so the worker is gone within one message.
    struct WorkerStopper
    {
        ~WorkerStopper() { g_stop.store(true, std::memory_order_relaxed); }
    };
}

bool collision::Install()
{
    static WorkerStopper stopper;  // registered before the thread starts
    g_worker = std::thread(WorkerLoop);
    g_worker.detach();
    return InstallHook(api.allocateRigidBodyRoom, reinterpret_cast<void*>(&AllocRoom), g_origAllocRoom, "X_RigidBodyRoom::allocateRigidBodyRoom");
}

void collision::Reset(std::uint32_t epoch)
{
    g_epoch = epoch;
    g_resendNeeded.store(false, std::memory_order_relaxed);
    g_dropped.store(0, std::memory_order_relaxed);
    g_queued.clear();
    {
        std::lock_guard lock(g_oversizeMutex);
        g_oversize.clear();
    }
    {
        // The epoch can come with a recalibrated mapping, so the footprint behind InsideLevel is
        // stale until the worker builds the next one. Until then it answers "inside", which is the
        // safe direction: outside MP2's level it kills Max.
        std::lock_guard lock(g_columnsMutex);
        g_columns.reset();
    }
    std::lock_guard lock(g_jobMutex);
    g_jobs.clear();
    g_jobs.push_back({ 0, 0, 0, epoch, true });
    g_jobCv.notify_one();
}

bool collision::InsideLevel(const McPoint& p)
{
    std::shared_ptr<const Columns> columns;
    {
        std::lock_guard lock(g_columnsMutex);
        columns = g_columns;
    }
    // True while the worker's footprint isn't built yet, or belongs to the level that is unloading:
    // MP2 kills Max outside its own geometry, so an unknown world has to be treated as walkable.
    if (!columns || columns->level != g_levelVersion.load(std::memory_order_relaxed) || columns->keys.empty())
        return true;
    const int x = BlockCoord(p.x), z = BlockCoord(p.z);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
            if (columns->keys.contains(ColumnKey(x + dx, z + dz)))
                return true;
    return false;
}

void collision::ClearGeometry()
{
    {
        std::lock_guard lock(g_geoMutex);
        g_mpTris.clear();
        g_seenRooms.clear();
        g_mpTotal = 0;
        g_geoVersion.fetch_add(1, std::memory_order_relaxed);
        g_levelVersion.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(g_columnsMutex);
        g_columns.reset();
    }
}

void collision::Update(const McPoint& player)
{
    // A message didn't reach Minecraft, but its region key is already in g_queued: put the whole
    // ring back in the queue so those regions are asked for again. Same recovery as new geometry
    // below, and both are safe because only this thread touches g_queued.
    if (g_resendNeeded.exchange(false, std::memory_order_relaxed)) {
        const std::uint32_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
        mclog::Info("collision: ring resend {} ({} regions dropped: Minecraft is behind)", ++g_resends, dropped);
        g_queued.clear();
    }

    // New geometry since we queued (rooms streamed in): send everything again.
    if (g_queuedVersion != g_geoVersion) {
        g_queuedVersion = g_geoVersion;
        g_queued.clear();
        {
            std::lock_guard lock(g_oversizeMutex);
            g_oversize.clear();
        }
    }

    const int px = RegionCoord(player.x);
    const int py = RegionCoord(player.y);
    const int pz = RegionCoord(player.z);
    std::vector<std::array<int, 4>> wanted;
    {
        // Taken once rather than per region: this runs every frame, and a region that can never fit
        // the ring must not be queued again, or the worker would spin on it for the rest of the epoch.
        std::lock_guard lock(g_oversizeMutex);
        for (int dx = -kRadiusXZ; dx <= kRadiusXZ; ++dx)
            for (int dy = -kRadiusY; dy <= kRadiusY; ++dy)
                for (int dz = -kRadiusXZ; dz <= kRadiusXZ; ++dz) {
                    const auto key = RegionKey(px + dx, py + dy, pz + dz);
                    if (!g_queued.contains(key) && !g_oversize.contains(key))
                        wanted.push_back({ px + dx, py + dy, pz + dz, dx * dx + 2 * dy * dy + dz * dz });
                }
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
