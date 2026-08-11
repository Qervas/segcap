#include "hooks.h"

#include <set>

#include "hooks_format.h"
#include "log.h"

namespace segcap {
// Enhanced Barriers detour. Records what the LEGACY path can no longer see.
//
// Per the D3D12 spec the runtime translates legacy ResourceBarrier calls into
// enhanced ones internally, and an application written against Barrier() never
// calls ResourceBarrier at all. Our state shadow is built entirely from
// ResourceBarrier, so against such an application it observes nothing and every
// StateBefore we declare is invented -- which is what the validation layer
// caught on inZOI (we said PIXEL_SHADER_RESOURCE, the runtime knew
// UNORDERED_ACCESS).
void STDMETHODCALLTYPE Hooks::Barrier_(ID3D12GraphicsCommandList7* self,
                                       UINT32 numGroups,
                                       const D3D12_BARRIER_GROUP* groups) {
    Hooks& h = Get();
    if (groups) {
        std::lock_guard<std::mutex> lock(h.mutex_);
        for (UINT32 g = 0; g < numGroups; ++g) {
            const D3D12_BARRIER_GROUP& grp = groups[g];
            if (grp.Type != D3D12_BARRIER_TYPE_TEXTURE || !grp.pTextureBarriers) continue;
            for (UINT32 i = 0; i < grp.NumBarriers; ++i) {
                const D3D12_TEXTURE_BARRIER& tb = grp.pTextureBarriers[i];
                if (!tb.pResource) continue;
                h.textureLayout_[tb.pResource] = tb.LayoutAfter;
                ++h.barriersSeen_[tb.pResource];
                ++h.enhancedBarrierCount_;
            }
        }
    }
    h.origEnhancedBarrier_(self, numGroups, groups);
}
// ---------------------------------------------------------------- hooks

void STDMETHODCALLTYPE Hooks::ExecuteCommandLists_(ID3D12CommandQueue* self, UINT count,
                                                   ID3D12CommandList* const* lists) {
    Hooks& h = Get();
    // The queue is not reachable from the swapchain, so this is where we learn
    // it. Only DIRECT queues are useful to us -- a copy or compute queue cannot
    // carry the graphics work we eventually want to submit alongside.
    if (!h.queue_) {
        const D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            h.queue_ = self;
            LogInfo("captured DIRECT command queue %p", static_cast<void*>(self));
            if (!h.device_) {
                if (SUCCEEDED(self->GetDevice(IID_PPV_ARGS(&h.device_)))) {
                    // GetDevice AddRefs; release immediately since the game
                    // outlives us and we only need the pointer for identity.
                    h.device_->Release();
                    LogInfo("resolved device %p from queue", static_cast<void*>(h.device_));
                    h.AttachInfoQueue();
                }
            }
        }
    }
    h.origExecute_(self, count, lists);
}
void STDMETHODCALLTYPE Hooks::CreateRenderTargetView_(ID3D12Device* self, ID3D12Resource* res,
                                                      const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Hooks& h = Get();
    if (res) {
        std::lock_guard<std::mutex> lock(h.mutex_);
        h.descriptorToResource_[dest.ptr] = res;
    }
    h.origCreateRTV_(self, res, desc, dest);
}
void STDMETHODCALLTYPE Hooks::CreateDepthStencilView_(ID3D12Device* self, ID3D12Resource* res,
                                                      const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Hooks& h = Get();
    if (res) {
        std::lock_guard<std::mutex> lock(h.mutex_);
        h.descriptorToResource_[dest.ptr] = res;

        // Log every DISTINCT depth-stencil resource the game ever views.
        //
        // This exists because a run once produced a census containing exactly
        // one depth target -- a 1x1 dummy -- while the game was plainly
        // rendering a 3D scene. From the census alone the two explanations are
        // indistinguishable: either the game never created a real depth buffer
        // (absurd), or OMSetRenderTargets was binding one we failed to resolve.
        // The census cannot tell them apart because it only ever sees targets
        // that were successfully resolved. This log sits upstream of that
        // filter, so silence here means the DSV really was never created, and
        // noise here means the loss happens later.
        static std::set<ID3D12Resource*> seen;
        if (seen.insert(res).second) {
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            LogInfo("CreateDSV #%zu: %p %s %llux%u samples=%u flags=0x%X viewFmt=%s",
                    seen.size(), static_cast<void*>(res), FormatName(d.Format),
                    d.Width, d.Height, d.SampleDesc.Count,
                    static_cast<unsigned>(d.Flags),
                    desc ? FormatName(desc->Format) : "<null desc>");
        }
    }
    h.origCreateDSV_(self, res, desc, dest);
}
void STDMETHODCALLTYPE Hooks::OMSetRenderTargets_(ID3D12GraphicsCommandList* self, UINT numRTs,
                                                  const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                                  BOOL single,
                                                  const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    Hooks& h = Get();

    if (dsv) {
        if (ID3D12Resource* res = h.ResolveDescriptor(dsv->ptr)) h.NoteBind(res, true);
    }
    if (rtvs && numRTs) {
        // With single==TRUE the handles are contiguous from rtvs[0]; otherwise
        // each entry is its own handle. Getting this backwards silently
        // mis-attributes every target after the first.
        const UINT stride = single ? 0 : 1;
        for (UINT i = 0; i < numRTs; ++i) {
            const D3D12_CPU_DESCRIPTOR_HANDLE hnd = rtvs[i * stride];
            if (ID3D12Resource* res = h.ResolveDescriptor(hnd.ptr)) h.NoteBind(res, false);
        }
    }

    h.origOMSetRT_(self, numRTs, rtvs, single, dsv);
}
void STDMETHODCALLTYPE Hooks::ResourceBarrier_(ID3D12GraphicsCommandList* self, UINT count,
                                               const D3D12_RESOURCE_BARRIER* barriers) {
    Hooks& h = Get();
    if (barriers) {
        std::lock_guard<std::mutex> lock(h.mutex_);
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_BARRIER& b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
            if (!b.Transition.pResource) continue;
            // Shadowing StateAfter is what later lets us transition a target to
            // COPY_SOURCE and back without guessing. A wrong guess here is a
            // debug-layer error at best and a GPU hang at worst.
            h.resourceState_[b.Transition.pResource] = b.Transition.StateAfter;
            ++h.barriersSeen_[b.Transition.pResource];
        }
    }
    h.origBarrier_(self, count, barriers);
}
// Resources carry an initial state that no ResourceBarrier ever announces. A
// depth buffer created in DEPTH_WRITE and never transitioned would otherwise
// read back as COMMON from our shadow -- observed on the test fixture. Using
// that wrong StateBefore in a transition to COPY_SOURCE is a debug-layer error
// at best and a GPU hang at worst, so the state map is seeded here.
HRESULT STDMETHODCALLTYPE Hooks::CreateCommittedResource_(
    ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heapProps,
    D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
    REFIID riid, void** resource) {
    Hooks& h = Get();
    const HRESULT hr = h.origCreateCommitted_(self, heapProps, heapFlags, desc,
                                              initialState, clearValue, riid, resource);
    if (SUCCEEDED(hr) && resource && *resource && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        h.NoteResourceCreated(static_cast<ID3D12Resource*>(*resource), *desc, initialState,
                              false, nullptr, 0);
    }
    return hr;
}
// UE5's RDG transient allocator takes this path, not the committed one, for
// render targets. Hooking it is what makes the whole state shadow apply on UE5
// at all -- before this, every render target inZOI used was created invisibly,
// so StateOf() returned COMMON for all of them and the recycling guard could
// never fire.
HRESULT STDMETHODCALLTYPE Hooks::CreatePlacedResource_(
    ID3D12Device* self, ID3D12Heap* heap, UINT64 heapOffset,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** resource) {
    Hooks& h = Get();
    const HRESULT hr = h.origCreatePlaced_(self, heap, heapOffset, desc, initialState,
                                           clearValue, riid, resource);
    if (SUCCEEDED(hr) && resource && *resource && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        h.NoteResourceCreated(static_cast<ID3D12Resource*>(*resource), *desc, initialState,
                              true, heap, heapOffset);
    }
    return hr;
}
void Hooks::NoteResourceCreated(ID3D12Resource* res, const D3D12_RESOURCE_DESC& desc,
                                D3D12_RESOURCE_STATES initialState, bool placed,
                                ID3D12Heap* heap, UINT64 heapOffset) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Hooks& h = *this;

        // ---- MEMORY ALIASING --------------------------------------------------
        //
        // A placed resource does not own its memory; it is a view onto a range of
        // a heap. Two placed resources at the same (heap, offset) are two names
        // for the SAME BYTES, and UE5's transient allocator does this on purpose
        // to keep the render-target pool small. So a second placed resource
        // covering a range we are tracking means our target's contents are now
        // someone else's to write.
        //
        // This is the mechanism behind the inZOI failure: the readback copied a
        // correctly-identified CustomDepth resource, with a correctly-computed
        // plane-1 footprint, and got back a 2-channel fp16 velocity buffer --
        // because by Present the heap range had been reissued to another pass.
        if (placed && heap) {
            const AliasRange incoming{heap, heapOffset, desc.Width, desc.Height, desc.Format};
            for (auto it = aliasRanges_.begin(); it != aliasRanges_.end();) {
                if (it->second.heap == heap && it->second.offset == heapOffset &&
                    it->first != res) {
                    ++aliasCollisions_;
                    if (it->first == electedTarget_) {
                        ++electedAliased_;
                        LogWarn("elected target %p SHARES HEAP MEMORY with a new placed "
                                "resource %p (heap %p offset %llu). Its contents are no "
                                "longer ours to read; election voided (alias #%llu)",
                                static_cast<void*>(it->first), static_cast<void*>(res),
                                static_cast<void*>(heap), heapOffset, electedAliased_);
                        electedTarget_ = nullptr;
                        electedProducedContent_ = false;
                    }
                    it = aliasRanges_.erase(it);
                } else {
                    ++it;
                }
            }
            aliasRanges_[res] = incoming;
        }

        // ---- ADDRESS RECYCLING ------------------------------------------------
        //
        // D3D12 hands the same ID3D12Resource* back for a completely different
        // resource once the previous occupant is released, and UE5's pooled
        // render-target allocator releases constantly -- 813 CreateDSV calls in
        // 25 minutes on inZOI. Every map here is keyed by that address, so
        // without this the dead resource's shadow state, barrier count and
        // accumulated election evidence all silently transfer to whatever is
        // created at the same address next.
        //
        // That is not hypothetical. inZOI elected 000001846D62F690 two hundred
        // times as a full-res depth-stencil; by the end of the session the same
        // address was reporting R10G10B10A2_UNORM with no stencil plane. We were
        // scoring a target that no longer existed and asking to copy from it --
        // which is the whole explanation for D3D12 error 527 (StateBefore
        // mismatch) and for the readback crash.
        //
        // This is the identical bug we already fixed on the UObject side with
        // generational handles. Creation at a known address IS the generation
        // bump; it is observable, so use it rather than inventing one.
        const size_t hadState = h.resourceState_.erase(res);
        const size_t hadBarriers = h.barriersSeen_.erase(res);
        const size_t hadLayout = h.textureLayout_.erase(res);
        const size_t hadEvidence = h.evidence_.erase(res);
        h.targets_.erase(res);
        if (hadState || hadBarriers || hadLayout || hadEvidence) {
            ++h.addressRecycles_;
            // If the recycled address is the one we were about to copy from,
            // the election is void. Say so loudly -- a silent re-election here
            // would look like ordinary target churn in the log.
            if (res == h.electedTarget_) {
                ++h.electedRecycles_;
                LogWarn("elected target %p was DESTROYED and its address reused by a new "
                        "resource; election voided (recycle #%llu)",
                        static_cast<void*>(res), h.electedRecycles_);
                h.electedTarget_ = nullptr;
                h.electedProducedContent_ = false;
            }
        }

        h.resourceState_[res] = initialState;
    }
}
void STDMETHODCALLTYPE Hooks::ClearDepthStencilView_(ID3D12GraphicsCommandList* self,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                                     D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                     UINT8 stencil, UINT numRects,
                                                     const D3D12_RECT* rects) {
    Hooks& h = Get();
    // The FLAGS matter and were previously discarded. D3D12_CLEAR_FLAG_DEPTH and
    // D3D12_CLEAR_FLAG_STENCIL are independent bits, and every depth target in a
    // frame gets its DEPTH cleared -- so "was cleared" carries almost no
    // information. "Had its STENCIL cleared" is a far sharper signal, because a
    // target nobody writes stencil into has no reason to clear stencil.
    //
    // This is not academic. On inZOI five 1280x800 stencil-capable targets reach
    // scoring, two are rejected as most-bound scene depth, and the remaining
    // three are separated ONLY by this +30. A counter that cannot see the
    // stencil flag was deciding a three-way tie.
    if (ID3D12Resource* res = h.ResolveDescriptor(dsv.ptr)) {
        h.NoteClear(res, (flags & D3D12_CLEAR_FLAG_STENCIL) != 0);
    }
    h.origClearDSV_(self, dsv, flags, depth, stencil, numRects, rects);
}
HRESULT STDMETHODCALLTYPE Hooks::Present_(IDXGISwapChain3* self, UINT sync, UINT flags) {
    Hooks& h = Get();
    // Backbuffer dimensions define "full resolution" for election. Read from
    // the swapchain rather than assumed, since resolution changes at runtime
    // and a stale value would silently disqualify every real candidate.
    if (h.backbufferWidth_ == 0) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(self->GetDesc(&desc))) {
            h.backbufferWidth_ = desc.BufferDesc.Width;
            h.backbufferHeight_ = desc.BufferDesc.Height;
            LogInfo("backbuffer %ux%u format=%s", h.backbufferWidth_, h.backbufferHeight_,
                    FormatName(desc.BufferDesc.Format));
        }
    }
    h.OnPresent(self);
    return h.origPresent_(self, sync, flags);
}
// ---------------------------------------------------------------- helpers

ID3D12Resource* Hooks::ResolveDescriptor(SIZE_T handlePtr) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = descriptorToResource_.find(handlePtr);
    if (it != descriptorToResource_.end()) return it->second;
    ++descriptorMisses_;
    return nullptr;
}
D3D12_RESOURCE_STATES Hooks::StateOf(ID3D12Resource* res) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resourceState_.find(res);
    return it == resourceState_.end() ? D3D12_RESOURCE_STATE_COMMON : it->second;
}
// How many ResourceBarrier transitions we have actually WATCHED for this
// resource. Zero is the number that matters.
//
// StateOf() answers COMMON for a resource it has never seen, which is a guess
// wearing the costume of an answer -- and the readback then submits a
// transition declaring that guess as StateBefore. On inZOI the debug layer
// caught it exactly: we declared NON_PIXEL|PIXEL_SHADER_RESOURCE for a resource
// the runtime knew was in UNORDERED_ACCESS.
//
// The likely mechanism is that UE5 transitions it somewhere we cannot see.
// Enhanced Barriers (ID3D12GraphicsCommandList7::Barrier) are a different entry
// point entirely and our ResourceBarrier hook is blind to them, which would
// make the shadow stale by construction rather than by accident. Rather than
// guess, count: a target that is bound every frame but for which we have
// watched zero barriers is a target whose state we are inventing.
uint64_t Hooks::BarriersSeenFor(ID3D12Resource* res) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = barriersSeen_.find(res);
    return it == barriersSeen_.end() ? 0 : it->second;
}
void Hooks::NoteBind(ID3D12Resource* res, bool asDepth) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& t = targets_[res];
    if (t.resource == nullptr) {
        t.resource = res;
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        t.format = d.Format;
        t.width = d.Width;
        t.height = d.Height;
        t.sampleCount = d.SampleDesc.Count;
        t.firstBindOrdinal = bindOrdinal_;
    }
    ++t.bindCount;
    t.everBoundAsDepth = t.everBoundAsDepth || asDepth;
    ++bindOrdinal_;
}
void Hooks::NoteClear(ID3D12Resource* res, bool clearedStencil) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& t = targets_[res];
    if (t.resource == nullptr) {
        t.resource = res;
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        t.format = d.Format;
        t.width = d.Width;
        t.height = d.Height;
        t.sampleCount = d.SampleDesc.Count;
    }
    ++t.clearCount;
    if (clearedStencil) ++t.stencilClearCount;
}

}  // namespace segcap
