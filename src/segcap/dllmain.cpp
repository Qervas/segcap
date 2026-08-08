// dllmain.cpp -- entry point for the injected capture DLL.
//
// The one rule that matters here: DllMain runs under the loader lock. Creating
// a D3D12 device, resolving vtables, or calling into MinHook from DllMain can
// deadlock the host process -- and a deadlock inside someone else's game looks
// like a hang with no error, which is the worst possible failure mode to debug.
//
// So DllMain does nothing but spawn a thread and return.

#include <windows.h>

#include "hooks.h"
#include "log.h"
#include "ue4.h"

namespace {

HMODULE g_self = nullptr;

// Signalled once hooks are live. The injector creates this event before
// injecting and waits on it before resuming a suspended process.
//
// Without it, suspended launch is useless: LoadLibraryW returns as soon as
// DllMain returns, and DllMain only spawns this thread. The injector would
// resume the game while we are still building dummy devices, and every
// descriptor created in that window is lost permanently. Measured against the
// test fixture, that window was ~300ms and cost us every render target.
void SignalReady() {
    wchar_t name[64];
    _snwprintf_s(name, _TRUNCATE, L"Local\\segcap_hooks_ready_%lu", GetCurrentProcessId());
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!ev) return;  // no injector waiting; attaching to a running process
    SetEvent(ev);
    CloseHandle(ev);
}

DWORD WINAPI InitThread(LPVOID) {
    segcap::Log::Get().Init(g_self);
    segcap::LogInfo("segcap attached to pid %lu", GetCurrentProcessId());

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    segcap::LogInfo("host: %ls", exe);

    // Census-only is selected by dropping a marker file next to the DLL. An
    // environment variable would not work: we inherit the host's environment,
    // which Steam fixed when it created the process.
    wchar_t marker[MAX_PATH] = {};
    if (GetModuleFileNameW(g_self, marker, MAX_PATH)) {
        std::wstring m(marker);
        const size_t dot = m.find_last_of(L'.');
        if (dot != std::wstring::npos) m = m.substr(0, dot);
        m += L".census";
        const bool census = GetFileAttributesW(m.c_str()) != INVALID_FILE_ATTRIBUTES;
        segcap::Hooks::Get().SetCensusOnly(census);
        segcap::LogInfo("census marker %ls: %s", m.c_str(), census ? "PRESENT" : "absent");
    }

    // The host may not have created its device yet when we are injected at
    // startup. Retry rather than give up: a single failed attempt at t=0 would
    // make injection order silently matter.
    for (int attempt = 1; attempt <= 30; ++attempt) {
        if (segcap::Hooks::Get().Install()) {
            segcap::LogInfo("hook installation succeeded on attempt %d", attempt);
            SignalReady();
            return 0;
        }
        segcap::LogWarn("hook installation attempt %d failed; retrying in 1s", attempt);
        Sleep(1000);
    }

    segcap::LogError("giving up after 30 attempts");
    SignalReady();
    return 1;
}

// Engine discovery runs on its own thread, well after startup.
//
// Two reasons it cannot share the hook thread: the scan walks the entire module
// image looking for a structure by shape, which takes long enough that it must
// not delay hook installation; and GUObjectArray is not populated at process
// start, so an early scan would correctly find nothing and we would wrongly
// conclude the technique does not work.
// The discovered engine. Static, NOT a temporary.
//
// An earlier version wrote `segcap::ue4::Engine().Discover()`, which constructs
// a temporary, discovers the addresses into it, and destroys it on the next
// semicolon. Discovery "succeeded" and every address was thrown away.
segcap::ue4::Engine g_engine;

DWORD WINAPI DiscoverThread(LPVOID) {
    for (int attempt = 1; attempt <= 40; ++attempt) {
        Sleep(2000);
        segcap::LogInfo("ue4: discovery attempt %d", attempt);
        if (g_engine.Discover()) {
            segcap::LogInfo("ue4: discovery succeeded on attempt %d", attempt);
            break;
        }
        if (attempt == 40) {
            segcap::LogError("ue4: discovery failed after 40 attempts");
            return 1;
        }
    }

    // ---- reflection FIRST -------------------------------------------------
    //
    // Ordering matters more than it looks. An earlier version ran the periodic
    // sampling and a full 325k-object CountDerivedFrom walk BEFORE this, putting
    // the most important results at the end of a 2.5 minute pipeline. A machine
    // crash at t+92s lost the entire run and produced nothing.
    //
    // Reflection needs only discovery and name resolution, both of which land at
    // t=2s, and it is pure memory reading -- it works fine on the main menu.
    // So it goes first, and the slow diagnostics come after.
    if (g_engine.namesResolved()) {
        void* primClass = g_engine.FindClass("PrimitiveComponent");
        segcap::LogInfo("ue4: UPrimitiveComponent UClass = %p", primClass);

        if (primClass) {
            const auto props = g_engine.ListProperties(primClass, false);
            segcap::LogInfo("ue4: %zu properties declared directly on UPrimitiveComponent",
                            props.size());
            int shown = 0;
            for (const auto& p : props) {
                if (shown++ >= 30) break;
                segcap::LogInfo("ue4:     +0x%-5X size=%-5d %s", p.offset, p.size,
                                p.name.c_str());
            }

            // The ones we actually need. Logged as MISSING rather than guessed:
            // this is the last read before we start writing to a live game.
            for (const char* want : {"bRenderCustomDepth", "CustomDepthStencilValue",
                                     "bVisible", "CustomDepthStencilWriteMask"}) {
                const auto info = g_engine.FindProperty(primClass, want);
                if (info.valid()) {
                    segcap::LogInfo("ue4:  FOUND %-28s at +0x%X (size %d)", want,
                                    info.offset, info.size);
                } else {
                    segcap::LogWarn("ue4:  MISSING %-28s -- will not write blind", want);
                }
            }
        }
    }

    // ---- ProcessEvent second ----------------------------------------------
    // Needs the engine actively dispatching, so give the game a moment to get
    // going, but do not wait for the full sampling schedule.
    if (g_engine.namesResolved()) {
        Sleep(25000);
        auto& pe = segcap::ue4::GetProcessEventHook();
        if (pe.Install(g_engine)) {
            segcap::LogInfo("ue4: game-thread execution point ready (vtable %d)",
                            pe.vtableIndex());
            // Prove the queue actually runs there, before anything depends on it.
            pe.RunOnGameThread([](segcap::ue4::Engine&) {
                segcap::LogInfo("ue4: >>> task executed on game thread %lu <<<",
                                GetCurrentThreadId());
            });
        }
    }

    // ---- slow diagnostics last --------------------------------------------
    // Everything below is useful context, not a gate on progress, so it runs
    // after the two results that matter.
    const int kIntervals[] = {60, 90, 120, 180};
    int elapsed = 30;
    for (int stage : kIntervals) {
        if (stage > elapsed) Sleep((stage - elapsed) * 1000);
        elapsed = stage;
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "t+%ds", stage);
        g_engine.ReportSample(label);
    }
    if (g_engine.namesResolved()) g_engine.CountDerivedFrom("PrimitiveComponent");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = module;
            DisableThreadLibraryCalls(module);
            HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            // Separate thread: the module-wide structural scan is slow and must
            // not delay hook installation, and the object array does not exist
            // yet at process start anyway.
            HANDLE d = CreateThread(nullptr, 0, DiscoverThread, nullptr, 0, nullptr);
            if (d) CloseHandle(d);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Deliberately not unhooking here. On process exit the host's
            // threads may still be inside our detours, and removing a trampoline
            // out from under a running thread crashes the game during shutdown --
            // which would look exactly like our capture corrupted something.
            break;
        default:
            break;
    }
    return TRUE;
}
