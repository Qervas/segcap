#include "readback.h"

#include "log.h"

namespace segcap {
namespace {

// Subresource index for a plane of a non-arrayed, non-mipped texture:
//   planeSlice * MipLevels * ArraySize + arraySlice * MipLevels + mipSlice
// For a plain 2D depth-stencil that reduces to the plane index itself.
// Plane 0 is depth, plane 1 is stencil.
constexpr UINT kStencilPlaneSubresource = 1;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace

bool Readback::Prepare(ID3D12Device* device, ID3D12Resource* target, uint32_t planeSlice) {
    if (!device || !target) return false;

    const D3D12_RESOURCE_DESC desc = target->GetDesc();

    // Compare the LAYOUT, not the resource pointer.
    //
    // A swapchain rotates between several backbuffers, so GetBuffer() returns a
    // different ID3D12Resource* every frame. Keying on the pointer meant Prepare
    // saw a "new" target each frame, tore down the readback buffers and command
    // lists while copies were still in flight, and rebuilt them -- a
    // resource-destruction storm on the render thread that crashed the game.
    //
    // Only the layout matters for sizing the ring: same dimensions, same format,
    // same plane means the existing buffers fit. Enqueue receives the actual
    // resource to copy from, so a rotating source is handled naturally.
    if (readyForLayout_ && width_ == desc.Width && height_ == desc.Height &&
        format_ == desc.Format && planeSlice_ == planeSlice) {
        return true;
    }

    // Any in-flight copies refer to the old target, so start clean.
    ReleaseResources();
    device_ = device;
    preparedFor_ = target;
    planeSlice_ = planeSlice;
    format_ = desc.Format;
    readyForLayout_ = false;
    width_ = static_cast<uint32_t>(desc.Width);
    height_ = desc.Height;

    // Ask the runtime for the stencil plane's layout rather than computing it.
    // The row pitch is padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256), and
    // for a typeless depth-stencil the plane's element format is not obvious --
    // getting either wrong yields a sheared or garbage mask.
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    device->GetCopyableFootprints(&desc, planeSlice, 1, 0, &footprint_,
                                  &numRows, &rowSizeBytes, &requiredSize_);

    LogInfo("readback: target %ux%u, stencil plane footprint fmt=%d pitch=%u rows=%u bytes=%llu",
            width_, height_, static_cast<int>(footprint_.Footprint.Format),
            footprint_.Footprint.RowPitch, numRows, requiredSize_);

    if (requiredSize_ == 0) {
        LogError("readback: GetCopyableFootprints returned 0 bytes -- does this "
                 "format have a stencil plane at all?");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = requiredSize_;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (uint32_t i = 0; i < kRingDepth; ++i) {
        Slot& s = slots_[i];
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&s.buffer)))) {
            // Rate-limited: Prepare() is retried every frame, so an
            // unsatisfiable request logs at frame rate. One such run wrote tens
            // of thousands of identical lines and buried the election messages
            // that explained WHY it was unsatisfiable.
            static uint64_t complaints = 0;
            if (++complaints <= 3 || (complaints % 600) == 0) {
                LogError("readback: could not create readback buffer %u (%llu bytes, "
                         "occurrence %llu) -- usually means the elected target has no "
                         "such plane",
                         i, static_cast<unsigned long long>(bufDesc.Width), complaints);
            }
            ReleaseResources();
            return false;
        }
        // Each slot needs its own allocator: an allocator cannot be reset while
        // the GPU is still executing commands recorded from it, and with a ring
        // there is always something in flight.
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&s.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator,
                                             nullptr, IID_PPV_ARGS(&s.list)))) {
            LogError("readback: could not create allocator/list %u", i);
            ReleaseResources();
            return false;
        }
        s.list->Close();
        s.pending = false;
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        LogError("readback: CreateFence failed");
        ReleaseResources();
        return false;
    }

    nextFenceValue_ = 1;
    nextSlot_ = 0;
    readyForLayout_ = true;
    LogInfo("readback: ring ready (%u slots x %llu bytes)", kRingDepth, requiredSize_);
    return true;
}

bool Readback::Enqueue(ID3D12CommandQueue* queue, ID3D12Resource* target,
                       D3D12_RESOURCE_STATES currentState, uint64_t frameIndex) {
    if (!fence_ || !queue || !target) return false;

    Slot& s = slots_[nextSlot_];
    if (s.pending) {
        // The GPU is more than kRingDepth frames behind. Dropping is correct --
        // stalling to wait would be exactly the behaviour this class exists to
        // avoid -- but it must be counted, not silent.
        ++dropped_;
        return false;
    }

    if (FAILED(s.allocator->Reset())) return false;
    if (FAILED(s.list->Reset(s.allocator, nullptr))) return false;

    // Transition using the state we shadowed from ResourceBarrier and
    // CreateCommittedResource. Guessing here is a debug-layer error at best and
    // a GPU hang at worst -- and crucially we transition BACK, so the game's own
    // notion of the resource state stays true.
    const bool needsTransition = currentState != D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = target;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = currentState;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (needsTransition) s.list->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = target;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = planeSlice_;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = s.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint_;

    s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (needsTransition) {
        D3D12_RESOURCE_BARRIER back = toCopy;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back.Transition.StateAfter = currentState;
        s.list->ResourceBarrier(1, &back);
    }

    if (FAILED(s.list->Close())) return false;

    // Submitting on the game's own queue means our copy is ordered after the
    // work it already submitted this frame, with no cross-queue sync needed.
    ID3D12CommandList* lists[] = {s.list};
    queue->ExecuteCommandLists(1, lists);

    s.fenceValue = nextFenceValue_++;
    s.frameIndex = frameIndex;
    s.pending = true;
    if (FAILED(queue->Signal(fence_, s.fenceValue))) {
        s.pending = false;
        return false;
    }

    ++submitted_;
    nextSlot_ = (nextSlot_ + 1) % kRingDepth;
    return true;
}

void Readback::Drain(const MaskCallback& onMask) {
    if (!fence_) return;

    // GetCompletedValue, never SetEventOnCompletion + Wait. This function is
    // called from the render thread and must be able to do nothing at all.
    const uint64_t completed = fence_->GetCompletedValue();

    for (uint32_t i = 0; i < kRingDepth; ++i) {
        Slot& s = slots_[i];
        if (!s.pending || s.fenceValue > completed) continue;

        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(requiredSize_)};
        if (SUCCEEDED(s.buffer->Map(0, &readRange, &mapped)) && mapped) {
            MaskFrame frame;
            frame.frameIndex = s.frameIndex;
            frame.width = width_;
            frame.height = height_;
            frame.rowPitch = footprint_.Footprint.RowPitch;
            frame.format = static_cast<uint32_t>(footprint_.Footprint.Format);
            // R8_UINT stencil planes are 1 byte; a colour backbuffer is 4.
            frame.bytesPerPixel =
                (footprint_.Footprint.Format == DXGI_FORMAT_R8_UINT ||
                 footprint_.Footprint.Format == DXGI_FORMAT_R8_TYPELESS) ? 1u : 4u;
            frame.data = static_cast<const uint8_t*>(mapped);
            if (onMask) onMask(frame);
            ++delivered_;

            // Empty write range: we did not modify the buffer, and telling the
            // runtime so avoids a pointless flush.
            const D3D12_RANGE noWrite = {0, 0};
            s.buffer->Unmap(0, &noWrite);
        } else {
            LogError("readback: Map failed on slot %u", i);
        }
        s.pending = false;
    }
}

void Readback::ReleaseResources() {
    for (uint32_t i = 0; i < kRingDepth; ++i) {
        Slot& s = slots_[i];
        SafeRelease(s.list);
        SafeRelease(s.allocator);
        SafeRelease(s.buffer);
        s.pending = false;
        s.fenceValue = 0;
    }
    SafeRelease(fence_);
    preparedFor_ = nullptr;
    readyForLayout_ = false;
    width_ = height_ = 0;
    requiredSize_ = 0;
}

void Readback::Shutdown() { ReleaseResources(); }

}  // namespace segcap
