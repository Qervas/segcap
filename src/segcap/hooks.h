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
#include <mutex>
#include <unordered_map>
#include <vector>

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
    uint32_t clearCount = 0;       // times cleared this frame
    uint32_t firstBindOrdinal = 0; // call ordinal within the frame
    D3D12_RESOURCE_STATES lastState = D3D12_RESOURCE_STATE_COMMON;
    bool everBoundAsDepth = false;
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
    static HRESULT STDMETHODCALLTYPE CreateCommittedResource_(
        ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heapProps,
        D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc,
        D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
        REFIID riid, void** resource);
    static void STDMETHODCALLTYPE ClearDepthStencilView_(ID3D12GraphicsCommandList* self,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                                         D3D12_CLEAR_FLAGS flags,
                                                         FLOAT depth, UINT8 stencil,
                                                         UINT numRects,
                                                         const D3D12_RECT* rects);

    void OnPresent();
    void NoteBind(ID3D12Resource* res, bool asDepth);
    void NoteClear(ID3D12Resource* res);

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
    std::unordered_map<ID3D12Resource*, TargetFingerprint> targets_;

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
};

}  // namespace segcap
