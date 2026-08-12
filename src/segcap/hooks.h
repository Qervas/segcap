// hooks.h -- D3D12 interception and the state we must shadow to use it.
//
// D3D12 moved bookkeeping the D3D11 runtime used to do into the application.
// For an injected DLL, "the application" is the game, so that bookkeeping
// becomes ours to reconstruct. Three things have to be recovered:
//
//   1. The command queue. It is not reachable from the swapchain by any
//      documented route, so it is sniffed from ExecuteCommandLists.
//   2. Descriptor handle -> resource. OMSetRenderTargets receives opaque
//      D3D12_CPU_DESCRIPTOR_HANDLE values, which are heap offsets carrying no
//      identity. CreateRenderTargetView / CreateDepthStencilView see both the
//      resource and the destination handle, so recording them there is what
//      makes the handles meaningful later.
//   3. Resource state. Transitioning a resource to COPY_SOURCE requires
//      knowing its current state; guessing produces debug-layer errors or GPU
//      hangs. ResourceBarrier is shadowed to track it.
//
// Everything here is READ-ONLY with respect to the game's rendering. No hook
// alters arguments, and no hook issues GPU work. Capture comes later and on our
// own command list.

#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "identity.h"
#include "readback.h"

namespace segcap {

// A render target or depth target observed being bound, described only by
// properties an injected DLL can actually see. Debug names are deliberately
// absent: they are stripped in shipping builds, so electing a buffer by name
// would be a technique that works only in development.
struct TargetFingerprint {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t width = 0;
    uint32_t height = 0;
    uint32_t sampleCount = 0;
    uint32_t bindCount = 0;        // times bound this frame
    uint32_t clearCount = 0;       // times cleared this frame (depth OR stencil)
    // Times the STENCIL plane specifically was cleared this frame. Tracked apart
    // from clearCount because every depth target has its depth cleared, so
    // clearCount barely discriminates; clearing stencil implies someone intends
    // to write stencil, which is exactly the CustomDepth signature.
    uint32_t stencilClearCount = 0;
    uint32_t firstBindOrdinal = 0; // call ordinal within the frame
    D3D12_RESOURCE_STATES lastState = D3D12_RESOURCE_STATE_COMMON;
    bool everBoundAsDepth = false;
    // Frames this resource has been observed in, accumulated across the whole
    // session rather than reset per frame. Against Stray this is the signal
    // that separates the real CustomDepth target (seen in 1096 of 1099 census
    // blocks, spanning the entire session) from transient loading-screen depth
    // buffers that existed for 25 seconds and scored identically on every
    // per-frame signal.
    uint32_t framesSeen = 0;
    // Frame index this target was last observed in. Election is made from
    // accumulated evidence, not from one frame's observations: a target that
    // simply is not bound or cleared during a given frame must not vanish from
    // consideration, or candidates take turns disappearing and the election
    // flaps between them.
    uint64_t lastSeenFrame = 0;
};

// Result of scoring one candidate. The reason string exists so that a wrong
// election is diagnosable from the log alone -- "it picked the wrong buffer" is
// useless without knowing which signal misled it.
struct ElectionScore {
    ID3D12Resource* resource = nullptr;
    int score = 0;
    char reason[256] = {};
};

class Hooks {
public:
    static Hooks& Get();

    // Installs the vtable hooks. Safe to call once; returns false on failure
    // with the reason already logged.
    bool Install();
    void Uninstall();

    // Must be set before Install(). Driven by a marker file rather than an
    // environment variable: an injected DLL inherits the host's environment,
    // which was fixed when Steam created the process, so we cannot influence it.
    void SetCensusOnly(bool on) { censusOnly_ = on; }
    bool censusOnly() const { return censusOnly_; }

    // Suppress the readback copy only. Unlike census mode this leaves marking,
    // reflection and ProcessEvent running, so it isolates GPU work from engine
    // work as a single variable.
    void SetNoReadback(bool on) { noReadback_ = on; }
    bool noReadback() const { return noReadback_; }

    // Must be set before Install(), i.e. before the game creates its device.
    void SetD3DDebug(bool on) { d3dDebug_ = on; }
    // Pre-load the format the id buffer is known to use, so discovery does not
    // start from zero every session. Verification is unchanged: the leased-slot
    // test still has to pass before anything is accepted.
    void SeedIdFormat(DXGI_FORMAT f) { provenFormat_ = f; }

    void SetRequireArm(bool on) { requireArm_ = on; }

    // The UObject count, published by the marking loop. Hooks cannot reach the
    // engine, and this is the single most informative number for telling menu
    // from loading from world -- so the one place that knows it hands it over.
    void SetObjectCount(int32_t n) { objectCount_.store(n, std::memory_order_relaxed); }

    // The engine's own answers: which level is loaded, and UE's actual pause
    // flag (1 paused, 0 running, -1 unknown). Direct facts, not proxies.
    // Answers obtained by CALLING the engine, which outrank anything inferred.
    void SetPausedDirect(int p) {
        std::lock_guard<std::mutex> lock(worldStateMutex_);
        pausedDirect_ = p;
    }
    void SetDilationDirect(float d) {
        std::lock_guard<std::mutex> lock(worldStateMutex_);
        dilationDirect_ = d;
    }

    void SetWorldState(const char* level, int paused, float dilation) {
        std::lock_guard<std::mutex> lock(worldStateMutex_);
        if (level && *level) worldLevel_ = level;
        worldPaused_ = paused;
        worldDilation_ = dilation;
    }

    // Isolation switch for the backbuffer copy; see the colour block in
    // OnPresent. Set from a "segcap.nocolour" marker.
    void SetNoColour(bool on) { noColour_ = on; }

    // A/B mode: capture colour frames on a fixed stride regardless of whether a
    // mask was kept, so the "marking off" condition produces frames at all.
    void SetAbTest(bool on) { abTest_ = on; }

    // ID-buffer probe: read back a scene-resolution INTEGER render target and
    // report what is in it.
    //
    // This is the second route to per-object identity, and the reason it
    // matters is that it needs no engine mutation at all. On UE5 the
    // CustomDepth route is blocked behind a ProcessEvent search that has no
    // safe answer yet, but the readback path already works there -- so if the
    // engine is already writing per-pixel ids into a buffer for its own
    // purposes, reading it sidesteps the blocker entirely.
    //
    // The census found exactly one candidate shape on inZOI: R32_UINT at the
    // scene's 1280x800, bound once per frame. Whether it holds object ids,
    // material ids, or something else is not inferable from its format and
    // size, which is what this probe exists to settle.
    void SetProbeIdBuffer(bool on) { probeIdBuffer_ = on; }
    bool probeIdBuffer() const { return probeIdBuffer_; }

    // Ground truth by intervention. Unmarks one slot mid-run and checks that
    // exactly its pixels disappear -- the only check here that a mask which is
    // wrong in a spatially and temporally coherent way cannot pass. Mutates the
    // game, so it is opt-in.
    void SetIntervene(bool on);

    // ---- foreign-list copy injection ---------------------------------------
    //
    // Record our copy into the GAME's command list, at the moment the game
    // itself transitions the buffer out of a write state. There StateAfter is
    // declared in the barrier struct we are standing in, so our StateBefore is
    // that value by construction -- which is the only way to be right about the
    // state of a UE5 transient resource from outside the engine.
    //
    // OFF by default and gated twice: this marker, and the resource having been
    // PROVEN to carry our leased slots. Stray reaches neither, and without the
    // marker the extra vtable hooks are not installed at all, so its process
    // gains no new code on any hot path.
    void SetForeignInject(bool on) { foreignInject_ = on; }
    bool foreignInject() const { return foreignInject_; }
    // Observe render passes, barrier flags and list types, and log what WOULD
    // have been injected -- without recording a single GPU command. Answers
    // "does this title use BeginRenderPass" before that question can remove the
    // device.
    void SetInjectDryRun(bool on) { injectDryRun_ = on; }

    // Capture profile, read from marker files at startup.
    //
    // These are a genuine trade-off rather than a tuning knob, which is why
    // they are configurable instead of compiled in. A SMALL stride captures
    // consecutive gameplay -- smooth motion, good for a demo -- but spends the
    // budget over a few seconds, which is a window too short to observe objects
    // leaving and re-entering the working set. A LARGE stride covers minutes
    // and shows identity recycling, but plays back as a slideshow. One run
    // cannot do both well, and pretending otherwise produced two separate
    // wrong-looking results earlier (see DEBUGGING.md 7.7 and 7.8c).
    void SetCaptureProfile(uint32_t maxCaptures, uint64_t stride) {
        if (maxCaptures) maxCaptures_ = maxCaptures;
        if (stride) captureStride_ = stride;
    }
    uint32_t maxCaptures() const { return maxCaptures_; }
    uint64_t captureStride() const { return captureStride_; }

    // ---- state recovered from the game ----------------------------------

    ID3D12Device* device() const { return device_; }
    ID3D12CommandQueue* queue() const { return queue_; }
    uint64_t frameIndex() const { return frameIndex_; }

    // Resolve a descriptor handle back to the resource it views. Returns
    // nullptr if we never saw the view being created -- which happens for
    // views created before injection, and is worth logging rather than
    // silently treating as "no target".
    ID3D12Resource* ResolveDescriptor(SIZE_T handlePtr);

    D3D12_RESOURCE_STATES StateOf(ID3D12Resource* res);

    // Snapshot of this frame's observed targets, for election scoring.
    std::vector<TargetFingerprint> SnapshotTargets();

    // Rank candidates for "the CustomDepth target", most likely first.
    // Uses only signals an injected DLL can observe at runtime -- never names.
    std::vector<ElectionScore> ScoreTargets(const std::vector<TargetFingerprint>& targets) const;

    uint32_t backbufferWidth() const { return backbufferWidth_; }
    uint32_t backbufferHeight() const { return backbufferHeight_; }

private:
    Hooks() = default;

    bool AcquireVTables();
    bool AcquireSwapChainVTable();

    // ---- hook entry points ----------------------------------------------
    static HRESULT STDMETHODCALLTYPE Present_(IDXGISwapChain3* self, UINT sync, UINT flags);
    static void STDMETHODCALLTYPE ExecuteCommandLists_(ID3D12CommandQueue* self,
                                                       UINT count,
                                                       ID3D12CommandList* const* lists);
    static void STDMETHODCALLTYPE CreateRenderTargetView_(ID3D12Device* self,
                                                          ID3D12Resource* res,
                                                          const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE dest);
    static void STDMETHODCALLTYPE CreateDepthStencilView_(ID3D12Device* self,
                                                          ID3D12Resource* res,
                                                          const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE dest);
    static void STDMETHODCALLTYPE OMSetRenderTargets_(ID3D12GraphicsCommandList* self,
                                                      UINT numRTs,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                                      BOOL single,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
    static void STDMETHODCALLTYPE ResourceBarrier_(ID3D12GraphicsCommandList* self,
                                                   UINT count,
                                                   const D3D12_RESOURCE_BARRIER* barriers);
    // Enhanced Barriers. A game written against this never calls the legacy
    // ResourceBarrier above, which is why the state shadow could see nothing.
    static void STDMETHODCALLTYPE Barrier_(ID3D12GraphicsCommandList7* self,
                                           UINT32 numGroups,
                                           const D3D12_BARRIER_GROUP* groups);
    // Render-pass scope. CopyTextureRegion recorded between BeginRenderPass and
    // EndRenderPass causes the runtime to REMOVE THE COMMAND LIST, while
    // ResourceBarrier is explicitly legal there -- so the injection point can be
    // a place where the barrier is fine and the copy is fatal. Tracked per list
    // so injection can refuse inside a pass.
    static void STDMETHODCALLTYPE BeginRenderPass_(ID3D12GraphicsCommandList4* self,
                                                   UINT numRenderTargets,
                                                   const void* renderTargets,
                                                   const void* depthStencil, UINT flags);
    static void STDMETHODCALLTYPE EndRenderPass_(ID3D12GraphicsCommandList4* self);
    static HRESULT STDMETHODCALLTYPE CreateCommittedResource_(
        ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heapProps,
        D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc,
        D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
        REFIID riid, void** resource);
    // PLACED resources are the UE4-vs-UE5 asymmetry. UE4.27 allocates render
    // targets as committed resources, so hooking the committed path alone was
    // sufficient for Stray. UE5's RDG transient allocator carves render targets
    // out of pooled heaps as PLACED resources that ALIAS one another within a
    // frame -- so on inZOI the entire render-target population was invisible to
    // the state shadow, to the address-recycling guard (which lives in
    // CreateCommittedResource_ and so never fired), and to the AddRef pin (which
    // keeps the ID3D12Resource object alive but cannot stop the heap memory
    // underneath being handed to another pass).
    static HRESULT STDMETHODCALLTYPE CreatePlacedResource_(
        ID3D12Device* self, ID3D12Heap* heap, UINT64 heapOffset,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** resource);
    // Bookkeeping shared by both creation paths: a new resource at a known
    // address invalidates everything we believed about that address.
    void NoteResourceCreated(ID3D12Resource* res, const D3D12_RESOURCE_DESC& desc,
                             D3D12_RESOURCE_STATES initialState, bool placed,
                             ID3D12Heap* heap, UINT64 heapOffset);
    static void STDMETHODCALLTYPE ClearDepthStencilView_(ID3D12GraphicsCommandList* self,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                                         D3D12_CLEAR_FLAGS flags,
                                                         FLOAT depth, UINT8 stencil,
                                                         UINT numRects,
                                                         const D3D12_RECT* rects);

    void OnPresent(IDXGISwapChain3* swapChain);
    void OnColourReady(const MaskFrame& frame);
    void WriteSidecar(const MaskFrame& frame) const;
    void NoteBind(ID3D12Resource* res, bool asDepth);
    void NoteClear(ID3D12Resource* res, bool clearedStencil);
    void OnMaskReady(const MaskFrame& frame);
    void OnIdBufferReady(const MaskFrame& frame);

    // ---- originals -------------------------------------------------------
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
    using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using CreateRTVFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                 const D3D12_RENDER_TARGET_VIEW_DESC*,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE);
    using CreateDSVFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                 const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE);
    using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                               const D3D12_RESOURCE_BARRIER*);
    using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS,
                                                FLOAT, UINT8, UINT, const D3D12_RECT*);
    using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
        REFIID, void**);

    PresentFn origPresent_ = nullptr;
    ExecuteFn origExecute_ = nullptr;
    CreateRTVFn origCreateRTV_ = nullptr;
    CreateDSVFn origCreateDSV_ = nullptr;
    OMSetRTFn origOMSetRT_ = nullptr;
    BarrierFn origBarrier_ = nullptr;
    ClearDSVFn origClearDSV_ = nullptr;
    CreateCommittedFn origCreateCommitted_ = nullptr;

    // ---- shadowed state --------------------------------------------------
    // One mutex covering all maps. These are touched on the render thread on a
    // hot path, so the critical sections are kept to a single map operation.
    std::mutex mutex_;
    std::unordered_map<SIZE_T, ID3D12Resource*> descriptorToResource_;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resourceState_;
    // Observations for the frame currently being built. Cleared every Present.
    std::unordered_map<ID3D12Resource*, TargetFingerprint> targets_;
    // Accumulated evidence across the session. This is what the election reads.
    // Entries not seen for a long time are pruned so destroyed resources cannot
    // win forever on stale persistence.
    std::unordered_map<ID3D12Resource*, TargetFingerprint> evidence_;

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    uint64_t frameIndex_ = 0;
    uint32_t bindOrdinal_ = 0;
    uint32_t backbufferWidth_ = 0;
    uint32_t backbufferHeight_ = 0;
    bool installed_ = false;

    // Counts of descriptor resolutions that missed, i.e. views created before
    // we were injected. A high number means our picture of the frame is
    // incomplete and any election decision is suspect.
    uint64_t descriptorMisses_ = 0;

    Readback readback_;
    // A second ring for the colour backbuffer. Captured in the SAME Present call
    // as the mask and stamped with the same frame index, so the two streams
    // share a frame grid by construction -- no timestamp matching, no
    // resampling, and no way for them to drift apart.
    Readback colourRing_;
    uint32_t colourDumped_ = 0;
    // Frame indices whose mask was kept, so the colour ring keeps exactly
    // the matching frames. Unpaired captures are dead weight: make_demo
    // can only use indices that have both.
    std::unordered_set<uint64_t> maskKept_;
    ID3D12Resource* electedTarget_ = nullptr;

    // Which slot table was live when each in-flight frame's copy was submitted.
    //
    // Readback latency is a few frames and the marking thread mutates the table
    // continuously, so reading the table when a mask ARRIVES describes a
    // different moment than the one the pixels came from. Bounded to a handful
    // of entries -- anything older than the ring depth can never be claimed.
    mutable std::deque<std::pair<uint64_t, std::shared_ptr<const FrameSidecar>>>
        sidecarHistory_;

    // frameIndex -> wall-clock milliseconds at Present. Written into the
    // sidecar so a captured frame can be joined against the input log that
    // vpad.exe writes from a different process. World models train on
    // (frame, action) pairs, so a frame with no action attached is half a
    // training example.
    mutable std::deque<std::pair<uint64_t, long long>> frameStamps_;

    // Has the currently elected target ever yielded a non-empty mask? Gates the
    // incumbency bonus, so a target that produces nothing can never entrench
    // itself against a better candidate discovered a few frames later. Reset
    // whenever the election changes.
    bool electedProducedContent_ = false;
    uint32_t masksDumped_ = 0;
    // Counted separately so a few baseline (all-zero) captures for the task-11
    // A/B comparison cannot consume the budget reserved for frames that
    // actually contain mask content.
    uint32_t emptyMasksDumped_ = 0;

    // ---- recording gate --------------------------------------------------
    //
    // Menus, loading screens and cutscene fades are worthless as segmentation
    // training data -- they would pad the dataset with frames containing no
    // labelled objects. Rather than trying to detect "is this a menu" from
    // engine state, the gate keys on the thing that actually matters: whether
    // this frame has mask content at all.
    //
    // Hysteresis in both directions. A single empty frame does not stop a
    // recording (a camera can briefly face an unmarked wall), and a single
    // content frame does not start one (a stray marked object visible behind a
    // menu should not open a session).
    bool recording_ = false;
    bool warnedForeignStencil_ = false;   // "that stencil is not ours", said once
    // "those VALUES are not ours" -- the stronger claim, checked against the
    // slots the sidecar actually leased. Said once so a permanently-wrong target
    // does not write the message every frame.
    bool warnedUnleasedIds_ = false;
    uint32_t consecutiveContent_ = 0;
    uint32_t consecutiveEmpty_ = 0;
    uint64_t recordedFrames_ = 0;
    uint64_t skippedFrames_ = 0;
    uint64_t recordingStartedFrame_ = 0;
    // Set from SEGCAP_CENSUS_ONLY=1. Suppresses all GPU work so an unfamiliar
    // title can be observed before anything is submitted on its queue.
    bool censusOnly_ = false;
    bool noReadback_ = false;
    bool warnedNoReadback_ = false;

    using EnhancedBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32,
                                                       const D3D12_BARRIER_GROUP*);
    EnhancedBarrierFn origEnhancedBarrier_ = nullptr;
    bool enhancedBarriersHooked_ = false;
    uint64_t enhancedBarrierCount_ = 0;
    std::unordered_map<ID3D12Resource*, D3D12_BARRIER_LAYOUT> textureLayout_;

    std::unordered_map<ID3D12Resource*, uint64_t> barriersSeen_;
    uint64_t BarriersSeenFor(ID3D12Resource* res);

    // ---- resource identity -------------------------------------------------
    //
    // Every map above is keyed by ID3D12Resource*, and D3D12 reuses those
    // addresses as soon as the previous resource is released. A reference held
    // on the elected target makes its address un-recyclable for as long as we
    // intend to copy from it; the counters record how often the game recycled
    // an address we were tracking, which is the signal that told us the
    // election had been scoring a destroyed resource.
    // Drop every reference we own to a resource that has been DESTROYED, without
    // calling Release on any of them. Once D3D12 has reissued an address, the
    // object our AddRef referred to is gone and our count went with it; the
    // pointer now names a live, game-owned resource. Releasing it then steals a
    // reference the game still needs -- see the comment above pinnedTarget_.
    void AbandonOwnedRefs(ID3D12Resource* dead, const char* why);

    // OWNED, but only conditionally: see AbandonOwnedRefs. An AddRef on this
    // pointer is worth nothing once the address has been recycled, so the ONLY
    // safe response to a recycle is to forget it, never to release it.
    ID3D12Resource* pinnedTarget_ = nullptr;
    D3D12_RESOURCE_DESC electedDesc_ = {};
    // Times the top-scoring candidate was refused because it was last seen in an
    // earlier frame. A high count means evidence_ is outliving its resources.
    uint64_t pinRefusedStale_ = 0;
    // Times a recycled address forced us to abandon an owned reference.
    uint64_t refsAbandoned_ = 0;
    uint64_t addressRecycles_ = 0;
    uint64_t electedRecycles_ = 0;

    // Where a placed resource lives. Two placed resources at the same
    // (heap, offset) are two names for the same bytes -- which is how UE5's
    // transient allocator reuses render-target memory within a frame, and how a
    // correctly-identified CustomDepth target can yield another pass's contents.
    struct AliasRange {
        ID3D12Heap* heap = nullptr;
        UINT64 offset = 0;
        UINT64 width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    std::unordered_map<ID3D12Resource*, AliasRange> aliasRanges_;
    uint64_t aliasCollisions_ = 0;
    uint64_t electedAliased_ = 0;

    using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                       const D3D12_RESOURCE_DESC*,
                                                       D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, REFIID,
                                                       void**);
    CreatePlacedFn origCreatePlaced_ = nullptr;
    bool warnedUnshadowed_ = false;
    ID3D12Resource* lastUnshadowedTarget_ = nullptr;
    bool loggedArmedOk_ = false;

    // Hold the readback until a segcap.arm file appears next to the DLL.
    bool requireArm_ = false;
    std::atomic<int32_t> objectCount_{0};
    std::mutex worldStateMutex_;
    std::string worldLevel_;
    int worldPaused_ = -1;
    float worldDilation_ = -1.0f;
    int pausedDirect_ = -1;
    float dilationDirect_ = -1.0f;
    int32_t lastStateObjects_ = 0;
    bool noColour_ = false;
    bool armed_ = false;

    bool d3dDebug_ = false;
    ID3D12InfoQueue* infoQueue_ = nullptr;
    uint64_t d3dMessagesLogged_ = 0;
    // The drain used to happen only on the Present thread. It is now also called
    // from whichever of UE's parallel translate threads recorded our copy, so it
    // needs a lock: two threads interleaving GetMessage/ClearStoredMessages would
    // drop exactly the messages we are draining early in order to read.
    std::mutex infoQueueMutex_;
    void AttachInfoQueue();
    void DrainInfoQueue();
    bool abTest_ = false;
    bool probeIdBuffer_ = false;
    Readback idRing_;
    ID3D12Resource* idTarget_ = nullptr;
    uint32_t idDumped_ = 0;
    // The probe walks every scene-scale integer target rather than picking one
    // by area and never reconsidering. On a Nanite title the per-object stencil
    // lives in a COLOUR target (PF_R16G16_UINT CombinedCustomStencil), so the
    // "largest area" rule reliably selected the wrong buffer and the right one
    // was never read once in 1,397 sightings.
    std::vector<ID3D12Resource*> idCandidates_;
    size_t idCandidateIndex_ = 0;
    // Logged once: the probe is waiting for enough of the world to be marked
    // before its acceptance test can distinguish an id buffer from one big region.
    bool warnedProbeTooEarly_ = false;
    // The format proven to carry our ids (R16G16_UINT on a Nanite title). Once
    // known, candidate selection ignores every other format, so a buffer lost to
    // a world transition is re-acquired immediately instead of rediscovered.
    // NOT cleared by re-probe on purpose: a re-probe wants a fresh RESOURCE, not
    // amnesia about which format works. `segcap.reprobe` clears it explicitly.
    DXGI_FORMAT provenFormat_ = DXGI_FORMAT_UNKNOWN;
    // Edge-trigger for the "inject ON/OFF" line, so the transition is logged
    // rather than the state being restated every frame.
    bool injectWasOn_ = false;
    // Watchdog state for a probe candidate that never gets barriered, which is
    // what a destroyed-but-still-pointed-at resource looks like from here.
    // Last frame the probe cleared its rejections and re-swept, so "every target
    // present has been tested" expires instead of ending the session.
    uint64_t idSweepFrame_ = 0;
    uint64_t idTargetSetFrame_ = 0;
    uint64_t injAttemptsAtSet_ = 0;
    // Candidates already tested and found not to contain our slots. Keyed by
    // resource rather than by index because the candidate list is rebuilt every
    // time -- new integer targets appear once the CustomDepth pass first runs.
    std::unordered_set<ID3D12Resource*> idRejected_;

    // When the probe proves which buffer and channel carry our slots, the mask
    // pipeline is repointed at it. Nanite titles need this: the ids live in a
    // colour target, not in any depth-stencil's stencil plane.
    ID3D12Resource* maskSource_ = nullptr;
    int maskChannel_ = -1;          // index into the probe's channel decomposition
    uint32_t maskSourceBpp_ = 1;
    std::vector<uint8_t> maskScratch_;   // channel extracted to 8-bit, reused
    bool warnedMaskSourceUnshadowed_ = false;
    // Frames spent waiting for a candidate to transition at least once. A
    // resource we have never seen transition has an unknown state, and copying
    // from it is a GPU fault rather than a bad mask.
    uint32_t idBarrierWait_ = 0;
    static constexpr uint32_t kIdBarrierWaitFrames = 600;
    // Consecutive frames the pinned id buffer has not been bound by the game.
    // A few frames of absence are normal (a pass can skip a frame); a sustained
    // absence means the transient allocator has moved on and we are copying a
    // resource nobody writes to any more.
    uint32_t maskSourceMissing_ = 0;
    static constexpr uint32_t kMaskSourceMissingLimit = 120;

    // ---- foreign-list copy injection ---------------------------------------
    bool foreignInject_ = false;
    bool injectDryRun_ = false;
    Readback maskInjectRing_;

    // --- colour by INJECTION, not at Present ---------------------------------
    //
    // The backbuffer copy issued from our own queue inside Present crashed inZOI
    // at CAPTURE ARMED, twice, against a clean control with it disabled. Outside
    // an engine you cannot reliably know a transient resource's state; the mask
    // copy has always taken StateBefore from the game's own barrier struct, and
    // that path has never caused this. The backbuffer's transition to PRESENT is
    // itself a write->read transition, so it fits the existing inject gate with
    // no new rules -- the colour path just had to stop being the exception.
    Readback colourInjectRing_;
    // Every buffer the swapchain owns. Collected once, then read lock-free on the
    // barrier hot path: written before injection is armed and never mutated after.
    ID3D12Resource* backBuffers_[8] = {};
    uint32_t backBufferCount_ = 0;
    std::atomic<bool> colourInjectArmed_{false};
    uint64_t colInjRecorded_ = 0;
    uint32_t colBarriersSeen_ = 0;
    // The one resource injection is permitted to touch, published by OnPresent
    // and read by ResourceBarrier_ on every recording thread. An atomic rather
    // than a lock because ResourceBarrier is the hottest hook in the process and
    // is called concurrently from every UE parallel-translate thread; when this
    // is null -- Stray always, inZOI until the probe converges -- the added cost
    // is one relaxed load and a branch not taken.
    std::atomic<ID3D12Resource*> injectTarget_{nullptr};
    std::atomic<uint64_t> injectFrame_{0};

    // Only the injected-copy token lives here. Render-pass nesting is
    // thread-local (see hooks_detours.cpp): a command list is a single-threaded
    // object, so a shared map for that was pure contention on the engine's
    // hottest path.
    struct ListState {
        Readback::InjectToken pendingToken{};
        // A list can carry BOTH a mask copy and a colour copy in the same frame.
        // One slot would let the second overwrite the first, and the overwritten
        // ring slot then never retires -- the ring starves and capture stops with
        // no error anywhere, which is the failure ReclaimStaleRecordings exists
        // to paper over. Two copies, two slots.
        Readback::InjectToken pendingColourToken{};
    };
    // Separate from mutex_ and NEVER nested with it: mutex_ is already held
    // across the barrier bookkeeping loop, and taking a second lock there would
    // serialise every recording thread in the game on us.
    std::mutex listMutex_;
    std::unordered_map<ID3D12GraphicsCommandList*, ListState> listState_;
    // Number of recorded-but-unsubmitted tokens. Read without the lock in
    // ExecuteCommandLists so the common case -- nothing outstanding -- costs one
    // relaxed load instead of a global mutex on every submit from every thread.
    std::atomic<uint32_t> outstandingTokens_{0};

    using BeginRenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT,
                                                       const void*, const void*, UINT);
    using EndRenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
    BeginRenderPassFn origBeginRenderPass_ = nullptr;
    EndRenderPassFn origEndRenderPass_ = nullptr;
    bool renderPassHooked_ = false;

    // Why an injection was declined, so a run that produces nothing says which
    // precondition failed instead of going quiet.
    uint64_t injAttempts_ = 0;
    uint64_t injRefusedRenderPass_ = 0;
    uint64_t injRefusedFlags_ = 0;
    uint64_t injRefusedSubresource_ = 0;
    uint64_t injRefusedNotWriteToRead_ = 0;
    uint64_t injRefusedNoSlot_ = 0;
    uint64_t injRecorded_ = 0;
    // Barriers that arrived on a non-DIRECT command list, where the game's own
    // StateBefore is not a state D3D12 permits.
    uint64_t injRefusedListType_ = 0;
    uint64_t injSubmitted_ = 0;
    bool loggedInjectArmed_ = false;
    void TryInjectColour(ID3D12GraphicsCommandList* list, ID3D12Resource* back,
                         D3D12_RESOURCE_STATES stateAfter, UINT subresource);
    bool IsBackBuffer(ID3D12Resource* r) const {
        for (uint32_t i = 0; i < backBufferCount_; ++i) {
            if (backBuffers_[i] == r) return true;
        }
        return false;
    }
    void TryInjectCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                       D3D12_RESOURCE_STATES stateAfter, UINT subresource);

    // ---- ground truth by intervention --------------------------------------
    //
    // Every other check we have is necessary and not sufficient, and one of them
    // once passed a whole session of masks that contained another render target
    // entirely. This is the only test that a coherent-but-wrong mask cannot pass:
    // turn ONE slot's flag off and require exactly its pixels to disappear.
    //
    // Runs as a state machine across frames because the unmark has to happen on
    // the game thread and the renderer takes a frame or two to rebuild the scene
    // proxy, so "before" and "after" cannot be adjacent frames.
    enum class Intervention { Off, Waiting, Requested, Settling, Done };
    Intervention interveneState_ = Intervention::Off;
    uint8_t interveneSlot_ = 0;
    // The component that held the slot when we unmarked it. The marker's own
    // churn can release and reissue a slot mid-measurement, and a different
    // object wearing the same number makes the pixel count meaningless.
    void* interveneComponent_ = nullptr;
    uint64_t intervenePixelsBefore_ = 0;
    uint64_t interveneOthersBefore_ = 0;
    uint64_t interveneFiredFrame_ = 0;
    uint32_t interveneStableFrames_ = 0;
    // Frames of settled recording before intervening, and frames to let the
    // scene proxy rebuild afterwards.
    static constexpr uint32_t kInterveneWarmupFrames = 90;
    static constexpr uint32_t kInterveneSettleFrames = 30;
    bool idChannelFound_ = false;
    bool loggedIdExhausted_ = false;
    // Dumps to spend before giving up on a candidate and advancing. Small,
    // because the leased-slot test is decisive on the first frame that has any
    // marks at all -- extra frames only cost time.
    static constexpr uint32_t kIdProbeAttemptsPerCandidate = 3;
    static constexpr uint32_t kIdProbeDumpBudget = 6;
    uint32_t maxCaptures_ = 600;   // 60 gave a 7-second demo at stride 4; a real video needs more
    uint64_t captureStride_ = 6;    // ~5/sec at 30fps: motion is visible without 600 near-identical frames
};

}  // namespace segcap
