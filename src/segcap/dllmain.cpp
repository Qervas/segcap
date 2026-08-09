// dllmain.cpp -- entry point for the injected capture DLL.
//
// The one rule that matters here: DllMain runs under the loader lock. Creating
// a D3D12 device, resolving vtables, or calling into MinHook from DllMain can
// deadlock the host process -- and a deadlock inside someone else's game looks
// like a hang with no error, which is the worst possible failure mode to debug.
//
// So DllMain does nothing but spawn a thread and return.

#include <windows.h>

#include "customdepth.h"
#include "hooks.h"
#include "log.h"
#include "ue4.h"

namespace {

HMODULE g_self = nullptr;

// Set from a "segcap.mark" marker file next to the DLL. Writing to the game is
// opt-in per run: every session so far has been read-only, and that must stay
// the default rather than something that happens because a flag was left on.
bool g_markCustomDepth = false;

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

        std::wstring mk(marker);
        const size_t dot2 = mk.find_last_of(L'.');
        if (dot2 != std::wstring::npos) mk = mk.substr(0, dot2);
        mk += L".mark";
        g_markCustomDepth = GetFileAttributesW(mk.c_str()) != INVALID_FILE_ATTRIBUTES;
        segcap::LogInfo("mark marker %ls: %s", mk.c_str(),
                        g_markCustomDepth ? "PRESENT -- WILL MUTATE GAME STATE"
                                          : "absent (read-only run)");

        // A/B mode. Captures colour frames on a stride from the moment the
        // elected target is live, irrespective of mask content, so the
        // marking-off condition yields frames to compare against.
        std::wstring ab(marker);
        const size_t dot3 = ab.find_last_of(L'.');
        if (dot3 != std::wstring::npos) ab = ab.substr(0, dot3);
        ab += L".abtest";
        const bool abtest = GetFileAttributesW(ab.c_str()) != INVALID_FILE_ATTRIBUTES;
        segcap::Hooks::Get().SetAbTest(abtest);
        if (abtest) segcap::LogInfo("A/B MODE: colour frames captured unconditionally");
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
segcap::CustomDepthMarker g_marker;

}  // namespace

// Accessor so hooks.cpp can snapshot the slot table when dumping a mask,
// without either file including the other's translation unit.
namespace segcap { CustomDepthMarker& GetMarker() { return g_marker; } }

namespace {

// Defined below; started from DiscoverThread as soon as ProcessEvent is proven.
DWORD WINAPI MarkLoopThread(LPVOID);

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
            // Probe the actual layout before trusting any assumed offset.
            g_engine.DumpStructLayout(primClass, "UPrimitiveComponent (via FindClass)");

            // The layout probe showed SuperStruct and PropertiesSize exactly
            // where UE 4.25+ puts them, but Children AND ChildProperties both
            // null -- impossible for a native class with dozens of UPROPERTYs.
            //
            // Hypothesis: FindClass returned a different UClass object than the
            // one live instances actually point to. Test it by taking the class
            // straight off a real StaticMeshComponent and comparing. If the
            // pointers differ, the name lookup is finding a stub.
            void* instance = nullptr;
            void* instanceClass = nullptr;
            for (int32_t i = 0; i < g_engine.NumObjects(); ++i) {
                segcap::ue4::ObjectRef ref;
                if (!g_engine.GetObject(i, ref)) continue;
                if (ref.className != "StaticMeshComponent") continue;
                instance = ref.object;
                instanceClass = *reinterpret_cast<void**>(
                    reinterpret_cast<uint8_t*>(ref.object) +
                    segcap::ue4::UObjectLayout::kClassPrivate);
                break;
            }
            segcap::LogInfo("ue4: sample StaticMeshComponent instance=%p class=%p",
                            instance, instanceClass);
            if (instanceClass) {
                g_engine.DumpStructLayout(instanceClass, "UStaticMeshComponent (via instance)");
            }

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
                if (!info.valid()) {
                    segcap::LogWarn("ue4:  MISSING %-28s -- will not write blind", want);
                    continue;
                }
                if (info.isPackedBit()) {
                    // Report the exact byte and bit. Writing the whole byte here
                    // would clobber the other bools sharing it.
                    segcap::LogInfo("ue4:  FOUND %-28s [%s] byte +0x%X mask 0x%02X"
                                    "  <- PACKED BIT, read-modify-write only",
                                    want, info.type.c_str(),
                                    info.offset + info.byteOffset, info.fieldMask);
                } else {
                    segcap::LogInfo("ue4:  FOUND %-28s [%s] at +0x%X (size %d)", want,
                                    info.type.c_str(), info.offset, info.size);
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

            // ---- task 8: opt primitives into CustomDepth --------------------
            //
            // Gated behind a marker file, deliberately. This is the only code in
            // the project that mutates the game, and it must never happen by
            // accident on a run intended as read-only.
            if (g_markCustomDepth) {
                segcap::LogInfo("customdepth: MARK MODE ENABLED");
                // Resolve on the game thread, then collect off it.
                pe.RunOnGameThread([](segcap::ue4::Engine& e) { g_marker.Resolve(e); });
                g_marker.CollectCandidates(g_engine, true);

                // Start marking NOW, on its own thread, rather than after the
                // sampling schedule.
                //
                // Marking used to run only after the staged samples finished at
                // t+180s. On a 240-second run that left ~40 seconds of marking,
                // and because captures are taken every few frames, all 21 of
                // them landed inside a 1.6-second window at t+201 -- before the
                // working set had filled. Every mask in that run contained
                // exactly one object, which reads as "CustomDepth barely works"
                // when the truth is "we photographed the first two seconds of a
                // process that needed twenty".
                //
                // Sampling is diagnostics; marking is the product. The product
                // should not wait on the diagnostics.
                CreateThread(nullptr, 0, MarkLoopThread, nullptr, 0, nullptr);
            }
        }
    }

    // ---- keep marking as the level streams in ------------------------------
    // A single marking pass at t+41s caught only 267 primitives, because the
    // level was still loading -- the object array grows from ~173k to ~350k
    // slots over the first couple of minutes. Re-marking picks up everything
    // that arrives later, which is also what a real capture session needs:
    // objects stream in and out constantly.
    const int kIntervals[] = {60, 90, 120, 150, 180};
    int elapsed = 30;
    for (int stage : kIntervals) {
        if (stage > elapsed) Sleep((stage - elapsed) * 1000);
        elapsed = stage;
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "t+%ds", stage);
        g_engine.ReportSample(label);

        if (g_markCustomDepth && segcap::ue4::GetProcessEventHook().verified()) {
            // Collect here, on the discovery thread: it walks ~350,000 slots and
            // is read-only, so it must not run inside ProcessEvent.
            g_marker.CollectCandidates(g_engine, true);
        }
    }
    if (g_engine.namesResolved()) g_engine.CountDerivedFrom("PrimitiveComponent");
    return 0;
}

// Continuous, visibility-tracked marking. Runs on its own thread from the
// moment ProcessEvent is verified until the process exits.
//
// Two operations per tick:
//
//   RefreshVisibility  re-test primitives that hold slots and hand back the
//                      ones the renderer has stopped drawing
//   MarkBatch          sweep the candidate pool and lease the freed slots to
//                      primitives that are currently being drawn
//
// Together they make the 255-slot working set follow the camera, which is the
// only way a fixed-width id channel can describe a level with tens of thousands
// of primitives. Release runs BEFORE acquire: with a full working set the sweep
// would otherwise find no free slots and could never react to the camera moving.
//
// Both are bounded per tick because every object tested costs a UFunction call
// inside the engine's own dispatch. The budgets here are ~1,700 visibility tests
// per second, a few milliseconds of game-thread time.
DWORD WINAPI MarkLoopThread(LPVOID) {
    segcap::LogInfo("customdepth: continuous visibility-tracked marking started");
    int tick = 0;
    for (;;) {
        Sleep(250);
        ++tick;

        auto& pe = segcap::ue4::GetProcessEventHook();
        if (!pe.verified() || !g_marker.ready()) continue;

        pe.RunOnGameThread([](segcap::ue4::Engine& e) { g_marker.RefreshVisibility(e, 120); });
        pe.RunOnGameThread([](segcap::ue4::Engine& e) { g_marker.MarkBatch(e, 300); });

        // Re-walk the object array periodically. Level streaming adds and
        // removes primitives throughout a session, so a pool collected once
        // slowly stops describing the level actually being played.
        if (tick % 120 == 0) {   // every ~30s
            g_marker.CollectCandidates(g_engine, true);
        }
    }
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
