#include "hooks.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

// Linker-provided base of this module; avoids needing the HMODULE passed around
// just to find where our own DLL lives.
extern "C" IMAGE_DOS_HEADER __ImageBase;

#include "customdepth.h"
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

// Capture budget, expressed as coverage in TIME rather than in frames.
//
// At 720p a mask is 900KB and a colour frame 3.6MB, so 150 captures is ~675MB.
// The stride is what matters: at 60fps a stride of 5 frames means 12 captures a
// second, so a 40-capture budget was spent in 3.3 seconds. One run recorded all
// 21 of its masks between t+201.4s and t+203.0s -- a 1.6-second window that
// happened to fall while the marking working set was still filling, so every
// mask contained exactly one object. The data was not wrong, it was a
// photograph of a transient.
//
// A stride of 60 gives one capture a second, so 150 of them span 150 seconds of
// gameplay: long enough to cross rooms, change what is on screen, and -- the
// reason it was raised from 30 -- long enough for objects to LEAVE the working
// set and come back. At stride 30 the captured window was 37 seconds, over
// which not one identity was released and re-acquired, so the analysis could
// not demonstrate that identity survives slot loss. The mechanism was fine; the
// observation window was too short to contain the event being claimed.
// Defaults. Overridable at runtime via segcap.captures / segcap.stride, because
// a demo run and an identity-analysis run want opposite settings and neither is
// "the" correct value. See Hooks::SetCaptureProfile.
constexpr uint32_t kMaxCaptures = 150;
constexpr uint64_t kCaptureStride = 60;

// A/B mode needs its budget spread across the WHOLE session, not the start of
// it. Marking cannot begin until ProcessEvent is verified and the level has
// loaded, which is ~8,000 frames in; at stride 30 the 150-frame budget was
// exhausted by frame 4,500 and every captured frame was from before the
// transition being measured. There was no "after" side at all.
constexpr uint64_t kAbCaptureStride = 120;   // ~2s at 60fps -> ~300s of coverage

constexpr int kCreateCommittedResourceSlot = 27; // ID3D12Device::CreateCommittedResource
// ID3D12Device vtable order after CreateCommittedResource(27) is
// CreateHeap(28), CreatePlacedResource(29), CreateReservedResource(30).
constexpr int kCreatePlacedResourceSlot = 29;    // ID3D12Device::CreatePlacedResource
constexpr int kResourceBarrierSlot = 26;     // ID3D12GraphicsCommandList::ResourceBarrier

// ID3D12GraphicsCommandList7::Barrier -- the Enhanced Barriers entry point.
//
// Derived by counting the interface chain rather than looked up in a table:
//   IUnknown 3, ID3D12Object 4, ID3D12DeviceChild 1, ID3D12CommandList 1  = 9
//   ID3D12GraphicsCommandList  51  -> 60
//   GCL1 6, GCL2 1, GCL3 1, GCL4 9, GCL5 2, GCL6 1                        -> 80
// Barrier is GCL7's only method, so slot 80.
//
// The same count puts ResourceBarrier at 9 + 18 - 1 = 26, which is the constant
// above and has been correct on two engines -- so the arithmetic is anchored to
// something already known good, and InstallBarrierHook re-checks that anchor at
// runtime before trusting slot 80. Guessing a vtable index is exactly the
// mistake that cost days on ProcessEvent; this one is derived and verified.
constexpr int kEnhancedBarrierSlot = 80;
constexpr int kOMSetRenderTargetsSlot = 46;  // ID3D12GraphicsCommandList::OMSetRenderTargets
constexpr int kClearDSVSlot = 47;            // ID3D12GraphicsCommandList::ClearDepthStencilView

void** VTableOf(void* obj) { return *reinterpret_cast<void***>(obj); }

// Never returns "other".
//
// It used to, and that hid evidence for the third time in this project. On
// inZOI, 800 of the observed render targets printed as "other" -- a single
// bucket covering every format this switch did not happen to list. Nanite's
// visibility buffer is an R32G32_UINT render target, so the one question worth
// asking of a UE5 census ("is the Nanite route available here?") was
// unanswerable from a log that had already thrown the answer away.
//
// Unknown formats now print their numeric DXGI_FORMAT, which is always enough
// to look up. A name I did not anticipate is not the same as no name.
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
        // Integer formats matter specifically: UE writes per-object and
        // per-cluster ids into UINT targets, so these are the candidates for an
        // id source that needs no engine mutation at all.
        case DXGI_FORMAT_R32G32_UINT: return "R32G32_UINT";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R16G16_UINT: return "R16G16_UINT";
        case DXGI_FORMAT_R16_UINT: return "R16_UINT";
        case DXGI_FORMAT_R8_UINT: return "R8_UINT";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        default: break;
    }
    // Thread-safe enough for logging: a small ring of buffers so several
    // formats can appear in one log line without clobbering each other.
    static thread_local char buf[4][24];
    static thread_local int which = 0;
    which = (which + 1) & 3;
    _snprintf_s(buf[which], sizeof(buf[which]), _TRUNCATE, "DXGI_FMT_%d",
                static_cast<int>(f));
    return buf[which];
}

// Milliseconds since the Unix epoch, from the system wall clock.
//
// Must be the SAME clock vpad.exe stamps its input log with, or the two cannot
// be joined -- they are different processes, so anything process-relative
// (QueryPerformanceCounter without a shared epoch, time since DLL attach) is
// meaningless across the boundary. GetSystemTimeAsFileTime is identical in both
// and has 100ns resolution, far finer than a 16ms frame.
long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

// Integer-typed render targets. UE uses these for identity data -- object ids,
// primitive ids, and Nanite's packed visible-cluster index.
bool IsIntegerFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8_UINT:
            return true;
        default:
            return false;
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
             CreateHook(deviceVT, kCreatePlacedResourceSlot,
                        reinterpret_cast<void*>(&Hooks::CreatePlacedResource_),
                        reinterpret_cast<void**>(&origCreatePlaced_)) &&
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

        // Enhanced Barriers, if this runtime exposes them.
        //
        // Verified, not assumed: QueryInterface must succeed AND the derived
        // slot must hold a code pointer AND the QI'd vtable must agree with the
        // base list at the known-good ResourceBarrier slot -- that last check is
        // what confirms the numbering, since both interfaces are the same object
        // and must share a vtable prefix.
        ID3D12GraphicsCommandList7* gcl7 = nullptr;
        if (SUCCEEDED(dummyList->QueryInterface(IID_PPV_ARGS(&gcl7))) && gcl7) {
            auto** vt7 = *reinterpret_cast<void***>(gcl7);
            const bool anchorOk = vt7[kResourceBarrierSlot] == listVT[kResourceBarrierSlot];
            if (!anchorOk) {
                LogError("GraphicsCommandList7 present but slot %d disagrees with the "
                         "base list -- refusing to hook slot %d on a numbering I cannot "
                         "confirm", kResourceBarrierSlot, kEnhancedBarrierSlot);
            } else if (CreateHook(vt7, kEnhancedBarrierSlot,
                                  reinterpret_cast<void*>(&Hooks::Barrier_),
                                  reinterpret_cast<void**>(&origEnhancedBarrier_))) {
                enhancedBarriersHooked_ = true;
                LogInfo("Enhanced Barriers hooked (ID3D12GraphicsCommandList7::Barrier, "
                        "slot %d) -- state shadow can now see them",
                        kEnhancedBarrierSlot);
            } else {
                LogError("failed to hook Enhanced Barriers at slot %d", kEnhancedBarrierSlot);
            }
            gcl7->Release();
        } else {
            LogInfo("ID3D12GraphicsCommandList7 not available; this runtime predates "
                    "Enhanced Barriers, so ResourceBarrier sees everything");
        }
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

void Hooks::AttachInfoQueue() {
    if (!d3dDebug_ || !device_ || infoQueue_) return;
    ID3D12InfoQueue* iq = nullptr;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) {
        LogWarn("info queue unavailable -- validation messages cannot be read");
        return;
    }
    // Deliberately NOT setting break-on-severity. A breakpoint in someone
    // else's process is an unrecoverable hang with no message, which is the
    // exact failure mode being diagnosed. Messages are pulled and logged
    // instead, so the game keeps running long enough to produce them.
    iq->SetMuteDebugOutput(FALSE);
    infoQueue_ = iq;
    LogInfo("D3D12 info queue attached");
}

// Called once per present. Drains whatever validation has accumulated into our
// own log, so the errors sit in the same timeline as the readback that caused
// them rather than in a debugger we are not attached to.
void Hooks::DrainInfoQueue() {
    if (!infoQueue_) return;
    const UINT64 n = infoQueue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(infoQueue_->GetMessage(i, msg, &len))) continue;
        if (msg->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;  // skip info/message
        const char* sev = msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                        : msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR      ? "ERROR"
                                                                             : "WARNING";
        LogError("D3D12 %s [id %d]: %.*s", sev, static_cast<int>(msg->ID),
                 static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
        ++d3dMessagesLogged_;
    }
    infoQueue_->ClearStoredMessages();
}

bool Hooks::Install() {
    if (installed_) return true;

    LogInfo("mode: %s", censusOnly_ ? "CENSUS ONLY (no GPU work issued)"
                                    : "capture (readback enabled)");

    // The D3D12 debug layer, if asked for.
    //
    // This MUST happen before the game creates its device, which is the one
    // thing injection actually gives us: the injector launches suspended and
    // waits for hooks to report ready before resuming the main thread, so we
    // are reliably ahead of the first D3D call.
    //
    // Why bother: inZOI dies 0.5-1.5s after the first readback, and everything
    // known about it so far is inferred from a corpse -- the log simply stops.
    // The debug layer turns "the copy kills it" into the actual reason, which
    // is the difference between a diagnosis and a good guess.
    if (d3dDebug_) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
            dbg->Release();
            LogWarn("D3D12 DEBUG LAYER ENABLED -- this is slow and is for "
                    "diagnosis only, never for a capture run");
        } else {
            // Not fatal, but say so loudly: a silent failure here would make
            // the absence of validation errors look like a clean bill of health.
            LogError("D3D12 debug layer requested but D3D12GetDebugInterface "
                     "failed -- the Graphics Tools optional feature is probably "
                     "not installed. NO validation is active this run.");
        }
    }

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

    // Establish the SCENE resolution, which is not necessarily the backbuffer
    // resolution.
    //
    // An earlier version hard-required t.width/height == backbuffer, on the
    // reasoning that anything smaller is a shadow map or a downsample. That
    // rejected every real depth target in a run where the game was configured
    // with ScreenPercentage=50: the backbuffer was 1280x720 but the entire 3D
    // scene -- including scene depth and CustomDepth -- rendered at 640x360.
    // The census then showed exactly one depth target, a 1x1 dummy, which reads
    // as "this game has no depth buffer" rather than "my filter ate them".
    //
    // Screen percentage, dynamic resolution and upsampling are all common, so
    // "equals the backbuffer" was never the right test. What actually
    // identifies the scene cohort is aspect ratio plus being the largest of
    // that shape: the scene targets share the backbuffer's aspect, while
    // shadow atlases are square and downsample chains are a fraction of it.
    uint64_t sceneW = 0;
    uint32_t sceneH = 0;
    if (backbufferWidth_ != 0 && backbufferHeight_ != 0) {
        // "Largest aspect-matching target" was wrong, and it failed exactly the
        // way a max-of-set rule always fails: ONE outlier redefines the answer
        // for everything else. inZOI produces an occasional 2560x1600
        // depth-stencil (a full-res pass over the upscaled image); the instant
        // it appeared, sceneW became 2560 and every real 1280x800 candidate was
        // hard-rejected as "1280x800 != scene 2560x1600". The election then held
        // an empty target for fifteen straight rounds, logged as "incumbent but
        // no content yet".
        //
        // The scene resolution is better defined as THE RESOLUTION THE GAME DOES
        // MOST OF ITS DEPTH RENDERING AT. Scene depth is bound far more than
        // anything else, so summing bind counts per resolution and taking the
        // maximum identifies it robustly -- and a rare full-res pass bound once
        // or twice cannot outvote the main depth buffer bound 15 times a frame.
        struct ResVotes {
            uint64_t width = 0;
            uint32_t height = 0;
            uint64_t binds = 0;
            uint32_t seen = 0;
        };
        std::vector<ResVotes> votes;
        for (const TargetFingerprint& t : targets) {
            if (!HasStencilPlane(t.format) || t.width == 0 || t.height == 0) continue;
            // Aspect match within 2%, done in integer arithmetic to avoid
            // float comparison entirely: w/h ~= bbW/bbH  <=>  w*bbH ~= h*bbW.
            const uint64_t lhs = t.width * static_cast<uint64_t>(backbufferHeight_);
            const uint64_t rhs = static_cast<uint64_t>(t.height) * backbufferWidth_;
            const uint64_t diff = lhs > rhs ? lhs - rhs : rhs - lhs;
            if (rhs == 0 || diff * 50 > rhs) continue;      // > 2% off: not the scene
            bool merged = false;
            for (ResVotes& v : votes) {
                if (v.width == t.width && v.height == t.height) {
                    v.binds += t.bindCount;
                    ++v.seen;
                    merged = true;
                    break;
                }
            }
            if (!merged) votes.push_back({t.width, t.height, t.bindCount, 1});
        }
        const ResVotes* best = nullptr;
        for (const ResVotes& v : votes) {
            // Most depth work wins; ties go to the resolution more targets share,
            // then to the larger one so the rule stays deterministic.
            if (!best || v.binds > best->binds ||
                (v.binds == best->binds &&
                 (v.seen > best->seen || (v.seen == best->seen && v.width > best->width)))) {
                best = &v;
            }
        }
        if (best) { sceneW = best->width; sceneH = best->height; }
    }

    std::vector<ElectionScore> out;
    for (const TargetFingerprint& t : targets) {
        ElectionScore s;
        s.resource = t.resource;

        // Hard requirements. Without a stencil plane there is nothing to read,
        // and a target below the scene resolution is a shadow map or a
        // downsample rather than a candidate.
        if (!HasStencilPlane(t.format)) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: no stencil plane (%s)",
                        FormatName(t.format));
            out.push_back(s);
            continue;
        }
        if (sceneW == 0) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: scene resolution not established yet");
            out.push_back(s);
            continue;
        }
        if (t.width != sceneW || t.height != sceneH) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: %llux%u != scene %llux%u",
                        t.width, t.height, sceneW, sceneH);
            out.push_back(s);
            continue;
        }

        int score = 100;
        char detail[192] = "scene-res + stencil";

        if (t.sampleCount == 1) {
            score += 20;
        } else {
            score -= 50;  // MSAA depth cannot be read back per-pixel meaningfully
            _snprintf_s(detail, _TRUNCATE, "%s; MSAA x%u penalised", detail, t.sampleCount);
        }

        // Cleared every frame is characteristic: UE clears CustomDepth even
        // when nothing renders into it, which is exactly how the candidate was
        // spotted in the RenderDoc capture.
        //
        // But WHICH plane was cleared is the discriminating part. Every depth
        // target gets its depth cleared, so a flag-blind clear counter is nearly
        // free information -- and on inZOI it was deciding a three-way tie
        // between identically-shaped 1280x800 targets, because it was the only
        // term separating them. Clearing STENCIL is the signal that someone
        // intends to write stencil, so it is scored far above a bare clear.
        if (t.stencilClearCount > 0) {
            score += 80;
            _snprintf_s(detail, _TRUNCATE, "%s; STENCIL cleared %ux", detail,
                        t.stencilClearCount);
        } else if (t.clearCount > 0) {
            score += 10;
            _snprintf_s(detail, _TRUNCATE, "%s; depth-only clear", detail);
        }

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

        // Incumbency, but EARNED rather than granted on arrival.
        //
        // Switching targets mid-capture splits the mask stream across two
        // buffers, so a challenger must be clearly better -- that is why the
        // bonus exists, and it did stop the thrashing it was written for.
        //
        // What it also did, unintentionally, was make the first guess
        // permanent. On one run the very first election happened at frame 2,
        // when only one scene-res candidate had been observed at all. It scored
        // 200 and became the incumbent. Two frames later the real CustomDepth
        // target appeared and scored 250 on its own merits -- but 230+40 beat
        // it every frame thereafter, so the wrong target stayed elected for the
        // entire session and every mask came back empty.
        //
        // The fix is to make incumbency conditional on the incumbent actually
        // having produced a non-empty mask. A target that has never yielded a
        // single non-zero stencil pixel has no stream worth protecting, so it
        // gets no protection and the election stays free to correct itself.
        // Once a target is genuinely producing data, the bonus applies and
        // thrash protection works as designed.
        if (t.resource == electedTarget_) {
            if (electedProducedContent_) {
                score += kIncumbencyBonus;
                _snprintf_s(detail, _TRUNCATE, "%s; INCUMBENT (producing)", detail);
            } else {
                _snprintf_s(detail, _TRUNCATE, "%s; incumbent but no content yet", detail);
            }
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
        // Has this ADDRESS changed identity since we last saw it?
        //
        // `fresh` was built from a live GetDesc in NoteBind during this frame,
        // so it is always the truth about whatever occupies the address now.
        // `acc` is what we have been scoring. When they disagree on the
        // immutable properties of a resource -- format and dimensions cannot
        // change without destroying and recreating it -- the previous occupant
        // is gone and the accumulated evidence belongs to a dead resource.
        //
        // This is the whole inZOI failure. The merge below deliberately carries
        // only the per-frame counters forward, which means format/width/height
        // were never refreshed after the first sighting. UE5's pooled allocator
        // recycles render-target addresses within a frame or two, so a dead
        // depth-stencil's format stayed attached to its address permanently
        // while the new occupant's binds were merged on top of it. The election
        // then chose a "1280x800 depth-stencil" that was really a 512x512
        // R8G8B8A8_TYPELESS, and the readback failed to size a stencil plane
        // that did not exist.
        //
        // The staleness prune below cannot catch this: the address keeps being
        // bound by its NEW owner, so lastSeenFrame keeps refreshing and the
        // entry never goes quiet. Identity has to be checked, not just liveness.
        const bool identityChanged =
            acc.resource != nullptr &&
            (acc.format != fresh.format || acc.width != fresh.width ||
             acc.height != fresh.height || acc.sampleCount != fresh.sampleCount);
        if (identityChanged) {
            ++addressRecycles_;
            if (addressRecycles_ <= 8 || (addressRecycles_ % 100) == 0) {
                LogWarn("address %p changed identity (%llux%u %s -> %llux%u %s); "
                        "discarding %llu frames of evidence (recycle #%llu)",
                        static_cast<void*>(kv.first), acc.width, acc.height,
                        FormatName(acc.format), fresh.width, fresh.height,
                        FormatName(fresh.format), acc.framesSeen, addressRecycles_);
            }
            if (kv.first == electedTarget_) {
                ++electedRecycles_;
                LogWarn("the ELECTED target %p is the one that changed identity; "
                        "election voided", static_cast<void*>(kv.first));
                electedTarget_ = nullptr;
                electedProducedContent_ = false;
            }
            resourceState_.erase(kv.first);
            barriersSeen_.erase(kv.first);
            textureLayout_.erase(kv.first);
            acc = TargetFingerprint{};
        }

        if (acc.resource == nullptr) {
            acc = fresh;
            acc.framesSeen = 0;
        } else {
            // Carry the latest per-frame counts forward; the scoring thresholds
            // are expressed per frame, so accumulating them would misclassify a
            // long-lived CustomDepth pass as the most-bound target.
            acc.bindCount = fresh.bindCount;
            acc.clearCount = fresh.clearCount;
            acc.stencilClearCount = fresh.stencilClearCount;
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
    // When the mask comes from a multi-byte id buffer rather than a stencil
    // plane, flatten the proven channel to 8 bits FIRST. Everything downstream --
    // the content scan, the leased-slot check, the PGM writer, every tool in
    // tools/ -- is written against one byte per pixel, and a slot is 1..255 by
    // construction (IdentityRegistry::kSlotCount), so nothing is lost.
    MaskFrame view = frame;
    if (frame.bytesPerPixel != 1 && maskChannel_ >= 0) {
        maskScratch_.assign(static_cast<size_t>(frame.width) * frame.height, 0);
        const uint32_t bpp = frame.bytesPerPixel;
        for (uint32_t y = 0; y < frame.height; ++y) {
            const uint8_t* src = frame.data + static_cast<size_t>(y) * frame.rowPitch;
            uint8_t* dst = maskScratch_.data() + static_cast<size_t>(y) * frame.width;
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint8_t* p = src + static_cast<size_t>(x) * bpp;
                uint32_t v = 0;
                if (bpp == 2) {
                    v = maskChannel_ == 0
                            ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8))
                            : (maskChannel_ == 1 ? p[0] : p[1]);
                } else {
                    const uint32_t w = static_cast<uint32_t>(p[0]) |
                                       (static_cast<uint32_t>(p[1]) << 8) |
                                       (static_cast<uint32_t>(p[2]) << 16) |
                                       (static_cast<uint32_t>(p[3]) << 24);
                    v = maskChannel_ == 0 ? (w & 0xFFFFu)
                                          : (maskChannel_ == 1 ? ((w >> 16) & 0xFFFFu) : w);
                }
                // Anything outside the slot range is not one of ours; drop it to
                // 0 (unlabelled) rather than let it alias onto a real slot.
                dst[x] = v < 256 ? static_cast<uint8_t>(v) : 0;
            }
        }
        view.data = maskScratch_.data();
        view.rowPitch = frame.width;
        view.bytesPerPixel = 1;
    }
    const MaskFrame& mf = view;

    bool seen[256] = {};
    for (uint32_t y = 0; y < mf.height; ++y) {
        const uint8_t* row = mf.data + static_cast<size_t>(y) * mf.rowPitch;
        for (uint32_t x = 0; x < mf.width; ++x) {
            seen[row[x]] = true;
        }
    }
    uint32_t distinctIds = 0;
    for (int i = 1; i < 256; ++i) {
        if (seen[i]) ++distinctIds;
    }

    // "Content" means a USEFUL mask, not merely a non-empty one.
    //
    // Gating on "any non-zero pixel" starts recording the instant the first
    // primitive is marked -- which is the worst possible moment, because the
    // working set then takes another 30-60 seconds to fill to 255 slots. With a
    // dense capture stride the entire budget was spent inside that ramp: a
    // 10-second demo where every frame showed 11 objects and 29% coverage,
    // while the same build 40 seconds later was doing 80 objects and 100%.
    //
    // This is the third time in this project the shutter fired at the wrong
    // moment (see DEBUGGING.md 7.7 and the note above). The pattern is always
    // the same -- a trigger condition that is technically satisfied long before
    // the thing it is meant to detect is actually happening.
    constexpr uint32_t kMinIdsToStart = 24;

    // ...and the ids in the buffer must be OURS.
    //
    // Fourth time, and the worst of the four, because this one produced output
    // that looked right. On inZOI the marker never resolved -- UE5's property
    // chains were not populated when it looked -- so segcap marked exactly zero
    // primitives. The elected target still had a stencil plane, because the
    // engine uses stencil for its own purposes, and this gate happily counted
    // 154 distinct values in it and started recording. The result was
    // 2560x1600 masks with a clean-looking 0..153 id range that were entirely
    // the game's own stencil bits and contained no object identity whatsoever.
    //
    // Every downstream tool would have accepted them. The sidecar would have
    // been empty or wrong, which is the only reason it was caught.
    //
    // "The buffer has content" and "the content is content we produced" are
    // different claims, and only the second one justifies writing a mask.
    const size_t ourMarks = GetMarker().markedCount();

    // ...and FIFTH time, worse again, because this one survived a PASS from the
    // validator. On inZOI the buffer was full of plausible-looking ids and every
    // check above was satisfied: 255 distinct values, 187 primitives marked. The
    // pixels were a 2-channel fp16 velocity buffer read through an 8-bit stencil
    // footprint -- another render target's memory entirely.
    //
    // "We marked something" and "these pixel values are the ones we handed out"
    // are STILL different claims. The sidecar knows exactly which slots were
    // leased, so the buffer can be checked against it directly: a value we never
    // leased cannot be ours, whatever it looks like.
    //
    // Stray measures 0% unleased. inZOI measured 5-23%, swinging frame to frame.
    // A tolerance is needed rather than zero because a primitive that hands its
    // slot back keeps drawing for a frame or two until its proxy is rebuilt
    // (see SlotBinding::released), so a small unleased fraction is legitimate.
    uint32_t unleasedIds = 0;
    uint64_t unleasedPixels = 0;
    uint64_t labelledPixels = 0;
    bool leaseCheckPossible = false;

    // Check against the table that was live WHEN THIS FRAME WAS SUBMITTED, not
    // the one live now. The readback ring is three deep and marking runs
    // continuously, so the current table can be several passes ahead -- and this
    // check compares individual pixel values against it, which makes it maximally
    // sensitive to that drift. Measured: 62.8% "unleased" against the current
    // table on a frame that was fine against its own.
    //
    // The dump below already does exactly this lookup for exactly this reason,
    // with the measured cost of getting it wrong recorded beside it. I wrote the
    // check without reusing it.
    std::shared_ptr<const FrameSidecar> atSubmission;
    for (const auto& entry : sidecarHistory_) {
        if (entry.first == frame.frameIndex && entry.second) {
            atSubmission = entry.second;
            break;
        }
    }
    if (!atSubmission) atSubmission = GetMarker().publishedSidecar();
    if (auto sc = atSubmission) {
        bool leased[256] = {};
        for (const auto& b : sc->bindings) {
            if (b.slot > 0 && b.slot < 256) leased[b.slot] = true;
        }
        leaseCheckPossible = !sc->bindings.empty();
        for (int i = 1; i < 256; ++i) {
            if (seen[i] && !leased[i]) ++unleasedIds;
        }
        for (uint32_t y = 0; y < mf.height; ++y) {
            const uint8_t* row = mf.data + static_cast<size_t>(y) * mf.rowPitch;
            for (uint32_t x = 0; x < mf.width; ++x) {
                const uint8_t v = row[x];
                if (!v) continue;
                ++labelledPixels;
                if (!leased[v]) ++unleasedPixels;
            }
        }
    }
    const double unleasedFrac =
        labelledPixels ? static_cast<double>(unleasedPixels) / static_cast<double>(labelledPixels)
                       : 0.0;
    constexpr double kMaxUnleasedFraction = 0.02;
    const bool idsAreOurs = !leaseCheckPossible || unleasedFrac <= kMaxUnleasedFraction;

    const bool hasContent = distinctIds >= kMinIdsToStart && ourMarks > 0 && idsAreOurs;

    if (leaseCheckPossible && !idsAreOurs && !warnedUnleasedIds_) {
        warnedUnleasedIds_ = true;
        LogError("capture: REFUSING to record -- %.1f%% of labelled pixels carry stencil "
                 "values we never leased (%u distinct unleased ids of %u present). Whatever "
                 "this buffer is, it is not our CustomDepth output. Elected target %p.",
                 100.0 * unleasedFrac, unleasedIds, distinctIds,
                 static_cast<void*>(electedTarget_));
    }

    if (distinctIds >= kMinIdsToStart && ourMarks == 0 && !warnedForeignStencil_) {
        warnedForeignStencil_ = true;
        LogWarn("capture: elected target holds %u distinct stencil values but WE HAVE "
                "MARKED NOTHING -- this is the engine's own stencil, not object ids. "
                "Not recording. (Marker resolved? see 'customdepth:' lines.)",
                distinctIds);
    }

    // ---- recording gate ----------------------------------------------------
    //
    // Only record while actually in gameplay. Menus, loading screens and fades
    // contain no labelled objects, so capturing them would pad the dataset with
    // frames that teach nothing -- and for a dataset pipeline that is worse than
    // capturing less.
    //
    // The gate keys on mask content rather than trying to infer "is this a menu"
    // from engine state, because content is exactly the property that makes a
    // frame worth keeping. Hysteresis both ways: a camera can briefly face an
    // unmarked wall without ending a session, and a stray marked object visible
    // behind a menu should not start one.
    constexpr uint32_t kFramesToStart = 3;
    constexpr uint32_t kFramesToStop = 30;

    if (hasContent) {
        // The elected target has now demonstrably produced a mask, which is
        // what entitles it to incumbency protection in future elections. This
        // is the only place that flag is ever set, so an unproductive target
        // can never acquire it.
        electedProducedContent_ = true;
        ++consecutiveContent_;
        consecutiveEmpty_ = 0;
        if (!recording_ && consecutiveContent_ >= kFramesToStart) {
            recording_ = true;
            recordingStartedFrame_ = frame.frameIndex;
            LogInfo("RECORDING STARTED at frame %llu (%u distinct ids in the mask)",
                    frame.frameIndex, distinctIds);
        }
    } else {
        ++consecutiveEmpty_;
        consecutiveContent_ = 0;
        if (recording_ && consecutiveEmpty_ >= kFramesToStop) {
            recording_ = false;
            LogInfo("RECORDING STOPPED at frame %llu (%llu frames captured over %llu)",
                    frame.frameIndex, recordedFrames_,
                    frame.frameIndex - recordingStartedFrame_);
        }
    }

    if (!recording_) {
        ++skippedFrames_;
        // One baseline empty capture is still useful for the task-11 A/B diff.
        if (emptyMasksDumped_ >= 1) return;
        ++emptyMasksDumped_;
    } else {
        ++recordedFrames_;
        if (masksDumped_ >= maxCaptures_) return;
        // Capture every Nth frame rather than N consecutive ones. At ~30fps a
        // run of consecutive frames is under two seconds of game time and shows
        // almost no motion, which makes a useless demo. Striding spreads the
        // same budget across ~20 seconds so objects actually move, enter, and
        // leave -- which is also what makes the identity/slot behaviour visible.
        if ((recordedFrames_ % captureStride_) != 1) return;
        ++masksDumped_;
        maskKept_.insert(frame.frameIndex);
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
    for (uint32_t y = 0; y < mf.height; ++y) {
        std::fwrite(mf.data + static_cast<size_t>(y) * mf.rowPitch, 1, mf.width, f);
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

    // The sidecar is what makes the mask decodable. Written for every dumped
    // mask, never separately, so a mask file cannot exist without the table
    // that says what its slot numbers mean.
    //
    // NOTE: masksDumped_ is incremented by the recording gate above, not here.
    // It used to be incremented in both places, which silently halved every
    // capture budget -- asking for 150 produced 75 masks and asking for 200
    // produced 100. It looked like a plausible number every single time, and
    // was only caught by requesting a round 200 and getting exactly half.
    WriteSidecar(frame);
}

// Reports what is actually inside a scene-resolution integer render target.
//
// The question this answers is not "does a buffer exist" -- the census already
// said yes -- but "does it contain per-object identity". Three signals separate
// the plausible answers, and none of them require knowing UE's internals:
//
//   how many distinct values      a handful  -> material or shading class
//                                 hundreds+  -> per-object or per-cluster ids
//   how the top values cluster    contiguous regions -> spatial objects
//                                 scattered          -> not per-object
//   whether 0 dominates           a mostly-empty buffer is a mask of something
//                                 specific, not a general id buffer
//
// A PGM is written alongside, colouring by value hash, because a histogram
// cannot show whether the regions line up with objects and a picture can.
void Hooks::OnIdBufferReady(const MaskFrame& frame) {
    // 1, 2 and 4 bytes per pixel are all legitimate id-buffer widths and this
    // used to reject everything but 4. inZOI presents an R8_UINT at exactly
    // scene resolution, bound once per frame -- an 8-bit single-channel integer
    // target whose range is precisely our 1..255 slot range, i.e. the single
    // most promising candidate in the whole census -- and the width check would
    // have discarded it with a one-line warning.
    if (frame.bytesPerPixel != 1 && frame.bytesPerPixel != 2 && frame.bytesPerPixel != 4) {
        LogWarn("idbuf: unsupported %u bytes/pixel", frame.bytesPerPixel);
        return;
    }
    ++idDumped_;

    // ---- does THIS candidate hold OUR ids? ---------------------------------
    //
    // The whole reason to enumerate candidates is to answer this by measurement.
    // A 4-byte integer texel is read three ways -- as R16 (low half), as G16
    // (high half), and as the whole 32-bit value -- because which channel
    // carries the stencil depends on the format and on the platform's channel
    // order. UE's Nanite export puts the custom stencil in .g on D3D.
    //
    // The test is the same one that guards the mask path: a value we never
    // leased cannot be ours. A channel where most labelled texels ARE leased
    // slots is the id channel, and that is a conclusion, not a guess.
    if (auto sc = GetMarker().publishedSidecar()) {
        // Bit-packed (8 KB) rather than a 64 KB bool array: this runs on the
        // render thread, and 64 KB of stack per frame is not worth the risk.
        std::vector<bool> leased(65536, false);
        size_t leasedCount = 0;
        for (const auto& b : sc->bindings) {
            if (b.slot > 0 && b.slot < 65536) {
                if (!leased[b.slot]) ++leasedCount;
                leased[b.slot] = true;
            }
        }
        if (leasedCount > 0) {
            uint64_t nonZero[3] = {0, 0, 0};
            uint64_t hit[3] = {0, 0, 0};
            const uint32_t bpp = frame.bytesPerPixel;
            for (uint32_t y = 0; y < frame.height; ++y) {
                const uint8_t* row = frame.data + static_cast<size_t>(y) * frame.rowPitch;
                for (uint32_t x = 0; x < frame.width; ++x) {
                    const uint8_t* p = row + static_cast<size_t>(x) * bpp;
                    // Decompose the texel every way an id could plausibly be
                    // stored at this width. Which channel actually carries the
                    // stencil depends on format and platform channel order --
                    // UE's Nanite export puts it in .g on D3D -- so all of them
                    // are measured and the data picks the winner.
                    uint32_t chan[3] = {0, 0, 0};
                    if (bpp == 1) {
                        chan[0] = p[0];
                    } else if (bpp == 2) {
                        chan[0] = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
                        chan[1] = p[0];
                        chan[2] = p[1];
                    } else {
                        const uint32_t v = static_cast<uint32_t>(p[0]) |
                                           (static_cast<uint32_t>(p[1]) << 8) |
                                           (static_cast<uint32_t>(p[2]) << 16) |
                                           (static_cast<uint32_t>(p[3]) << 24);
                        chan[0] = v & 0xFFFFu;
                        chan[1] = (v >> 16) & 0xFFFFu;
                        chan[2] = v;
                    }
                    for (int c = 0; c < 3; ++c) {
                        if (!chan[c]) continue;
                        ++nonZero[c];
                        if (chan[c] < 65536 && leased[chan[c]]) ++hit[c];
                    }
                }
            }
            const char* names1[3] = {"R8", "-", "-"};
            const char* names2[3] = {"R16", "byte0", "byte1"};
            const char* names4[3] = {"R16(low)", "G16(high)", "R32(whole)"};
            const char* const* names = bpp == 1 ? names1 : (bpp == 2 ? names2 : names4);
            int bestChan = -1;
            double bestFrac = 0.0;
            for (int c = 0; c < 3; ++c) {
                const double frac =
                    nonZero[c] ? static_cast<double>(hit[c]) / static_cast<double>(nonZero[c])
                               : 0.0;
                LogInfo("idbuf   channel %-10s: %llu non-zero texels, %.1f%% carry a slot we "
                        "leased",
                        names[c], nonZero[c], 100.0 * frac);
                if (frac > bestFrac) { bestFrac = frac; bestChan = c; }
            }
            // A channel of unrelated data will land near zero; genuine ids land
            // high. Half is a wide margin between those two outcomes.
            constexpr double kIdChannelThreshold = 0.50;
            if (bestChan >= 0 && bestFrac >= kIdChannelThreshold) {
                LogWarn("idbuf: FOUND OUR IDS in %p channel %s (%.1f%% of non-zero texels are "
                        "leased slots, %zu slots leased). This is the per-object id buffer.",
                        static_cast<void*>(idTarget_), names[bestChan], 100.0 * bestFrac,
                        leasedCount);
                idChannelFound_ = true;
                // Switch the MASK pipeline onto this buffer. On a Nanite title
                // the depth-stencil route cannot work at all: Nanite exports
                // depth to CombinedCustomDepth (whose stencil plane is never
                // written) and the stencil VALUE to this separate colour target,
                // which the election was hard-rejecting as "no stencil plane".
                maskSource_ = idTarget_;
                maskChannel_ = bestChan;
                maskSourceBpp_ = bpp;
                // Pin it, for the same reason the elected depth-stencil is
                // pinned: D3D12 recycles addresses and UE5's transient allocator
                // reissues heap ranges within a frame, and this is now the one
                // resource every captured mask comes from.
                maskSource_->AddRef();
                LogWarn("capture: mask source switched to the id buffer %p (channel %s). "
                        "The CustomDepth stencil-plane route does not carry ids on this "
                        "title.",
                        static_cast<void*>(maskSource_), names[bestChan]);
            } else if (idDumped_ >= kIdProbeAttemptsPerCandidate) {
                // Abandon this candidate permanently and pick another next
                // frame. Recorded in idRejected_ rather than by index, because
                // the candidate list is rebuilt each time and an index would
                // point at a different resource once new targets appear.
                LogInfo("idbuf: candidate %p holds none of our slots (best %.1f%% over %zu "
                        "leased); rejected, will try another",
                        static_cast<void*>(idTarget_), 100.0 * bestFrac, leasedCount);
                idRejected_.insert(idTarget_);
                idTarget_ = nullptr;
                idDumped_ = 0;
            }
        }
    }

    std::unordered_map<uint32_t, uint32_t> hist;
    hist.reserve(4096);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.data + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * frame.bytesPerPixel;
            uint32_t v = 0;
            for (uint32_t b = 0; b < frame.bytesPerPixel; ++b) {
                v |= static_cast<uint32_t>(p[b]) << (8 * b);
            }
            ++hist[v];
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> top(hist.begin(), hist.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    const size_t total = static_cast<size_t>(frame.width) * frame.height;
    LogInfo("idbuf frame %llu: %ux%u fmt=%u -- %zu DISTINCT VALUES over %zu pixels",
            frame.frameIndex, frame.width, frame.height, frame.format, hist.size(), total);
    for (size_t i = 0; i < top.size() && i < 12; ++i) {
        LogInfo("idbuf   0x%08X  %8u px  %5.2f%%", top[i].first, top[i].second,
                100.0 * top[i].second / static_cast<double>(total));
    }

    // Visualise by hashing the value into a byte. Distinct ids become distinct
    // greys; a buffer with four values looks like four flat regions and a
    // per-object buffer looks like the scene.
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_idbuf_%llu.pgm", dir.c_str(), frame.frameIndex);
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    std::fprintf(f, "P5\n%u %u\n255\n", frame.width, frame.height);
    std::vector<uint8_t> row(frame.width);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const auto* src =
            reinterpret_cast<const uint32_t*>(frame.data + static_cast<size_t>(y) * frame.rowPitch);
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint32_t v = src[x];
            // Cheap avalanche so neighbouring ids get far-apart greys.
            uint32_t h = v * 2654435761u;
            h ^= h >> 16;
            row[x] = v == 0 ? 0 : static_cast<uint8_t>(16 + (h % 240));
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LogInfo("idbuf: wrote %ls", path);
}

// Writes the colour backbuffer as a binary PPM (P6).
//
// PPM rather than PNG because the DLL has no image encoder and adding one to
// code that runs inside someone else's process is not worth it. The validator
// converts. Frames are named by the same monotonic index as the masks, which is
// what lets a mask and its frame be paired by filename alone.
void Hooks::OnColourReady(const MaskFrame& frame) {
    if (colourDumped_ >= maxCaptures_) return;

    // A/B mode captures colour UNCONDITIONALLY on a stride.
    //
    // The normal rule -- keep a colour frame only if its mask was kept -- is
    // right for the dataset and useless for the A/B test, because the whole
    // point of the "before" side is that there is no mask yet. Without this the
    // baseline condition produces zero frames and there is nothing to compare.
    if (abTest_) {
        if ((frame.frameIndex % kAbCaptureStride) != 0) return;
        ++colourDumped_;
    } else {
        if (!recording_) return;
        // Only keep colour frames whose mask was also kept. An unpaired frame is
        // dead weight -- make_demo can only use indices that have both.
        if (!maskKept_.count(frame.frameIndex)) return;
        ++colourDumped_;
    }

    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_frame_%llu.ppm", dir.c_str(),
                 frame.frameIndex);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);

    std::vector<uint8_t> row(static_cast<size_t>(frame.width) * 3);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint32_t px = *reinterpret_cast<const uint32_t*>(src + x * 4);
            if (frame.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                // 10 bits per channel packed into 32; shift down to 8. Stray
                // renders to an HDR10 backbuffer, so a naive byte read would
                // give channel-swapped garbage.
                row[x * 3 + 0] = static_cast<uint8_t>(((px >> 0) & 0x3FF) >> 2);
                row[x * 3 + 1] = static_cast<uint8_t>(((px >> 10) & 0x3FF) >> 2);
                row[x * 3 + 2] = static_cast<uint8_t>(((px >> 20) & 0x3FF) >> 2);
            } else {
                // Assume 8-bit BGRA/RGBA.
                row[x * 3 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF);
                row[x * 3 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
                row[x * 3 + 2] = static_cast<uint8_t>((px >> 0) & 0xFF);
            }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LogInfo("colour frame %llu dumped (%ux%u fmt=%u)", frame.frameIndex, frame.width,
            frame.height, frame.format);
}

// Writes the slot -> object table for one frame, as JSON next to the mask.
//
// A mask alone is ambiguous: the stencil channel is 8 bits, so slot 42 in one
// frame and slot 42 in another can be different objects. The sidecar is what
// resolves it, and it is emitted with EVERY mask so the two cannot become
// separated. A mask file without its sidecar is undecodable, and the format
// should make that impossible to forget rather than merely documented.
void Hooks::WriteSidecar(const MaskFrame& frame) const {
    // Use the table as it stood WHEN THE COPY WAS SUBMITTED, not as it stands
    // now.
    //
    // Readback is asynchronous by design -- the copy is queued on the game's own
    // command list and collected several frames later, which is what keeps the
    // render thread off the GPU's critical path. Meanwhile the marking thread is
    // continuously releasing slots for primitives that left the screen and
    // leasing them to new ones. So by the time a mask lands, the live slot table
    // can already describe a different set of objects than the one that wrote
    // that mask's pixels.
    //
    // The overlay is what exposed this: a gameplay mask contained ids 154, 156,
    // 242 and 255 that had no binding at all in the sidecar written beside it --
    // 5.4% of the frame's pixels labelled with an id whose meaning had already
    // been recycled. Reading the table at dump time is exactly the kind of
    // "close enough" that produces a dataset with quietly wrong labels.
    FrameSidecar sc;
    bool fromHistory = false;
    for (const auto& entry : sidecarHistory_) {
        if (entry.first == frame.frameIndex && entry.second) {
            sc = *entry.second;
            fromHistory = true;
            break;
        }
    }
    if (!fromHistory) {
        // Fall back to the live table rather than emitting nothing, but say so:
        // a sidecar that might not correspond to its mask is worth knowing about
        // rather than silently trusting.
        sc = GetMarker().SnapshotSidecar(frame.frameIndex, frame.width, frame.height);
        LogWarn("sidecar for frame %llu not found in history; using the live table, "
                "which may describe a different moment", frame.frameIndex);
    }
    sc.frameIndex = frame.frameIndex;
    sc.width = frame.width;
    sc.height = frame.height;

    long long stamp = 0;
    for (const auto& e : frameStamps_) {
        if (e.first == frame.frameIndex) { stamp = e.second; break; }
    }

    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_mask_%llu.json", dir.c_str(),
                 frame.frameIndex);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"frameIndex\": %llu,\n", sc.frameIndex);
    // Wall clock at Present, on the same clock vpad.exe stamps its input log
    // with. This is the join key between what was rendered and what was pressed.
    std::fprintf(f, "  \"timestampMs\": %lld,\n", stamp);
    std::fprintf(f, "  \"width\": %u,\n", sc.width);
    std::fprintf(f, "  \"height\": %u,\n", sc.height);
    std::fprintf(f, "  \"bindings\": [\n");
    for (size_t i = 0; i < sc.bindings.size(); ++i) {
        const SlotBinding& b = sc.bindings[i];
        // "released" marks a trailing binding: the object handed its slot back
        // but its render proxy may not have caught up, so these pixels can
        // still appear for a frame or two. A consumer that wants only current
        // labels can filter on it; without it those pixels would simply be
        // undecodable.
        std::fprintf(f,
                     "    {\"slot\": %u, \"stableId\": %llu, \"className\": \"%s\", "
                     "\"objectName\": \"%s\", \"serial\": %d, \"released\": %s}%s\n",
                     b.slot, b.stableId, b.className.c_str(), b.objectName.c_str(),
                     b.serialNumber, b.released ? "true" : "false",
                     (i + 1 < sc.bindings.size()) ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

void Hooks::OnPresent(IDXGISwapChain3* swapChain) {
    ++frameIndex_;

    // Readback runs every frame; only the logging is periodic. Draining first
    // means a copy issued on frame N is collected here on frame N+2 or later,
    // which is what keeps the render thread off the GPU's critical path.
    // Before the readback, so anything the previous frame's copy provoked is
    // already in the log by the time this frame's copy is submitted.
    DrainInfoQueue();

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
    if (electionChanged) {
        // A new target has produced nothing yet by definition, so it starts
        // without incumbency protection and must earn it the same way.
        electedProducedContent_ = false;

        // PIN the winner. Purging on creation (see CreateCommittedResource_)
        // keeps the bookkeeping honest, but it is reactive: between the moment
        // the game releases the elected target and the moment something else is
        // created at its address, we would still be holding a dangling pointer
        // and calling GetDesc/CopyTextureRegion on it. One AddRef removes that
        // window entirely for the single resource we actually read from. The
        // cost is one full-res depth-stencil kept alive (~5 MB); pinning every
        // tracked target instead would pin hundreds and cost gigabytes.
        //
        // Safe to AddRef here: the winner was bound or cleared during this
        // frame, so the game held a reference to it moments ago on this thread.
        if (pinnedTarget_) {
            pinnedTarget_->Release();
            pinnedTarget_ = nullptr;
        }
        electedDesc_ = {};
        if (winner) {
            winner->AddRef();
            pinnedTarget_ = winner;
            const D3D12_RESOURCE_DESC d = winner->GetDesc();
            electedDesc_ = d;
            LogInfo("elected target PINNED %p %llux%u fmt=%s samples=%u",
                    static_cast<void*>(winner), d.Width, d.Height,
                    FormatName(d.Format), d.SampleDesc.Count);
        }
    }
    electedTarget_ = winner;

    // ---- id-buffer probe -----------------------------------------------------
    //
    // Its own gate, deliberately NOT nested under the CustomDepth election.
    // This is a different question about a different buffer, and on a title
    // where no CustomDepth candidate is ever elected -- which is exactly the
    // situation that makes this route interesting -- nesting it would mean the
    // probe silently never runs.
    // SIXTH time the shutter would have fired at the wrong moment, and I wrote
    // this one while reading the comment listing the previous five.
    //
    // The probe's entire budget was spent at t=22.8s, in the main menu, before a
    // single primitive had been marked. With no leased slots the channel test
    // has nothing to compare against and silently does nothing, and the budget
    // was gone for the rest of the process.
    //
    // The probe's only question is "does this buffer contain the slots WE
    // leased", so it is meaningless until we have leased some. Gate on that
    // rather than on a frame count -- the same fix as dumping on CONTENT rather
    // than on frame number, applied to the buffer that identifies the content.
    const bool haveMarksToTestAgainst = GetMarker().markedCount() > 0;
    if (probeIdBuffer_ && device_ && queue_ && haveMarksToTestAgainst && !idChannelFound_ &&
        !loggedIdExhausted_) {
        // ENUMERATE, do not guess.
        //
        // This used to take the largest-area integer target with a strict `>`,
        // pick it once, and never reconsider. On inZOI that always resolved to a
        // 1280x800 R32_UINT, which is why the "integer route is closed" verdict
        // was recorded -- while a 1280x800 R16G16_UINT sat in the same census,
        // observed 1,397 times, never once read.
        //
        // That R16G16_UINT matters: on UE 5.6 / PC D3D12 the Nanite CustomDepth
        // export takes the pixel-shader path (UseComputeDepthExport() requires
        // console-only GRHISupportsExplicitHTile), which writes depth to
        // CombinedCustomDepth and the STENCIL VALUE to a separate
        // CombinedCustomStencil of format PF_R16G16_UINT. So on a Nanite title
        // the per-object ids are in a COLOUR target, and the depth-stencil we
        // were electing never has its stencil plane written at all.
        //
        // Rather than swapping one hardcoded guess for another, collect every
        // scene-scale integer candidate and step through them: a candidate that
        // fails to contain any slot we leased is abandoned for the next one.
        // The leased-slot test is the same one that now guards the mask path, so
        // "which buffer holds our ids" is answered by measurement.
        if (!idTarget_) {
            // Rebuilt every time we need a candidate, NOT once. The Nanite
            // CombinedCustomStencil does not exist during the menu -- it is
            // allocated when something actually requests the CustomDepth pass --
            // so a list built at startup cannot contain the one buffer this
            // probe exists to find. Targets already rejected stay rejected via
            // idRejected_.
            idCandidates_.clear();
            for (const TargetFingerprint& t : snapshot) {
                if (!IsIntegerFormat(t.format)) continue;
                // Scene-scale, not thumbnail. Nanite's culling and hierarchy
                // buffers are integer targets too, and are 32x32 to 224x128.
                if (backbufferWidth_ && t.width < backbufferWidth_ / 4) continue;
                if (idRejected_.count(t.resource)) continue;
                idCandidates_.push_back(t.resource);
            }
            // Deterministic order so a re-run probes the same sequence, and so
            // "candidate 2 of 5" in the log means the same thing twice.
            std::sort(idCandidates_.begin(), idCandidates_.end());
            idCandidateIndex_ = 0;
            if (idCandidateIndex_ < idCandidates_.size()) {
                idTarget_ = idCandidates_[idCandidateIndex_];
                D3D12_RESOURCE_DESC d = idTarget_->GetDesc();
                LogInfo("idbuf: probing candidate %zu of %zu: %p %llux%u %s",
                        idCandidateIndex_ + 1, idCandidates_.size(),
                        static_cast<void*>(idTarget_), d.Width, d.Height,
                        FormatName(d.Format));
            } else if (!idRejected_.empty()) {
                loggedIdExhausted_ = true;
                LogWarn("idbuf: every scene-scale integer target present has been tested "
                        "(%zu rejected); none contained a stencil slot we leased. The "
                        "per-object ids are not in an integer render target on this title.",
                        idRejected_.size());
            }
        }
        // NEVER copy from a resource whose state we have not observed. This is
        // the same rule the mask path follows, and the probe was exempt from it
        // by omission rather than by argument -- which killed inZOI twice: once
        // on the first armed copy, and once at t=44.9s on the probe's very first
        // candidate. StateOf() returns COMMON for an unseen resource, and
        // declaring COMMON for something the game is actually using as a render
        // target is an invalid transition, i.e. a GPU fault.
        //
        // Waiting costs nothing: a render target the game actually uses gets
        // transitioned within a frame or two, so a candidate that never does is
        // one we could not have copied safely anyway.
        if (idTarget_) {
            const uint64_t candidateBarriers = BarriersSeenFor(idTarget_);
            if (candidateBarriers == 0) {
                if (++idBarrierWait_ > kIdBarrierWaitFrames) {
                    LogInfo("idbuf: candidate %p never transitioned in %u frames, so its "
                            "state is unknown and copying it is unsafe; skipping it",
                            static_cast<void*>(idTarget_), kIdBarrierWaitFrames);
                    idRejected_.insert(idTarget_);
                    idTarget_ = nullptr;
                    idBarrierWait_ = 0;
                }
            } else {
                idBarrierWait_ = 0;
                if (idRing_.Prepare(device_, idTarget_, 0 /*colour plane*/)) {
                    idRing_.Enqueue(queue_, idTarget_, StateOf(idTarget_), frameIndex_);
                }
            }
        }
        idRing_.Drain([this](const MaskFrame& f) { OnIdBufferReady(f); });
    }

    // Census-only mode issues no GPU work at all: no copy, no barriers, nothing
    // submitted on the game's queue. Used for first contact with an unfamiliar
    // title, where a wrong shadowed state would show up as a GPU hang rather
    // than an error message. Observe first, then act.
    // A separate switch from census mode, and the distinction is the whole
    // point of it.
    //
    // Census suppresses the GPU work AND the engine work, so a census run
    // proves nothing about which of the two kills a game. inZOI died 0.5-1.5s
    // after RECORDING STARTED in four consecutive marking runs (110.59/~111,
    // 109.52/~110.3, 110.00/~110.5, 44.63/~45.6) while census runs survived the
    // identical load -- but that comparison changes two variables at once.
    //
    // `-Captures 0` was the first attempt at isolating it and was not an
    // isolation at all: it zeroes the DUMP budget, which is CPU-side work on
    // already-read-back data, while the copy below still runs every frame. The
    // run died at 45.56s against the previous run's 45.6s, i.e. the independent
    // variable never moved.
    //
    // This suppresses exactly one thing: the copy, its two barriers, and the
    // submission on the game's queue. Marking, reflection and ProcessEvent all
    // stay live. Survive with this set and the readback is the culprit; die
    // anyway and it is not.
    if (noReadback_ && electedTarget_ && !warnedNoReadback_) {
        warnedNoReadback_ = true;
        LogWarn("readback SUPPRESSED by marker -- marking stays live, no GPU work "
                "is issued. Isolation run: this is the only variable.");
    }
    // Refuse to transition a resource whose state we have never watched change.
    //
    // Without this the readback declares StateBefore from StateOf(), which
    // answers COMMON for anything it has not seen and a possibly-stale value
    // for anything it has. Declaring a wrong StateBefore is not a soft error:
    // the runtime took it as a promise, and inZOI died 0.5-1.5s later every
    // time. Refusing costs a title we could not have captured correctly anyway,
    // and it says why instead of taking the process down.
    // Runtime arming.
    //
    // Every other marker is read once, at injection. This one cannot be: the
    // whole point is to open the shutter at a moment only a human watching the
    // screen can identify -- "the save has loaded and I am standing in the
    // world". The readback carries a fuse on inZOI (a cross-queue race that
    // kills the process within seconds), so spending it on menu frames wastes
    // the only window we get. The previous attempt burned all 24 masks between
    // t=22s and t=45s and never reached gameplay at all.
    //
    // Polled, not watched, and only while disarmed -- an existence check every
    // 60 frames costs nothing and stops entirely once armed.
    if (requireArm_ && !armed_ && (frameIndex_ % 60) == 0) {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
        std::wstring p(self);
        const size_t dot = p.find_last_of(L'.');
        if (dot != std::wstring::npos) p = p.substr(0, dot);
        p += L".arm";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            armed_ = true;
            LogWarn("CAPTURE ARMED at frame %llu -- readback begins now", frameIndex_);
        }
    }

    // Two independent reasons the readback may not run, reported separately.
    //
    // The first version folded both into one boolean and printed the barrier
    // message for either -- so a run that was merely waiting to be armed
    // reported "we have observed ZERO ResourceBarrier transitions", and I read
    // that as evidence for Enhanced Barriers. It was evidence of nothing. A
    // diagnostic that cannot tell two causes apart will always name the one you
    // were already expecting.
    //
    // Also no longer one-shot: the elected target CHANGES during a session
    // (menu target, then gameplay target), and a latched warning describes
    // whichever came first forever. It reports per target instead, which is the
    // thing the question is actually about.
    const bool armReady = !requireArm_ || armed_;
    const uint64_t barriers = electedTarget_ ? BarriersSeenFor(electedTarget_) : 0;
    const bool shadowUsable = electedTarget_ && barriers > 0;

    if (electedTarget_ && !censusOnly_ && !noReadback_ && armReady && !shadowUsable
        && electedTarget_ != lastUnshadowedTarget_) {
        lastUnshadowedTarget_ = electedTarget_;
        // The Enhanced Barriers explanation this message used to carry was
        // measured and killed: inZOI calls ID3D12GraphicsCommandList7::Barrier
        // zero times while a legacy target showed 7,340 transitions. The real
        // cause found afterwards was address recycling -- we were scoring a
        // resource that had already been destroyed, so of course nothing had
        // ever been observed transitioning the thing now at that address.
        LogError("readback REFUSED for target %p (%llux%u %s): ARMED and elected, but ZERO "
                 "ResourceBarrier transitions observed for it, so any StateBefore we "
                 "declare would be invented. %llu address recycles seen so far. Marking "
                 "is unaffected.",
                 static_cast<void*>(electedTarget_), electedDesc_.Width, electedDesc_.Height,
                 FormatName(electedDesc_.Format), addressRecycles_);
    }

    // Say plainly when we are simply waiting, so silence is never ambiguous.
    if (requireArm_ && !armed_ && electedTarget_ && (frameIndex_ % 600) == 0) {
        int layout = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = textureLayout_.find(electedTarget_);
            if (it != textureLayout_.end()) layout = static_cast<int>(it->second);
        }
        // The number that settles the Enhanced Barriers question, printed
        // rather than inferred from an absence.
        LogInfo("readback holding: DISARMED (target %p, %llu barriers observed, "
                "enhanced hooked=%d total enhanced=%llu, layout=%d)",
                static_cast<void*>(electedTarget_), barriers,
                enhancedBarriersHooked_ ? 1 : 0, enhancedBarrierCount_, layout);
    }

    if (armReady && electedTarget_ && shadowUsable && !loggedArmedOk_) {
        loggedArmedOk_ = true;
        LogInfo("readback ENABLED for target %p (%llu barriers observed)",
                static_cast<void*>(electedTarget_), barriers);
    }

    // Once the probe has PROVEN which buffer carries our slots, read that,
    // regardless of what the depth-stencil election concluded. On a Nanite title
    // the two are different resources and only one of them has ever contained an
    // id: measured 100.0% of non-zero texels in the R16G16_UINT's G channel
    // against 0.0% everywhere else.
    //
    ID3D12Resource* const copySource = maskSource_ ? maskSource_ : electedTarget_;
    const UINT copyPlane = maskSource_ ? 0u : 1u;   // colour target vs stencil plane

    // The barrier guard applies to WHATEVER we copy, not just to the elected
    // depth-stencil.
    //
    // The first version of this exempted maskSource_ on the reasoning that "the
    // copy already succeeded during probing, so the state is fine". It is not the
    // same situation: the probe copies while disarmed, at a different point in
    // the frame's state history, and a resource can sit in a different state by
    // the time capture begins. inZOI died on the very first copy after CAPTURE
    // ARMED -- the exact failure this guard was written to prevent, re-enabled by
    // an argument rather than by evidence.
    //
    // If we have never seen a transition for the source, we do not know its
    // state, and declaring one is how a StateBefore becomes a lie.
    const uint64_t sourceBarriers = copySource ? BarriersSeenFor(copySource) : 0;
    const bool sourceShadowUsable = copySource && sourceBarriers > 0;
    const bool canCopy = sourceShadowUsable && armReady;

    if (maskSource_ && armReady && !sourceShadowUsable && !warnedMaskSourceUnshadowed_) {
        warnedMaskSourceUnshadowed_ = true;
        LogError("capture: id buffer %p carries our ids but ZERO barriers have been observed "
                 "for it, so its state is unknown and copying would risk a GPU fault. "
                 "Refusing.",
                 static_cast<void*>(maskSource_));
    }

    if (!censusOnly_ && !noReadback_ && canCopy && copySource && device_ && queue_) {
        if (readback_.Prepare(device_, copySource, copyPlane)) {
            readback_.Enqueue(queue_, copySource, StateOf(copySource), frameIndex_);

            // Remember which slot table was live at submission. Copying a
            // shared_ptr costs nothing per frame; the table itself is only
            // rebuilt when the marking thread actually changes it.
            if (auto sc = GetMarker().publishedSidecar()) {
                sidecarHistory_.emplace_back(frameIndex_, std::move(sc));
                // The readback ring is 3 deep, so nothing older than a few
                // frames can still be in flight.
                while (sidecarHistory_.size() > 16) sidecarHistory_.pop_front();
            }
            // Stamped HERE, at Present, not when the mask lands. The action
            // that produced a frame is the one in effect while it was being
            // rendered; a timestamp taken when the readback completes is
            // several frames late and would shift every action label.
            frameStamps_.emplace_back(frameIndex_, NowMs());
            while (frameStamps_.size() > 64) frameStamps_.pop_front();
        }

        // Colour backbuffer, same frame, same index. Capturing both here is the
        // whole reason the streams cannot drift: there is no separate video
        // recorder with its own clock to reconcile afterwards.
        if (swapChain) {
            ID3D12Resource* back = nullptr;
            const UINT idx = swapChain->GetCurrentBackBufferIndex();
            if (SUCCEEDED(swapChain->GetBuffer(idx, IID_PPV_ARGS(&back))) && back) {
                if (colourRing_.Prepare(device_, back, 0 /*colour, not a plane*/)) {
                    // At Present time the game has transitioned the backbuffer to
                    // PRESENT (0). Use the shadowed state rather than assuming.
                    colourRing_.Enqueue(queue_, back, StateOf(back), frameIndex_);
                }
                back->Release();
            }
        }
        colourRing_.Drain([this](const MaskFrame& f) { OnColourReady(f); });
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
    LogInfo("    recording=%s  captured=%llu  skipped(menu/empty)=%llu",
            recording_ ? "YES" : "no", recordedFrames_, skippedFrames_);

    for (const TargetFingerprint& t : snapshot) {
        // Only depth-stencil targets matter for the mask route; logging every
        // colour target would bury them.
        //
        // "Ever bound as depth" is included alongside the format test on
        // purpose. Filtering on format alone means a depth buffer in a format
        // this build does not recognise is invisible here -- and "invisible"
        // and "absent" look identical in a log, which is exactly the confusion
        // that cost a run. If something was bound to the depth slot, it gets
        // printed whatever its format, and an unexpected format shows up as
        // "other" rather than as silence.
        if (!HasStencilPlane(t.format) && !t.everBoundAsDepth) continue;
        LogInfo("  DS %p %-22s %llux%u samples=%u binds=%u clears=%u sclears=%u depth=%s "
                "state=0x%X",
                static_cast<void*>(t.resource), FormatName(t.format), t.width, t.height,
                t.sampleCount, t.bindCount, t.clearCount, t.stencilClearCount,
                t.everBoundAsDepth ? "yes" : "NO", StateOf(t.resource));
    }

    // Integer render targets, listed separately with their dimensions.
    //
    // These are the OTHER route to per-object identity. UE writes ids into
    // integer targets, and UE5's Nanite visibility buffer is an R32G32_UINT at
    // scene resolution packing depth with a visible-cluster index. Reading one
    // needs no engine mutation at all -- which matters on a title where the
    // CustomDepth route is blocked because ProcessEvent cannot be found safely.
    //
    // Listed here rather than inferred from the election's rejection lines,
    // which say only "no stencil plane" and drop the dimensions. Knowing an
    // R32G32_UINT exists is useless; knowing whether it is scene-sized is the
    // entire question.
    for (const TargetFingerprint& t : snapshot) {
        if (!IsIntegerFormat(t.format)) continue;
        const bool sceneSized = backbufferWidth_ != 0 && t.width >= backbufferWidth_ / 4;
        LogInfo("  ID? %p %-22s %llux%u binds=%u clears=%u%s",
                static_cast<void*>(t.resource), FormatName(t.format), t.width, t.height,
                t.bindCount, t.clearCount,
                sceneSized ? "   <-- scene-scale integer target" : "");
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
