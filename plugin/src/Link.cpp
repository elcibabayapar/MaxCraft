#include "Link.h"

#include "Log.h"

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    template <class T>
    std::atomic_ref<T> Atomic(T& value)
    {
        return std::atomic_ref<T>(value);
    }

    constexpr std::uint64_t kMcTimeoutMs = 3000;
    constexpr std::uint64_t kGranularity = 0x10000;  // MapViewOfFile offsets must be 64 KB aligned

    // One seqlock read for both MC -> MP2 payloads, with two bounds on the retry loop. A fixed
    // attempt count is not a time bound: what those attempts cost depends on the CPU's _mm_pause
    // latency (up to 140 cycles on Skylake-SP) and on how hard the cache line is being fought over
    // while the JVM thread writes it, so a producer preempted between its odd and even sequence
    // store leaves us guessing. The clock makes the bound explicit and in microseconds, and lets
    // the 15 KB payload - about 2 us per attempt at ~8 GB/s - be given more room than the 200 byte
    // one, instead of both being a guess expressed as a number of spins. Both budgets stay far
    // inside one 16.6 ms frame, and a false return is the contract every caller already handles (the
    // last good frame is kept, the frame's entities are skipped).
    constexpr int                        kSeqlockAttempts = 64;
    constexpr std::chrono::microseconds kSeqlockBudgetSmall{ 250 };
    constexpr std::chrono::microseconds kSeqlockBudgetLarge{ 750 };

    template <class Copy>
    bool ReadSeqlock(std::atomic_ref<std::uint32_t> seq, int attempts, std::chrono::microseconds budget, Copy&& copy)
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            const std::uint32_t s1 = seq.load(std::memory_order_acquire);
            if (!(s1 & 1)) {
                copy();
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq.load(std::memory_order_relaxed) == s1)
                    return true;
            }
            _mm_pause();
            if (std::chrono::steady_clock::now() >= deadline)
                break;  // the writer is not coming back within this frame; never return a torn copy
        }
        return false;
    }

    // The overlay slot index is two bits of OverlayCtl::state, and 3 is not a slot: the three slot
    // headers end where the WaterGrid begins (kOffOverlaySlotHdr + 0x40 * 3 == 0x400), so an
    // unchecked 3 hands Render.cpp a water surface's first float as a width and a height and then
    // asks for a pixel upload of it. Both consumers clamp the same way, so the header and the
    // pixels they describe are always one and the same slot.
    std::uint32_t ClampSlot(std::uint32_t raw)
    {
        return raw < proto::kOverlaySlots ? raw : proto::kOverlaySlots - 1;
    }

    // Link::Get()'s singleton, so Shutdown() can reach it without an instance. Cleared by Teardown().
    Link* g_link = nullptr;

    // Registered by the first Create() that succeeds, as a second caller of Shutdown() beside main.cpp's
    // OnDetach. On process termination OnDetach gets there first and does the ordered teardown
    // (collision::StopWorker(), then this) - static destructors are not run at all on that path, so
    // this is a harmless duplicate and Teardown() is idempotent. On FreeLibrary it is the *only*
    // caller, because the loader lock forbids the ordered path there.
    //
    // It is constructed after Link::Get()'s own singleton (that call comes first), and function-local
    // statics are destroyed in the reverse order of their construction, so it runs while that object
    // is still alive. Nothing here locks or logs: UnmapViewOfFile and CloseHandle cannot deadlock
    // under the loader lock, and mclog::Write may already be past its own teardown.
    struct LinkTearDown
    {
        ~LinkTearDown() { Link::Shutdown(); }
    };

    // Same access rules as SkyCraft: this user, SYSTEM and administrators, at medium integrity, so a
    // game run as administrator can still be opened by a Minecraft started from the desktop.
    PSECURITY_DESCRIPTOR SharedWithThisUser()
    {
        std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
        HANDLE       token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<std::uint8_t> buffer(size);
            if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
                LPWSTR sid = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
                    sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
                    LocalFree(sid);
                }
            }
            CloseHandle(token);
        }
        sddl += L"S:(ML;;NW;;;ME)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            // Without this the mapping gets the default ACL, and a Minecraft running as this user
            // can then fail to open it with nothing but this line to explain why.
            const DWORD err = GetLastError();
            mclog::Info("shared memory: couldn't build its access rules ({}); using the defaults", err);
            return nullptr;
        }
        return descriptor;
    }
}

Link& Link::Get()
{
    static Link link;
    return link;
}

std::uint8_t* Link::MapRange(HANDLE mapping, std::uint64_t offset, std::uint64_t bytes, void*& viewBase)
{
    const std::uint64_t aligned = offset & ~(kGranularity - 1);
    const std::uint64_t delta = offset - aligned;
    viewBase = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned),
                             static_cast<SIZE_T>(bytes + delta));
    if (!viewBase) {
        // GetLastError() first: the other arguments are evaluated in an unspecified order, and
        // nothing else here is allowed to be what decides the code we report. The requested window
        // and the aligned one that actually reached the kernel are both logged, because only the
        // second one is the address MapViewOfFile refused (a 0x80 byte window logs as 0 MB).
        const DWORD err = GetLastError();
        mclog::Info("MapViewOfFile({:#x}, {} MB) failed ({}): asked for that window as {:#x}, {} MB for {} bytes",
                    offset, bytes >> 20, err, aligned, (bytes + delta) >> 20, bytes + delta);
        return nullptr;
    }
    return static_cast<std::uint8_t*>(viewBase) + delta;
}

bool Link::Create()
{
    if (Valid())
        return true;

    // One caller, on the render thread (Render.cpp's Present, behind its own linkTried flag), so
    // filling the members below cannot race another Create(). Every other thread reaches the link
    // only through the published pointers.
    const std::uint64_t size = proto::kMappingBytes;
    SECURITY_ATTRIBUTES access{ sizeof(access), SharedWithThisUser(), FALSE };
    HANDLE             mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, access.lpSecurityDescriptor ? &access : nullptr, PAGE_READWRITE,
                                                    static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), proto::kMappingName);
    // Read before LocalFree and before the logging, which may run Win32 calls of its own.
    const DWORD created = GetLastError();
    if (access.lpSecurityDescriptor)
        LocalFree(access.lpSecurityDescriptor);
    if (!mapping) {
        mclog::Info("CreateFileMapping({}) failed ({})", mclog::Utf8(proto::kMappingName), created);
        return false;
    }

    // Everything below runs on locals: collision::Install()'s worker is already running and gates
    // on Valid(), so a member published before the region is ready would let a WriteCollision land
    // in front of the memsets that wipe it - a ring whose head counts bytes the consumer has
    // already passed.
    void* lowView = nullptr;
    void* ringView = nullptr;
    std::uint8_t* low = MapRange(mapping, 0, proto::kOffOverlayPixels, lowView);
    std::uint8_t* ring = MapRange(mapping, proto::kOffRenderRing, proto::kRenderRingBytes, ringView);
    if (!low || !ring) {
        if (lowView)
            UnmapViewOfFile(lowView);
        if (ringView)
            UnmapViewOfFile(ringView);
        CloseHandle(mapping);
        return false;
    }

    // A stale mapping survives if Minecraft still has it open from an earlier run: reset our side.
    auto* header = reinterpret_cast<proto::Header*>(low + proto::kOffHeader);
    std::memset(low + proto::kOffSkyState, 0, sizeof(proto::SkyState));
    std::memset(low + proto::kOffOverlayCtl, 0, 0x100);
    // MaxCraft never writes the WaterGrid: MP2's levels have no water surface to describe, and
    // inventing one would make Minecraft drown the player in it. It still has to be zeroed, and
    // the whole gap up to the input ring with it, because this mapping can be an
    // ERROR_ALREADY_EXISTS one: SkyCraft's readWaterGrid() only gives up when seq == 0, so a stale
    // grid from a previous session would keep being read as live water.
    static_assert(sizeof(proto::WaterGrid) <= proto::kOffInputRing - proto::kOffWaterGrid, "the grid no longer fits the gap being cleared");
    std::memset(low + proto::kOffWaterGrid, 0, proto::kOffInputRing - proto::kOffWaterGrid);
    std::memset(low + proto::kOffInputRing, 0, proto::kInputRingDataOff);
    std::memset(low + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
    std::memset(low + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
    std::memset(low + proto::kOffEventRing, 0, proto::kEventRingDataOff);
    std::memset(low + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
    std::memset(ring, 0, proto::kRenRingDataOff);
    header->version = proto::kVersion;
    header->skyrimPid = GetCurrentProcessId();
    header->skyrimHeartbeatMs = GetTickCount64();
    Atomic(header->magic).store(proto::kMagic, std::memory_order_release);

    // Published only now, and only after the region is fully initialised. In this order, so that
    // acquiring the member a caller dereferences also orders every store above it before it.
    mapping_.store(mapping, std::memory_order_release);
    lowView_.store(lowView, std::memory_order_release);
    ringView_.store(ringView, std::memory_order_release);
    low_.store(low, std::memory_order_release);
    ring_.store(ring, std::memory_order_release);
    g_link = this;
    static LinkTearDown stopper;

    // The name comes from the protocol header so it cannot drift from what CreateFileMappingW was
    // given; std::format cannot format a wide string, hence mclog::Utf8.
    mclog::Info("shared memory {} ({} MB, {}), protocol v{}", mclog::Utf8(proto::kMappingName), size >> 20,
                created == ERROR_ALREADY_EXISTS ? "reused" : "created", proto::kVersion);
    return true;
}

void Link::Shutdown()
{
    if (g_link)
        g_link->Teardown();
}

void Link::Teardown()
{
    // The published pointers go first, so a producer that is not inside a call yet sees no link and
    // returns instead of writing into a view that is about to go away. One already inside a call
    // keeps the pointer it loaded, which is why Shutdown() may only be called once the producers
    // are stopped: Collision.cpp's worker has to have returned before this runs.
    low_.store(nullptr, std::memory_order_release);
    ring_.store(nullptr, std::memory_order_release);
    if (const HANDLE mapping = mapping_.exchange(nullptr, std::memory_order_acq_rel)) {
        for (auto& slot : slots_) {
            if (slot.view)
                UnmapViewOfFile(slot.view);
            slot = {};
        }
        if (void* view = lowView_.exchange(nullptr, std::memory_order_acq_rel))
            UnmapViewOfFile(view);
        if (void* view = ringView_.exchange(nullptr, std::memory_order_acq_rel))
            UnmapViewOfFile(view);
        CloseHandle(mapping);
    }
    overlayFront_ = proto::kOverlaySlots - 1;  // 2, what ResetOverlay starts from
    g_link = nullptr;
}

bool Link::McAlive() const
{
    if (!Valid())
        return false;
    const auto last = Atomic(Low<proto::Header>(proto::kOffHeader)->mcHeartbeatMs).load(std::memory_order_acquire);
    return last != 0 && GetTickCount64() - last < kMcTimeoutMs;
}

std::uint32_t Link::McPid() const
{
    return Valid() ? Atomic(Low<proto::Header>(proto::kOffHeader)->mcPid).load(std::memory_order_acquire) : 0;
}

void Link::Heartbeat()
{
    if (Valid())
        Atomic(Low<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs).store(GetTickCount64(), std::memory_order_release);
}

void Link::WriteSkyState(const proto::SkyState& state)
{
    if (!Valid())
        return;
    auto*      dst = Low<proto::SkyState>(proto::kOffSkyState);
    auto       seq = Atomic(dst->seq);
    const auto s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(reinterpret_cast<std::uint8_t*>(dst) + 4, reinterpret_cast<const std::uint8_t*>(&state) + 4, sizeof(proto::SkyState) - 4);
    seq.store(s + 2, std::memory_order_release);
}

bool Link::ReadMcState(proto::McState& out) const
{
    if (!Valid())
        return false;
    auto* src = Low<proto::McState>(proto::kOffMcState);
    return ReadSeqlock(Atomic(src->seq), kSeqlockAttempts, kSeqlockBudgetSmall,
                       [&] { std::memcpy(&out, src, sizeof(proto::McState)); });
}

void Link::PushInput(proto::InputType type, std::uint16_t code, std::int32_t a, std::int32_t b, std::int32_t c)
{
    if (!Valid())
        return;
    auto*      ring = low_.load(std::memory_order_acquire) + proto::kOffInputRing;
    auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
    auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
    auto       head = Atomic(headRef);

    // Two producers, and only MP2's accident makes them the same thread: the DirectInput hooks and
    // the causeDamage hook that reports a hurt. A relaxed load of head, a write into its slot and a
    // release store is a read-modify-write, so two overlapping pushes lose one update - and the one
    // that goes missing is an attack or a hurt Minecraft never hears about. This is the critical
    // section a compare-exchange alone cannot be: the wire gives us a single published head, and
    // whoever moves it is what hands the slot to the consumer, so the bytes have to be written
    // before it moves and only one producer may be in between. The lock is held for a few
    // instructions (a handful of events per frame) and never across a wait, so no producer can spin
    // behind a preempted peer.
    std::lock_guard lock(inputMutex_);
    const auto       h = head.load(std::memory_order_relaxed);
    const auto       t = Atomic(tailRef).load(std::memory_order_acquire);
    if (h - t >= proto::kInputRingEntries)
        return;  // Minecraft a full ring behind: drop this event, never overwrite an unread one
    auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (h & (proto::kInputRingEntries - 1));
    *entry = { static_cast<std::uint16_t>(type), code, a, b, c };
    head.store(h + 1, std::memory_order_release);
}

bool Link::WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes)
{
    if (!Valid())
        return false;
    auto*          ring = low_.load(std::memory_order_acquire) + proto::kOffCollisionRing;
    auto&          headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
    auto&          tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
    auto*          data = ring + proto::kColRingDataOff;
    constexpr auto size = proto::kColRingDataBytes;

    // The same limit and the same arithmetic Collision.cpp's kColRingLimit uses, so a payload its
    // worker accepted is never refused here - it pre-checks (hdr + bytes + 7) & ~7 against
    // kColRingDataBytes / 2 to tell a message that will never fit from one that is merely early.
    constexpr auto       limit = proto::kColRingDataBytes / 2;
    const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + std::uint64_t(bytes) + 7) & ~7ull;
    if (msgBytes > limit) {
        mclog::Info("collision message too large ({} bytes, limit {})", msgBytes, limit);
        return false;
    }
    auto       head = Atomic(headRef).load(std::memory_order_relaxed);
    const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
    auto       pos = head % size;
    const auto padBytes = (pos + msgBytes > size) ? size - pos : 0;
    if (size - (head - tail) < msgBytes + padBytes)
        return false;
    if (padBytes) {
        *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = { proto::kColPad, 0 };
        head += padBytes;
        pos = 0;
    }
    *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = { type, bytes };
    std::memcpy(data + pos + sizeof(proto::ColMsgHeader), payload, bytes);
    Atomic(headRef).store(head + msgBytes, std::memory_order_release);
    return true;
}

void Link::WriteActors(const proto::ActorRecord* records, std::uint32_t count)
{
    if (!Valid())
        return;
    auto*      table = Low<proto::ActorTable>(proto::kOffActorTable);
    auto       seq = Atomic(table->seq);
    const auto s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    count = (std::min)(count, proto::kMaxActors);
    table->count = count;
    if (count)
        std::memcpy(table->actors, records, sizeof(proto::ActorRecord) * count);
    seq.store(s + 2, std::memory_order_release);
}

bool Link::PopEvent(proto::McEvent& out)
{
    if (!Valid())
        return false;
    auto*      ring = low_.load(std::memory_order_acquire) + proto::kOffEventRing;
    auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
    auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);
    const auto head = Atomic(headRef).load(std::memory_order_acquire);
    auto       tail = Atomic(tailRef).load(std::memory_order_relaxed);
    if (tail >= head)
        return false;
    if (head - tail > proto::kEventRingEntries)
        tail = head - proto::kEventRingEntries;
    out = reinterpret_cast<const proto::McEvent*>(ring + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1)];
    Atomic(tailRef).store(tail + 1, std::memory_order_release);
    return true;
}

bool Link::ReadWorldEntities(proto::WorldEntities& out) const
{
    if (!Valid())
        return false;
    auto* src = Low<proto::WorldEntities>(proto::kOffWorldEntities);
    return ReadSeqlock(Atomic(src->seq), kSeqlockAttempts, kSeqlockBudgetLarge, [&] {
        const auto count = (std::min)(src->count, proto::kMaxWorldEntities);
        std::memcpy(&out, src, offsetof(proto::WorldEntities, entities) + sizeof(proto::WorldEntity) * count);
        out.count = count;
    });
}

void Link::DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes)
{
    std::uint8_t* ring = ring_.load(std::memory_order_acquire);
    if (!ring)
        return;
    auto&          headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
    auto&          tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);
    const auto     head = Atomic(headRef).load(std::memory_order_acquire);
    auto           tail = Atomic(tailRef).load(std::memory_order_relaxed);
    auto*          data = ring + proto::kRenRingDataOff;
    constexpr auto size = proto::kRenRingDataBytes;
    std::uint64_t  done = 0;
    while (tail < head && done < maxBytes) {
        const auto  pos = tail % size;
        const auto* hdr = reinterpret_cast<const proto::ColMsgHeader*>(data + pos);
        if (hdr->type == proto::kRenPad) {
            tail += size - pos;
            continue;
        }
        fn(hdr->type, data + pos + sizeof(proto::ColMsgHeader), hdr->payloadBytes);
        const auto msgBytes = (sizeof(proto::ColMsgHeader) + hdr->payloadBytes + 7) & ~7ull;
        tail += msgBytes;
        done += msgBytes;
    }
    Atomic(tailRef).store(tail, std::memory_order_release);
}

bool Link::AcquireOverlayFrame()
{
    if (!Valid())
        return false;
    auto& state = Low<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
    if (!(Atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty))
        return false;
    const auto old = Atomic(state).exchange(overlayFront_, std::memory_order_acq_rel);
    overlayFront_ = ClampSlot(old & 3);
    return true;
}

void Link::ResetOverlay()
{
    if (!Valid())
        return;
    Atomic(Low<proto::OverlayCtl>(proto::kOffOverlayCtl)->state).store(0, std::memory_order_release);
    overlayFront_ = proto::kOverlaySlots - 1;  // 2: Minecraft's writer starts at slot 1
}

const std::uint8_t* Link::FrontPixels(std::uint32_t width, std::uint32_t height)
{
    const HANDLE mapping = mapping_.load(std::memory_order_acquire);
    if (!mapping)
        return nullptr;
    const std::uint32_t front = ClampSlot(overlayFront_);  // the same slot FrontHeader() describes
    width = (std::min)(width, proto::kMaxOverlayW);
    height = (std::min)(height, proto::kMaxOverlayH);
    const std::uint64_t need = std::uint64_t(width) * height * 4;
    Slot&               slot = slots_[front];
    if (slot.mappedBytes < need) {
        if (slot.view)
            UnmapViewOfFile(slot.view);
        slot = {};
        slot.pixels = MapRange(mapping, proto::kOffOverlayPixels + proto::kOverlaySlotBytes * front, need, slot.view);
        slot.mappedBytes = slot.pixels ? need : 0;
    }
    return slot.pixels;
}

const proto::OverlaySlotHdr* Link::FrontHeader() const
{
    const std::uint8_t* low = low_.load(std::memory_order_acquire);
    if (!low)
        return nullptr;  // no link: the caller only reaches here after AcquireOverlayFrame() succeeded
    return reinterpret_cast<const proto::OverlaySlotHdr*>(low + proto::kOffOverlaySlotHdr +
                                                          sizeof(proto::OverlaySlotHdr) * ClampSlot(overlayFront_));
}