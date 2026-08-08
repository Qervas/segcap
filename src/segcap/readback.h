// readback.h -- asynchronous GPU->CPU copy of a depth target's stencil plane.
//
// The naive version of this is one line: copy the resource, Map it, read it.
// That stalls the render thread for a full GPU frame every frame, which would
// be plainly visible to the player and would make any frame-time measurement
// meaningless.
//
// So instead:
//   - our own command allocator per ring slot, our own command list, our own
//     fence. We never touch the game's allocators, which it is still using.
//   - the copy is recorded and submitted on the GAME's queue, so GPU ordering
//     against the game's own work is guaranteed without any synchronisation.
//   - Map happens only after the fence says the copy landed, checked with
//     GetCompletedValue. There is no Wait anywhere in this file, by design.
//   - a 3-deep ring means a copy issued on frame N is read on frame N+2 or
//     later, and the render thread never waits on the GPU.
//
// The stencil plane is copied on its own, as PlaneSlice=1 of the depth-stencil
// resource. That is the D3D12 capability with no D3D11 equivalent: in D3D11 the
// only option is copying the whole interleaved D24S8 surface and masking bytes
// on the CPU. Here we move one byte per pixel instead of four or eight.

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace segcap {

// Delivered once a copy has completed and been read back. `data` points at the
// stencil plane with `rowPitch` bytes per row, which is padded up to
// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and is therefore usually wider than
// `width`. Reading it as tightly packed is the classic way to get a sheared
// image; the callback receives the pitch so it does not have to guess.
struct MaskFrame {
    uint64_t frameIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    uint32_t bytesPerPixel = 1;   // 1 for a stencil plane, 4 for a colour target
    uint32_t format = 0;          // DXGI_FORMAT, so callers can decode correctly
    const uint8_t* data = nullptr;
};

using MaskCallback = std::function<void(const MaskFrame&)>;

class Readback {
public:
    static constexpr uint32_t kRingDepth = 3;

    // Prepares ring resources for a specific target. Safe to call repeatedly;
    // reinitialises if the target or its dimensions changed (resolution change
    // in the host would otherwise silently produce garbage).
    //
    // `planeSlice` selects the subresource: 1 for the stencil plane of a
    // depth-stencil resource, 0 for a plain colour target such as the
    // backbuffer. Parameterised because the same ring machinery serves both --
    // and capturing the colour frame in the SAME Present call as the mask is
    // what keeps them on one frame grid by construction, rather than requiring
    // timestamp matching afterwards.
    bool Prepare(ID3D12Device* device, ID3D12Resource* target, uint32_t planeSlice = 1);

    // Records and submits a copy of the current stencil plane. Returns false if
    // no ring slot is free, which means the GPU is more than kRingDepth frames
    // behind -- worth logging rather than silently dropping.
    bool Enqueue(ID3D12CommandQueue* queue, ID3D12Resource* target,
                 D3D12_RESOURCE_STATES currentState, uint64_t frameIndex);

    // Delivers any completed copies. Never blocks: slots whose fence has not
    // signalled are simply left for a later call.
    void Drain(const MaskCallback& onMask);

    void Shutdown();

    uint64_t submitted() const { return submitted_; }
    uint64_t delivered() const { return delivered_; }
    uint64_t dropped() const { return dropped_; }

private:
    struct Slot {
        ID3D12Resource* buffer = nullptr;       // D3D12_HEAP_TYPE_READBACK
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        uint64_t fenceValue = 0;
        uint64_t frameIndex = 0;
        bool pending = false;
    };

    void ReleaseResources();

    Slot slots_[kRingDepth];
    ID3D12Fence* fence_ = nullptr;
    ID3D12Device* device_ = nullptr;
    ID3D12Resource* preparedFor_ = nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_ = {};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t planeSlice_ = 1;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    // Ring is sized for a LAYOUT, not a specific resource -- see Prepare.
    bool readyForLayout_ = false;
    uint64_t requiredSize_ = 0;

    uint64_t nextFenceValue_ = 1;
    uint32_t nextSlot_ = 0;

    uint64_t submitted_ = 0;
    uint64_t delivered_ = 0;
    uint64_t dropped_ = 0;
};

}  // namespace segcap
