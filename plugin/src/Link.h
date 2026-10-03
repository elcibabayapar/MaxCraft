#pragma once

// The shared-memory link to Minecraft, speaking SkyCraft's protocol byte for byte
// (extern/SkyCraft/protocol/skycraft_protocol.h), so SkyCraft's Fabric mod runs unchanged.
//
// SkyCraft maps the whole ~230 MB region in one view. Max Payne 2 is a 32-bit process without a
// large address space, so this maps it in pieces: the small structures and collision ring, the
// render ring, and only as much of each overlay slot as our resolution needs.

#include <skycraft_protocol.h>

#include <Windows.h>

#include <cstdint>
#include <functional>

namespace proto = skycraft::proto;

class Link
{
public:
    static Link& Get();

    bool Create();
    [[nodiscard]] bool Valid() const { return low_ != nullptr; }

    [[nodiscard]] bool          McAlive() const;
    [[nodiscard]] std::uint32_t McPid() const;
    void                        Heartbeat();

    void WriteSkyState(const proto::SkyState& state);
    bool ReadMcState(proto::McState& out) const;

    void PushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);
    // Collision ring producer (one thread only). False if the ring is full.
    bool WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes);

    void WriteActors(const proto::ActorRecord* records, std::uint32_t count);
    bool PopEvent(proto::McEvent& out);
    bool ReadWorldEntities(proto::WorldEntities& out) const;
    void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes);

    // Overlay triple buffer (consumer). Maps the slot views on demand for width x height.
    bool                                       AcquireOverlayFrame();
    void                                       ResetOverlay();
    const std::uint8_t*                        FrontPixels(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

private:
    template <class T>
    T* Low(std::uint64_t offset) const { return reinterpret_cast<T*>(low_ + offset); }

    std::uint8_t* MapRange(std::uint64_t offset, std::uint64_t bytes, void*& viewBase);

    HANDLE        mapping_{ nullptr };
    std::uint8_t* low_{ nullptr };   // [0, kOffOverlayPixels)
    std::uint8_t* ring_{ nullptr };  // the render ring
    void*         lowView_{ nullptr };
    void*         ringView_{ nullptr };

    struct Slot
    {
        void*         view{ nullptr };
        std::uint8_t* pixels{ nullptr };
        std::uint64_t mappedBytes{ 0 };
    };
    Slot          slots_[proto::kOverlaySlots];
    std::uint32_t overlayFront_{ 2 };
};
