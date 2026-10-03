#include "Link.h"

#include "Log.h"

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <intrin.h>
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
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return nullptr;
        return descriptor;
    }
}

Link& Link::Get()
{
    static Link link;
    return link;
}

std::uint8_t* Link::MapRange(std::uint64_t offset, std::uint64_t bytes, void*& viewBase)
{
    const std::uint64_t aligned = offset & ~(kGranularity - 1);
    const std::uint64_t delta = offset - aligned;
    viewBase = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned),
                             static_cast<SIZE_T>(bytes + delta));
    if (!viewBase) {
        mclog::Info("MapViewOfFile({:#x}, {} MB) failed ({})", offset, bytes >> 20, GetLastError());
        return nullptr;
    }
    return static_cast<std::uint8_t*>(viewBase) + delta;
}

bool Link::Create()
{
    if (low_)
        return true;

    const std::uint64_t size = proto::kMappingBytes;
    SECURITY_ATTRIBUTES access{ sizeof(access), SharedWithThisUser(), FALSE };
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, access.lpSecurityDescriptor ? &access : nullptr, PAGE_READWRITE,
                                  static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), proto::kMappingName);
    const DWORD created = GetLastError();
    if (access.lpSecurityDescriptor)
        LocalFree(access.lpSecurityDescriptor);
    if (!mapping_) {
        mclog::Info("CreateFileMapping failed ({})", created);
        return false;
    }

    low_ = MapRange(0, proto::kOffOverlayPixels, lowView_);
    ring_ = MapRange(proto::kOffRenderRing, proto::kRenderRingBytes, ringView_);
    if (!low_ || !ring_) {
        if (lowView_)
            UnmapViewOfFile(lowView_);
        if (ringView_)
            UnmapViewOfFile(ringView_);
        low_ = ring_ = nullptr;
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    // A stale mapping survives if Minecraft still has it open from an earlier run: reset our side.
    auto* header = Low<proto::Header>(proto::kOffHeader);
    std::memset(low_ + proto::kOffSkyState, 0, sizeof(proto::SkyState));
    std::memset(low_ + proto::kOffOverlayCtl, 0, 0x100);
    std::memset(low_ + proto::kOffInputRing, 0, proto::kInputRingDataOff);
    std::memset(low_ + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
    std::memset(low_ + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
    std::memset(low_ + proto::kOffEventRing, 0, proto::kEventRingDataOff);
    std::memset(low_ + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
    std::memset(ring_, 0, proto::kRenRingDataOff);
    header->version = proto::kVersion;
    header->skyrimPid = GetCurrentProcessId();
    header->skyrimHeartbeatMs = GetTickCount64();
    Atomic(header->magic).store(proto::kMagic, std::memory_order_release);

    mclog::Info("shared memory Local\\SkyCraft_v1 ({} MB, {}), protocol v{}", size >> 20,
                created == ERROR_ALREADY_EXISTS ? "reused" : "created", proto::kVersion);
    return true;
}

bool Link::McAlive() const
{
    if (!low_)
        return false;
    const auto last = Atomic(Low<proto::Header>(proto::kOffHeader)->mcHeartbeatMs).load(std::memory_order_acquire);
    return last != 0 && GetTickCount64() - last < kMcTimeoutMs;
}

std::uint32_t Link::McPid() const
{
    return low_ ? Atomic(Low<proto::Header>(proto::kOffHeader)->mcPid).load(std::memory_order_acquire) : 0;
}

void Link::Heartbeat()
{
    if (low_)
        Atomic(Low<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs).store(GetTickCount64(), std::memory_order_release);
}

void Link::WriteSkyState(const proto::SkyState& state)
{
    if (!low_)
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
    if (!low_)
        return false;
    auto* src = Low<proto::McState>(proto::kOffMcState);
    auto  seq = Atomic(src->seq);
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            _mm_pause();
            continue;
        }
        std::memcpy(&out, src, sizeof(proto::McState));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == s1)
            return true;
    }
    return false;
}

void Link::PushInput(proto::InputType type, std::uint16_t code, std::int32_t a, std::int32_t b, std::int32_t c)
{
    if (!low_)
        return;
    auto*      ring = low_ + proto::kOffInputRing;
    auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
    auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
    const auto head = Atomic(headRef).load(std::memory_order_relaxed);
    const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
    if (head - tail >= proto::kInputRingEntries)
        return;
    auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (head & (proto::kInputRingEntries - 1));
    *entry = { static_cast<std::uint16_t>(type), code, a, b, c };
    Atomic(headRef).store(head + 1, std::memory_order_release);
}

bool Link::WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes)
{
    if (!low_)
        return false;
    auto*          ring = low_ + proto::kOffCollisionRing;
    auto&          headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
    auto&          tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
    auto*          data = ring + proto::kColRingDataOff;
    constexpr auto size = proto::kColRingDataBytes;

    const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
    if (msgBytes > size / 2) {
        mclog::Info("collision message too large ({} bytes)", msgBytes);
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
    if (!low_)
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
    if (!low_)
        return false;
    auto*      ring = low_ + proto::kOffEventRing;
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
    if (!low_)
        return false;
    auto* src = Low<proto::WorldEntities>(proto::kOffWorldEntities);
    auto  seq = Atomic(src->seq);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            _mm_pause();
            continue;
        }
        const auto count = (std::min)(src->count, proto::kMaxWorldEntities);
        std::memcpy(&out, src, offsetof(proto::WorldEntities, entities) + sizeof(proto::WorldEntity) * count);
        out.count = count;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == s1)
            return true;
    }
    return false;
}

void Link::DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes)
{
    if (!ring_)
        return;
    auto&          headRef = *reinterpret_cast<std::uint64_t*>(ring_ + proto::kRenRingHeadOff);
    auto&          tailRef = *reinterpret_cast<std::uint64_t*>(ring_ + proto::kRenRingTailOff);
    const auto     head = Atomic(headRef).load(std::memory_order_acquire);
    auto           tail = Atomic(tailRef).load(std::memory_order_relaxed);
    auto*          data = ring_ + proto::kRenRingDataOff;
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
    if (!low_)
        return false;
    auto& state = Low<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
    if (!(Atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty))
        return false;
    const auto old = Atomic(state).exchange(overlayFront_, std::memory_order_acq_rel);
    overlayFront_ = old & 3;
    return true;
}

void Link::ResetOverlay()
{
    if (!low_)
        return;
    Atomic(Low<proto::OverlayCtl>(proto::kOffOverlayCtl)->state).store(0, std::memory_order_release);
    overlayFront_ = 2;
}

const std::uint8_t* Link::FrontPixels(std::uint32_t width, std::uint32_t height)
{
    if (!mapping_ || overlayFront_ >= proto::kOverlaySlots)
        return nullptr;
    width = (std::min)(width, proto::kMaxOverlayW);
    height = (std::min)(height, proto::kMaxOverlayH);
    const std::uint64_t need = std::uint64_t(width) * height * 4;
    Slot&               slot = slots_[overlayFront_];
    if (slot.mappedBytes < need) {
        if (slot.view)
            UnmapViewOfFile(slot.view);
        slot = {};
        slot.pixels = MapRange(proto::kOffOverlayPixels + proto::kOverlaySlotBytes * overlayFront_, need, slot.view);
        slot.mappedBytes = slot.pixels ? need : 0;
    }
    return slot.pixels;
}

const proto::OverlaySlotHdr* Link::FrontHeader() const
{
    return Low<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * overlayFront_);
}
