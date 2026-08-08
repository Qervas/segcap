#include "hooks.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdio>

#include "log.h"

namespace segcap {
namespace {

// Vtable slot indices. These are fixed by the COM interface layout, not by any
// particular D3D12 version, so they are stable -- but they are also the kind of
// magic number that is impossible to review, hence the names.
constexpr int kPresentSlot = 8;              // IDXGISwapChain::Present
constexpr int kExecuteCommandListsSlot = 10; // ID3D12CommandQueue::ExecuteCommandLists
constexpr int kCreateRTVSlot = 20;           // ID3D12Device::CreateRenderTargetView
constexpr int kCreateDSVSlot = 21;           // ID3D12Device::CreateDepthStencilView
constexpr int kCreateCommittedResourceSlot = 27; // ID3D12Device::CreateCommittedResource
constexpr int kResourceBarrierSlot = 26;     // ID3D12GraphicsCommandList::ResourceBarrier
constexpr int kOMSetRenderTargetsSlot = 46;  // ID3D12GraphicsCommandList::OMSetRenderTargets
constexpr int kClearDSVSlot = 47;            // ID3D12GraphicsCommandList::ClearDepthStencilView

void** VTableOf(void* obj) { return *reinterpret_cast<void***>(obj); }

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        default: return "other";
    }
}

bool HasStencilPlane(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

bool CreateHook(void** vtable, int slot, void* detour, void** original) {
    void* target = vtable[slot];
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        LogError("MH_CreateHook failed for vtable slot %d (target %p)", slot, target);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        LogError("MH_EnableHook failed for vtable slot %d", slot);
        return false;
    }
    return true;
}

}  // namespace

Hooks& Hooks::Get() {
    static Hooks instance;
    return instance;
}

// Creating a throwaway device, queue, swapchain and command list is the only
// portable way to read the vtable pointers: the interfaces are COM, so the
// function addresses live in a vtable shared by every instance in the process.
// The game's objects therefore share these addresses, and hooking here hooks
// the game's calls too.
bool Hooks::AcquireVTables() {
    ID3D12Device* dummyDevice = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&dummyDevice)))) {
        LogError("D3D12CreateDevice failed; is this process actually D3D12?");
        return false;
    }

    ID3D12CommandQueue* dummyQueue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dummyDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&dummyQueue)))) {
        LogError("CreateCommandQueue failed on dummy device");
        dummyDevice->Release();
        return false;
    }

    ID3D12CommandAllocator* dummyAlloc = nullptr;
    ID3D12GraphicsCommandList* dummyList = nullptr;
    bool ok = false;

    if (SUCCEEDED(dummyDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&dummyAlloc))) &&
        SUCCEEDED(dummyDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 dummyAlloc, nullptr,
                                                 IID_PPV_ARGS(&dummyList)))) {
        void** deviceVT = VTableOf(dummyDevice);
        void** queueVT = VTableOf(dummyQueue);
        void** listVT = VTableOf(dummyList);

        ok = CreateHook(queueVT, kExecuteCommandListsSlot,
                        reinterpret_cast<void*>(&Hooks::ExecuteCommandLists_),
                        reinterpret_cast<void**>(&origExecute_)) &&
             CreateHook(deviceVT, kCreateRTVSlot,
                        reinterpret_cast<void*>(&Hooks::CreateRenderTargetView_),
                        reinterpret_cast<void**>(&origCreateRTV_)) &&
             CreateHook(deviceVT, kCreateDSVSlot,
                        reinterpret_cast<void*>(&Hooks::CreateDepthStencilView_),
                        reinterpret_cast<void**>(&origCreateDSV_)) &&
             CreateHook(deviceVT, kCreateCommittedResourceSlot,
                        reinterpret_cast<void*>(&Hooks::CreateCommittedResource_),
                        reinterpret_cast<void**>(&origCreateCommitted_)) &&
             CreateHook(listVT, kResourceBarrierSlot,
                        reinterpret_cast<void*>(&Hooks::ResourceBarrier_),
                        reinterpret_cast<void**>(&origBarrier_)) &&
             CreateHook(listVT, kOMSetRenderTargetsSlot,
                        reinterpret_cast<void*>(&Hooks::OMSetRenderTargets_),
                        reinterpret_cast<void**>(&origOMSetRT_)) &&
             CreateHook(listVT, kClearDSVSlot,
                        reinterpret_cast<void*>(&Hooks::ClearDepthStencilView_),
                        reinterpret_cast<void**>(&origClearDSV_));

        if (ok) LogInfo("device/queue/commandlist vtables hooked");
    } else {
        LogError("could not create dummy command allocator/list");
    }

    if (dummyList) dummyList->Release();
    if (dummyAlloc) dummyAlloc->Release();
    dummyQueue->Release();
    dummyDevice->Release();
    return ok;
}

// Present lives on IDXGISwapChain, which cannot be created without an HWND.
// A hidden 1x1 window is enough -- the swapchain is never presented, only used
// to read the vtable that the game's swapchain also uses.
bool Hooks::AcquireSwapChainVTable() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"segcap_dummy";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        LogError("could not create dummy window for swapchain vtable");
        return false;
    }

    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    bool ok = false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.BufferCount = 2;
    scd.Width = 1;
    scd.Height = 1;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;

    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
        SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1))) {
        ok = CreateHook(VTableOf(sc1), kPresentSlot,
                        reinterpret_cast<void*>(&Hooks::Present_),
                        reinterpret_cast<void**>(&origPresent_));
        if (ok) LogInfo("swapchain vtable hooked (Present)");
    } else {
        LogError("could not create dummy swapchain");
    }

    if (sc1) sc1->Release();
    if (factory) factory->Release();
    if (queue) queue->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool Hooks::Install() {
    if (installed_) return true;

    if (MH_Initialize() != MH_OK) {
        LogError("MH_Initialize failed");
        return false;
    }
    if (!AcquireVTables() || !AcquireSwapChainVTable()) {
        MH_Uninitialize();
        return false;
    }

    installed_ = true;
    LogInfo("hooks installed");
    return true;
}

void Hooks::Uninstall() {
    if (!installed_) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    installed_ = false;
    LogInfo("hooks removed");
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
        auto* res = static_cast<ID3D12Resource*>(*resource);
        std::lock_guard<std::mutex> lock(h.mutex_);
        h.resourceState_[res] = initialState;
    }
    return hr;
}

void STDMETHODCALLTYPE Hooks::ClearDepthStencilView_(ID3D12GraphicsCommandList* self,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                                     D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                     UINT8 stencil, UINT numRects,
                                                     const D3D12_RECT* rects) {
    Hooks& h = Get();
    if (ID3D12Resource* res = h.ResolveDescriptor(dsv.ptr)) h.NoteClear(res);
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
    h.OnPresent();
    return h.origPresent_(self, sync, flags);
}

// Score candidates for "this is the CustomDepth target".
//
// Only runtime-observable signals are used. Debug names are excluded on
// purpose: they are stripped in shipping builds, so an election that leans on
// them is a technique that only works where it is not needed.
//
// The discriminator that matters is bind count. In Stray's captured frame the
// scene depth target took 1365 draws while the CustomDepth candidate took none
// -- three orders of magnitude apart. Once primitives opt in, CustomDepth still
// takes only a handful. So "full-res, has stencil, few binds, gets cleared" is
// the signature, and "most-bound target in the frame" is the thing to exclude.
std::vector<ElectionScore> Hooks::ScoreTargets(
    const std::vector<TargetFingerprint>& targets) const {
    uint32_t maxBinds = 0;
    // std::max, not max: NOMINMAX is defined project-wide so the Windows macro
    // does not exist here.
    for (const TargetFingerprint& t : targets) maxBinds = std::max(maxBinds, t.bindCount);

    std::vector<ElectionScore> out;
    for (const TargetFingerprint& t : targets) {
        ElectionScore s;
        s.resource = t.resource;

        // Hard requirements. Without a stencil plane there is nothing to read,
        // and a target that is not full-res is a shadow map or a downsample.
        if (!HasStencilPlane(t.format)) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: no stencil plane (%s)",
                        FormatName(t.format));
            out.push_back(s);
            continue;
        }
        const bool fullRes = backbufferWidth_ != 0 &&
                             t.width == backbufferWidth_ && t.height == backbufferHeight_;
        if (!fullRes) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: %llux%u != backbuffer %ux%u",
                        t.width, t.height, backbufferWidth_, backbufferHeight_);
            out.push_back(s);
            continue;
        }

        int score = 100;
        char detail[192] = "full-res + stencil";

        if (t.sampleCount == 1) {
            score += 20;
        } else {
            score -= 50;  // MSAA depth cannot be read back per-pixel meaningfully
            _snprintf_s(detail, _TRUNCATE, "%s; MSAA x%u penalised", detail, t.sampleCount);
        }

        // Cleared every frame is characteristic: UE clears CustomDepth even
        // when nothing renders into it, which is exactly how the candidate was
        // spotted in the RenderDoc capture.
        if (t.clearCount > 0) score += 30;

        // The most-bound target in the frame is the scene depth buffer, whose
        // stencil UE4 fully owns (sandbox bit, lighting channels, receive-decal
        // bit). Writing there would change what the player sees.
        if (maxBinds > 0 && t.bindCount == maxBinds && maxBinds > 8) {
            score -= 200;
            _snprintf_s(detail, _TRUNCATE,
                        "%s; MOST-BOUND (%u binds) => scene depth, stencil owned by UE4",
                        detail, t.bindCount);
        } else if (t.bindCount == 0 && t.clearCount > 0) {
            score += 40;
            _snprintf_s(detail, _TRUNCATE,
                        "%s; cleared but never bound => CustomDepth with no opt-ins",
                        detail);
        } else if (t.bindCount > 0 && t.bindCount <= 8) {
            score += 50;
            _snprintf_s(detail, _TRUNCATE, "%s; few binds (%u) => CustomDepth pass",
                        detail, t.bindCount);
        }

        s.score = score;
        _snprintf_s(s.reason, _TRUNCATE, "%s", detail);
        out.push_back(s);
    }

    std::sort(out.begin(), out.end(),
              [](const ElectionScore& a, const ElectionScore& b) { return a.score > b.score; });
    return out;
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

void Hooks::NoteClear(ID3D12Resource* res) {
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
}

std::vector<TargetFingerprint> Hooks::SnapshotTargets() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TargetFingerprint> out;
    out.reserve(targets_.size());
    for (auto& kv : targets_) out.push_back(kv.second);
    return out;
}

void Hooks::OnPresent() {
    ++frameIndex_;

    // Report periodically rather than every frame: this runs on the render
    // thread, and the log flushes each line.
    if (frameIndex_ % 300 != 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        targets_.clear();
        bindOrdinal_ = 0;
        return;
    }

    std::vector<TargetFingerprint> snapshot = SnapshotTargets();
    LogInfo("--- frame %llu: %zu distinct targets observed, %llu descriptor misses ---",
            frameIndex_, snapshot.size(), descriptorMisses_);

    for (const TargetFingerprint& t : snapshot) {
        // Only depth-stencil-capable targets matter for the mask route; logging
        // every colour target would bury them.
        if (!HasStencilPlane(t.format)) continue;
        LogInfo("  DS %p %-22s %llux%u samples=%u binds=%u clears=%u depth=%s state=0x%X",
                static_cast<void*>(t.resource), FormatName(t.format), t.width, t.height,
                t.sampleCount, t.bindCount, t.clearCount,
                t.everBoundAsDepth ? "yes" : "NO", StateOf(t.resource));
    }

    // The full score table is logged, not just the winner. A wrong election is
    // only debuggable if the runner-up and the reason it lost are visible.
    const std::vector<ElectionScore> ranked = ScoreTargets(snapshot);
    LogInfo("  election:");
    for (const ElectionScore& e : ranked) {
        LogInfo("    %+5d %p  %s", e.score, static_cast<void*>(e.resource), e.reason);
    }
    if (!ranked.empty() && ranked.front().score > 0) {
        LogInfo("  ELECTED %p (score %d)", static_cast<void*>(ranked.front().resource),
                ranked.front().score);
    } else {
        LogWarn("  no viable CustomDepth candidate this frame");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets_.clear();
        bindOrdinal_ = 0;
    }
}

}  // namespace segcap
