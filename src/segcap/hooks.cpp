#include "hooks.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <string>

// Linker-provided base of this module; avoids needing the HMODULE passed around
// just to find where our own DLL lives.
extern "C" IMAGE_DOS_HEADER __ImageBase;

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
// How much the currently elected target is favoured. Large enough that a
// marginally-better challenger cannot displace it, small enough that a target
// which stops being viable (goes to zero or negative) still loses.
constexpr int kIncumbencyBonus = 40;

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

    LogInfo("mode: %s", censusOnly_ ? "CENSUS ONLY (no GPU work issued)"
                                    : "capture (readback enabled)");

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
        // stencil UE4 fully owns -- sandbox bit, lighting channels, receive-decal
        // bit, all confirmed by reading actual stencil write masks out of a
        // RenderDoc capture of this game.
        //
        // This is a HARD rejection, not a penalty. An earlier version subtracted
        // 200, which the persistence bonus then partly cancelled, leaving scene
        // depth at a positive score -- so if the real CustomDepth target ever
        // disappeared we would elect it and emit masks made of lighting-channel
        // bits. "No viable candidate" is the correct answer in that situation;
        // a confidently wrong mask is worse than none.
        if (maxBinds > 0 && t.bindCount == maxBinds && maxBinds > 8) {
            _snprintf_s(s.reason, _TRUNCATE,
                        "rejected: MOST-BOUND (%u binds) => scene depth, stencil owned by UE4",
                        t.bindCount);
            out.push_back(s);
            continue;
        }
        if (t.bindCount == 0 && t.clearCount > 0) {
            score += 40;
            _snprintf_s(detail, _TRUNCATE,
                        "%s; cleared but never bound => CustomDepth with no opt-ins",
                        detail);
        } else if (t.bindCount > 0 && t.bindCount <= 8) {
            score += 50;
            _snprintf_s(detail, _TRUNCATE, "%s; few binds (%u) => CustomDepth pass",
                        detail, t.bindCount);
        }

        // Persistence. Against Stray, the genuine CustomDepth target appeared in
        // 1096 of 1099 census blocks spanning the whole 118s session, while two
        // transient loading-screen depth buffers lived ~25s and scored
        // identically on every per-frame signal. Without this term the election
        // flipped between those two on essentially every frame.
        if (t.framesSeen >= 600) {
            score += 60;
            _snprintf_s(detail, _TRUNCATE, "%s; persistent (%u frames)", detail, t.framesSeen);
        } else if (t.framesSeen >= 120) {
            score += 30;
            _snprintf_s(detail, _TRUNCATE, "%s; seen %u frames", detail, t.framesSeen);
        } else {
            _snprintf_s(detail, _TRUNCATE, "%s; new (%u frames)", detail, t.framesSeen);
        }

        // Incumbency. Switching targets mid-capture splits the mask stream
        // across two buffers, so a challenger must be clearly better, not
        // marginally. This is what actually stops the thrashing; persistence
        // alone would still flip while two candidates are neck and neck.
        if (t.resource == electedTarget_) {
            score += kIncumbencyBonus;
            _snprintf_s(detail, _TRUNCATE, "%s; INCUMBENT", detail);
        }

        s.score = score;
        _snprintf_s(s.reason, _TRUNCATE, "%s", detail);
        out.push_back(s);
    }

    // Ties broken by resource address, not left to sort order. Two candidates
    // scoring equally must still produce the same winner every frame -- an
    // arbitrary tie-break is a coin flip re-tossed 30 times a second.
    std::sort(out.begin(), out.end(), [](const ElectionScore& a, const ElectionScore& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.resource < b.resource;
    });
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

// Folds this frame's observations into the accumulated evidence and returns the
// evidence, not the frame.
//
// Electing from a single frame was the cause of residual flapping even after
// persistence and incumbency were added: a candidate not bound or cleared in a
// given frame is simply absent from that frame's map, so it cannot be elected
// no matter how good it is. Two candidates alternating their absence trade the
// election back and forth. Accumulated evidence removes the whole failure mode.
std::vector<TargetFingerprint> Hooks::SnapshotTargets() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& kv : targets_) {
        const TargetFingerprint& fresh = kv.second;
        TargetFingerprint& acc = evidence_[kv.first];
        if (acc.resource == nullptr) {
            acc = fresh;
            acc.framesSeen = 0;
        } else {
            // Carry the latest per-frame counts forward; the scoring thresholds
            // are expressed per frame, so accumulating them would misclassify a
            // long-lived CustomDepth pass as the most-bound target.
            acc.bindCount = fresh.bindCount;
            acc.clearCount = fresh.clearCount;
            acc.firstBindOrdinal = fresh.firstBindOrdinal;
            acc.everBoundAsDepth = acc.everBoundAsDepth || fresh.everBoundAsDepth;
        }
        ++acc.framesSeen;
        acc.lastSeenFrame = frameIndex_;
    }

    // Prune resources that have gone quiet. Without this, a destroyed
    // loading-screen buffer would keep winning on accumulated persistence long
    // after it stopped existing -- and its pointer could be reused by an
    // unrelated allocation.
    constexpr uint64_t kStaleAfterFrames = 600;
    for (auto it = evidence_.begin(); it != evidence_.end();) {
        if (frameIndex_ > it->second.lastSeenFrame + kStaleAfterFrames) {
            it = evidence_.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<TargetFingerprint> out;
    out.reserve(evidence_.size());
    for (const auto& kv : evidence_) out.push_back(kv.second);
    return out;
}

// Writes the stencil plane as a binary PGM. Chosen because it is trivially
// readable by any validator without a decoder, and because a mask is exactly a
// single-channel 8-bit image -- which is also a reminder that the channel is 8
// bits and therefore holds a per-frame slot, not an identity.
void Hooks::OnMaskReady(const MaskFrame& frame) {
    // Scan for content BEFORE deciding whether to dump.
    //
    // An earlier version dumped the first three frames unconditionally. Those
    // landed at t=3.1s, while primitives were not marked until t=41.2s -- so
    // every dump was of an empty buffer taken 38 seconds before there was
    // anything to see, and the empty result was misread as CustomDepth being
    // broken. The pipeline was correct; the shutter fired at the wrong moment.
    //
    // Dumping on CONTENT rather than on frame number removes the timing
    // coupling entirely: the capture happens when there is something to capture,
    // whenever that turns out to be.
    bool seen[256] = {};
    bool hasContent = false;
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.data + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; ++x) {
            if (row[x] != 0) hasContent = true;
            seen[row[x]] = true;
        }
    }

    // A few empty frames are still worth having as a baseline for the A/B
    // comparison in task 11, but they must not consume the content budget.
    if (!hasContent) {
        if (emptyMasksDumped_ >= 1) return;
        ++emptyMasksDumped_;
    } else if (masksDumped_ >= 4) {
        return;
    } else {
        ++masksDumped_;
    }

    // Absolute path next to the DLL. A relative filename resolves against the
    // GAME's working directory, which put three 4MB dumps inside Stray's install
    // folder -- writing our output into someone else's game directory.
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_mask_%llu.pgm", dir.c_str(),
                 frame.frameIndex);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;

    std::fprintf(f, "P5\n%u %u\n255\n", frame.width, frame.height);
    // Row by row using rowPitch, not width: the readback pitch is padded to 256
    // bytes, so treating it as tightly packed would shear the image.
    for (uint32_t y = 0; y < frame.height; ++y) {
        std::fwrite(frame.data + static_cast<size_t>(y) * frame.rowPitch, 1, frame.width, f);
    }
    std::fclose(f);

    // Report the distinct values present. For the fixture these must be exactly
    // {0} plus 1..16, which is the whole point of having known ground truth.
    char values[512] = {};
    size_t used = 0;
    for (int v = 0; v < 256 && used < sizeof(values) - 8; ++v) {
        if (!seen[v]) continue;
        used += static_cast<size_t>(
            _snprintf_s(values + used, sizeof(values) - used, _TRUNCATE, "%d ", v));
    }
    LogInfo("mask frame %llu dumped (%ux%u pitch=%u); distinct stencil values: %s",
            frame.frameIndex, frame.width, frame.height, frame.rowPitch, values);
    ++masksDumped_;
}

void Hooks::OnPresent() {
    ++frameIndex_;

    // Readback runs every frame; only the logging is periodic. Draining first
    // means a copy issued on frame N is collected here on frame N+2 or later,
    // which is what keeps the render thread off the GPU's critical path.
    readback_.Drain([this](const MaskFrame& f) { OnMaskReady(f); });

    // Election runs every frame too. It is cheap (a handful of targets) and the
    // elected target can legitimately change -- resolution changes, or a pass
    // that only appears in some frames. Electing only on logging frames would
    // mean reading a stale or never-set target for 299 frames out of 300.
    const std::vector<TargetFingerprint> snapshot = SnapshotTargets();
    const std::vector<ElectionScore> ranked = ScoreTargets(snapshot);
    ID3D12Resource* const winner =
        (!ranked.empty() && ranked.front().score > 0) ? ranked.front().resource : nullptr;
    const bool electionChanged = (winner != electedTarget_);
    electedTarget_ = winner;

    // Census-only mode issues no GPU work at all: no copy, no barriers, nothing
    // submitted on the game's queue. Used for first contact with an unfamiliar
    // title, where a wrong shadowed state would show up as a GPU hang rather
    // than an error message. Observe first, then act.
    if (!censusOnly_ && electedTarget_ && device_ && queue_) {
        if (readback_.Prepare(device_, electedTarget_)) {
            readback_.Enqueue(queue_, electedTarget_, StateOf(electedTarget_), frameIndex_);
        }
    }

    // Log periodically, but always on an election change: a target switching
    // mid-session is exactly the event worth seeing in the log.
    if (frameIndex_ % 300 != 1 && !electionChanged) {
        std::lock_guard<std::mutex> lock(mutex_);
        targets_.clear();
        bindOrdinal_ = 0;
        return;
    }

    LogInfo("--- frame %llu: %zu distinct targets observed, %llu descriptor misses ---",
            frameIndex_, snapshot.size(), descriptorMisses_);
    LogInfo("    readback submitted=%llu delivered=%llu dropped=%llu",
            readback_.submitted(), readback_.delivered(), readback_.dropped());

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
