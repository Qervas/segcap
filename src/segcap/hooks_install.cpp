#include "hooks.h"

#include <MinHook.h>

#include <vector>

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

// ID3D12GraphicsCommandList4 begins at 68 on the same arithmetic that anchors
// kEnhancedBarrierSlot: GCL4 occupies 68..76 (nine methods, BeginRenderPass and
// EndRenderPass first), GCL5 77..78, GCL6 79, GCL7 80. That the chain lands
// exactly on the Barrier slot already proven in this file is the check.
constexpr int kBeginRenderPassSlot = 68;
constexpr int kEndRenderPassSlot = 69;
constexpr int kOMSetRenderTargetsSlot = 46;  // ID3D12GraphicsCommandList::OMSetRenderTargets
constexpr int kClearDSVSlot = 47;            // ID3D12GraphicsCommandList::ClearDepthStencilView
void** VTableOf(void* obj) { return *reinterpret_cast<void***>(obj); }
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

        // Render-pass scope, installed ONLY when foreign-list injection is
        // enabled. CopyTextureRegion recorded between BeginRenderPass and
        // EndRenderPass makes the runtime remove the command list, while
        // ResourceBarrier is legal there -- so without this the injection point
        // can be somewhere the barrier is fine and the copy is fatal.
        //
        // Gated so a title that never asks for injection (Stray) gains no extra
        // vtable patches and no new code on any hot path at all.
        // Not in dry-run: the guard exists to stop a COPY landing inside a render
        // pass, and a dry run records no copy. Installing two extra detours on
        // one of the engine's hottest paths to protect against something that
        // cannot happen is exactly the kind of cost that should not exist -- and
        // it doubles as the bisection that says whether these hooks are what
        // destabilised the game.
        if (foreignInject_ && !injectDryRun_) {
            ID3D12GraphicsCommandList4* gcl4 = nullptr;
            if (SUCCEEDED(dummyList->QueryInterface(IID_PPV_ARGS(&gcl4))) && gcl4) {
                auto** vt4 = *reinterpret_cast<void***>(gcl4);
                // Same anchor discipline as above: both interfaces are the same
                // object and must share a vtable prefix. Not skipped just
                // because slot 80 already worked -- GCL4 is a different QI.
                const bool anchorOk = vt4[kResourceBarrierSlot] == listVT[kResourceBarrierSlot];
                if (!anchorOk) {
                    LogError("GraphicsCommandList4 slot %d disagrees with the base list -- "
                             "refusing to hook render-pass slots on unconfirmed numbering",
                             kResourceBarrierSlot);
                } else if (CreateHook(vt4, kBeginRenderPassSlot,
                                      reinterpret_cast<void*>(&Hooks::BeginRenderPass_),
                                      reinterpret_cast<void**>(&origBeginRenderPass_)) &&
                           CreateHook(vt4, kEndRenderPassSlot,
                                      reinterpret_cast<void*>(&Hooks::EndRenderPass_),
                                      reinterpret_cast<void**>(&origEndRenderPass_))) {
                    renderPassHooked_ = true;
                    LogInfo("render-pass scope hooked (slots %d/%d) -- copy injection will "
                            "refuse inside a render pass",
                            kBeginRenderPassSlot, kEndRenderPassSlot);
                } else {
                    LogError("failed to hook BeginRenderPass/EndRenderPass; injection will "
                             "stay disabled because a copy inside a render pass removes the "
                             "command list");
                }
                gcl4->Release();
            } else {
                LogWarn("ID3D12GraphicsCommandList4 not available -- no render passes exist "
                        "on this runtime, so injection cannot hit that hazard");
                renderPassHooked_ = true;   // hazard cannot occur
            }
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

}  // namespace segcap
