#pragma once

// The shared-memory link to Minecraft, speaking SkyCraft's protocol byte for byte
// (extern/SkyCraft/protocol/skycraft_protocol.h), so SkyCraft's Fabric mod runs unchanged.
//
// The mapping is proto::kMappingBytes = 200,327,168 bytes = 191.06 MiB (200.3 MB), not the
// "~230 MB" the design notes claim. SkyCraft maps the whole region in one view. Max Payne 2 is a
// 32-bit process without a large address space, so this maps it in pieces: the small structures
// and collision ring, the render ring, and only as much of each overlay slot as our resolution
// needs.

#include <skycraft_protocol.h>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace proto = skycraft::proto;

class Link
{
public:
    static Link& Get();

    bool Create();
    // Unmaps every view, closes the handle and clears the members, so a later Create() starts from
    // nothing. Idempotent, and safe to call while the module goes away: no lock, no log line. Only
    // call it once the producers have stopped - Collision.cpp's worker has to have returned, or it
    // can still be inside a call holding the unmapped view.
    static void Shutdown();
    [[nodiscard]] bool Valid() const { return low_.load(std::memory_order_acquire) != nullptr; }

    [[nodiscard]] bool          McAlive() const;
    [[nodiscard]] std::uint32_t McPid() const;
    void                        Heartbeat();

    void WriteSkyState(const proto::SkyState& state);
    bool ReadMcState(proto::McState& out) const;

    // Input ring producer. Two producers reach this: MP2's DirectInput hooks and the causeDamage
    // hook on the game thread. They are the same thread only by MP2's accident, so the ring is
    // taken as a critical section; the event is dropped if Minecraft has fallen a full ring behind.
    void PushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);
    // Collision ring producer (one thread only). False if the ring is full.
    bool WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes);

    void WriteActors(const proto::ActorRecord* records, std::uint32_t count);
    bool PopEvent(proto::McEvent& out);
    bool ReadWorldEntities(proto::WorldEntities& out) const;
    void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes);

    // Overlay triple buffer (consumer). Maps the slot views on demand for width x height. The slot
    // index is the low two bits of the shared control word, which nothing in the protocol validates,
    // so both of these clamp it to a real slot and always describe the same one.
    bool                                       AcquireOverlayFrame();
    void                                       ResetOverlay();
    const std::uint8_t*                        FrontPixels(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

private:
    // Undoes Create(). Only Shutdown() calls it, once every other thread is gone.
    void Teardown();

    // Acquire: Create()'s release store is what makes its memsets and header visible to whoever sees
    // the pointer, so a caller that loads this has a region that is ready to be written.
    template <class T>
    T* Low(std::uint64_t offset) const { return reinterpret_cast<T*>(low_.load(std::memory_order_acquire) + offset); }

    std::uint8_t* MapRange(HANDLE mapping, std::uint64_t offset, std::uint64_t bytes, void*& viewBase);

    // Published by Create(), and only once the region behind them is fully initialised: the
    // collision worker starts before the link exists and gates on Valid(), so a pointer it can see
    // must already describe memory it may write. Stores run in this order, so acquiring the member
    // you dereference also orders the earlier ones.
    std::atomic<HANDLE>        mapping_{ nullptr };
    std::atomic<std::uint8_t*> low_{ nullptr };   // [0, kOffOverlayPixels)
    std::atomic<std::uint8_t*> ring_{ nullptr };  // the render ring
    std::atomic<void*>         lowView_{ nullptr };
    std::atomic<void*>         ringView_{ nullptr };

    // PushInput only. Held for the few instructions between claiming an entry and publishing it.
    std::mutex inputMutex_;

    struct Slot
    {
        void*         view{ nullptr };
        std::uint8_t* pixels{ nullptr };
        std::uint64_t mappedBytes{ 0 };
    };
    Slot          slots_[proto::kOverlaySlots];  // render thread only
    std::uint32_t overlayFront_{ 2 };            // render thread only
};