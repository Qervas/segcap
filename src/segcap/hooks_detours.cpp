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
    // Claim any injected copies carried by these lists BEFORE forwarding.
    //
    // Taken first because a Reset on another thread could otherwise steal the
    // token out from under us; signalled AFTER, because a fence signalled before
    // ExecuteCommandLists can complete ahead of the copy and Drain would map a
    // buffer the GPU has not written yet.
    Readback::InjectToken claimed[8];
    UINT claimedCount = 0;
    // The atomic is checked BEFORE the lock, and it is not an optimisation.
    //
    // ExecuteCommandLists is called many times per frame from several threads,
    // and world streaming is the heaviest submission period in the process.
    // Taking a global mutex there unconditionally serialised every submitting
    // thread on us -- including in DRY-RUN mode, where no token can ever exist,
    // so the lock was pure cost protecting an always-empty map. inZOI died
    // during world load in that mode, with no GPU work recorded at all.
    //
    // When nothing is outstanding -- always in dry run, and most frames
    // otherwise -- the cost is now one relaxed load.
    if (h.foreignInject_ && lists && count &&
        h.outstandingTokens_.load(std::memory_order_relaxed) != 0) {
        std::lock_guard<std::mutex> lock(h.listMutex_);
        for (UINT i = 0; i < count && claimedCount < 8; ++i) {
            auto it = h.listState_.find(reinterpret_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (it == h.listState_.end() || !it->second.pendingToken.valid) continue;
            claimed[claimedCount++] = it->second.pendingToken;
            h.outstandingTokens_.fetch_sub(1, std::memory_order_relaxed);
            // Cleared so a list submitted twice does not signal the same slot
            // twice -- the second signal would retire a slot whose buffer is
            // about to be overwritten by the re-execution.
            it->second.pendingToken = Readback::InjectToken{};
        }
    }

    h.origExecute_(self, count, lists);

    for (UINT i = 0; i < claimedCount; ++i) {
        if (h.maskInjectRing_.NotifySubmitted(self, claimed[i])) ++h.injSubmitted_;
    }
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

    // Read the injection gate FIRST and without a lock. This is the hottest hook
    // in the process -- UE calls it from every parallel command-list recording
    // thread -- and when nothing is armed (Stray always, inZOI until the id
    // probe converges) the entire added cost is this load and a branch.
    ID3D12Resource* const want = h.injectTarget_.load(std::memory_order_relaxed);

    bool inject = false;
    D3D12_RESOURCE_STATES injectState = D3D12_RESOURCE_STATE_COMMON;
    UINT injectSub = 0;

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

            if (!want || b.Transition.pResource != want || inject) continue;
            ++h.injAttempts_;

            // A BEGIN_ONLY split barrier leaves the subresource neither readable
            // nor writable until its matching END_ONLY, and the only legal
            // intervening barrier is that END_ONLY. Injecting into that window
            // is a device removal.
            if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) {
                ++h.injRefusedFlags_;
                continue;
            }
            // Mirror the subresource; anything else would invent a StateBefore
            // for subresources this barrier did not name.
            if (b.Transition.Subresource != 0 &&
                b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                ++h.injRefusedSubresource_;
                continue;
            }
            // Only WRITE -> READ. Half of a render target's transitions go INTO a
            // write state, and copying there captures the PREVIOUS frame or an
            // undefined transient. The transition out of the export pass is the
            // one moment the buffer is both finished and readable.
            constexpr D3D12_RESOURCE_STATES kWrite =
                D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
                D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_COPY_DEST;
            if ((b.Transition.StateBefore & kWrite) == 0 ||
                (b.Transition.StateAfter & kWrite) != 0) {
                ++h.injRefusedNotWriteToRead_;
                continue;
            }
            inject = true;
            injectState = b.Transition.StateAfter;
            injectSub = b.Transition.Subresource;
        }
    }

    // Forward the game's barrier BEFORE recording ours, so the state we declare
    // is genuinely the state now established in this list. mutex_ is released by
    // here on purpose: it must never be held across a D3D12 call, or every
    // recording thread in the game serialises on us.
    h.origBarrier_(self, count, barriers);

    if (inject) h.TryInjectCopy(self, want, injectState, injectSub);
}

// Render-pass nesting is tracked THREAD-LOCAL, not in a shared map.
//
// A D3D12 command list is a single-threaded object: its BeginRenderPass,
// EndRenderPass, ResourceBarrier and CopyTextureRegion all happen on the one
// thread currently recording it. So "am I inside a render pass on this list" and
// "am I inside a render pass on this thread" are the same question, and the
// thread-local answer needs no lock and no hash lookup.
//
// The first version used a global mutex plus an unordered_map lookup on every
// BeginRenderPass and EndRenderPass. UE records with many parallel translate
// threads and issues many passes per frame, so that serialised the engine's
// hottest path through one lock -- and inZOI died in DRY-RUN mode, which records
// no GPU work at all, i.e. the instrumentation was the problem rather than
// anything it was instrumenting.
thread_local uint32_t g_renderPassDepth = 0;

void STDMETHODCALLTYPE Hooks::BeginRenderPass_(ID3D12GraphicsCommandList4* self,
                                               UINT numRenderTargets, const void* renderTargets,
                                               const void* depthStencil, UINT flags) {
    ++g_renderPassDepth;
    Get().origBeginRenderPass_(self, numRenderTargets, renderTargets, depthStencil, flags);
}

void STDMETHODCALLTYPE Hooks::EndRenderPass_(ID3D12GraphicsCommandList4* self) {
    if (g_renderPassDepth) --g_renderPassDepth;
    Get().origEndRenderPass_(self);
}

void Hooks::TryInjectCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                          D3D12_RESOURCE_STATES stateAfter, UINT subresource) {
    // Inside a render pass, ResourceBarrier is legal but CopyTextureRegion makes
    // the runtime remove the command list -- which surfaces as the GAME's own
    // Close() failing and UE turning that into a fatal check. So the barrier
    // reaching us is not evidence the copy is safe here.
    if (g_renderPassDepth > 0) {
        ++injRefusedRenderPass_;
        return;
    }

    // DIRECT lists only.
    //
    // We mirror the game's StateBefore straight out of its barrier, and on this
    // title that is routinely 0xC0 = NON_PIXEL_SHADER_RESOURCE|PIXEL_SHADER_
    // RESOURCE. PIXEL_SHADER_RESOURCE is NOT a legal state on a COMPUTE command
    // list -- D3D12 restricts compute lists to a subset that excludes it -- so
    // recording that transition there is an invalid argument. Like every other
    // invalid recorded command it returns void, gets latched into the list, and
    // detonates in the GAME's Close() as E_INVALIDARG (docs/DEBUGGING.md 8.7).
    //
    // Suspected because the crash tracks WHICH resource we target: runs that
    // probed the R32_UINT candidate survived 83 and 128 copies, while every run
    // that reached the R16G16_UINT Nanite CombinedCustomStencil died within
    // milliseconds of `inject: ON`. Nanite's export runs on the compute path, so
    // its barriers arrive on a different list type than the ones we had been
    // injecting into successfully.
    const D3D12_COMMAND_LIST_TYPE listType = list->GetType();
    if (listType != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ++injRefusedListType_;
        if (injRefusedListType_ == 1) {
            LogWarn("inject: REFUSED -- command list %p is type %d, not DIRECT. The game's "
                    "StateBefore=0x%X cannot legally be declared on this list type.",
                    static_cast<void*>(list), static_cast<int>(listType),
                    static_cast<unsigned>(stateAfter));
        }
        return;
    }

    if (injectDryRun_) {
        if (injRecorded_ < 4) {
            LogWarn("inject DRY RUN: would copy %p on list %p at StateAfter=0x%X sub=%u "
                    "(no GPU work recorded)",
                    static_cast<void*>(target), static_cast<void*>(list),
                    static_cast<unsigned>(stateAfter), subresource);
        }
        ++injRecorded_;
        return;
    }

    const Readback::InjectToken token = maskInjectRing_.RecordInto(
        list, target, stateAfter, subresource,
        injectFrame_.load(std::memory_order_relaxed), origBarrier_);
    if (!token.valid) {
        ++injRefusedNoSlot_;
        // RecordInto can fail after it has already issued barriers, so there may
        // be validation to read even on the path that records no copy.
        DrainInfoQueue();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(listMutex_);
        listState_[list].pendingToken = token;
    }
    outstandingTokens_.fetch_add(1, std::memory_order_relaxed);
    ++injRecorded_;
    if (injRecorded_ == 1) {
        LogWarn("inject: FIRST copy recorded into the game's own command list %p "
                "(StateBefore=0x%X taken from the game's barrier, not from our shadow)",
                static_cast<void*>(list), static_cast<unsigned>(stateAfter));
    }
    // Read the debug layer HERE, on the recording thread, not on the next Present.
    //
    // D3D12's recording methods return void; an invalid CopyTextureRegion is
    // latched into the command list and only surfaces when the GAME calls Close(),
    // as E_INVALIDARG, which UE turns into a fatal check. That is exactly how the
    // 11:46 run died -- and because the drain lived only on the Present path, the
    // messages naming the invalid argument were still sitting in the info queue
    // when the process went down. There is no "next Present" after the call that
    // kills you. Costs nothing when the debug layer is off: infoQueue_ is null.
    DrainInfoQueue();
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
        // BEFORE the bookkeeping: creation at this address proves the previous
        // occupant was destroyed, so any reference we still hold to it is void.
        // Forget those references without releasing them -- releasing would
        // decrement the count of the resource being created right now. This runs
        // unconditionally, not under the hadState/hadEvidence guard below: we can
        // hold a pin on a resource whose maps have already been pruned.
        h.AbandonOwnedRefs(res, "a new resource was created at this address");

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
