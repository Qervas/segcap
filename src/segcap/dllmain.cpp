// dllmain.cpp -- entry point for the injected capture DLL.
//
// The one rule that matters here: DllMain runs under the loader lock. Creating
// a D3D12 device, resolving vtables, or calling into MinHook from DllMain can
// deadlock the host process -- and a deadlock inside someone else's game looks
// like a hang with no error, which is the worst possible failure mode to debug.
//
// So DllMain does nothing but spawn a thread and return.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "customdepth.h"
#include "hooks.h"
#include "log.h"
#include "perception.h"
#include "ue4.h"

namespace {

HMODULE g_self = nullptr;

// Set from a "segcap.mark" marker file next to the DLL. Writing to the game is
// opt-in per run: every session so far has been read-only, and that must stay
// the default rather than something that happens because a flag was left on.
bool g_markCustomDepth = false;
bool g_peTriageInCensus = false;

// True while a marking pass is queued but not yet drained on the game thread.
std::atomic<bool> g_markPassPending{false};

// Read-only introspection dump, from a "segcap.introspect" marker file. Each
// non-empty line names a class whose reflected properties should be logged.
bool g_introspect = false;
std::vector<std::string> g_introspectClasses;

// Enumerates what is actually extractable from a title.
//
// Deliberately a measurement rather than a design document. On an unfamiliar
// game the useful question is not "what does UE expose" -- that is known -- but
// "which classes does THIS game instantiate, and what fields do they carry".
// Both are answerable by reflection in a few hundred milliseconds, and guessing
// either has already cost this project a run.
void Introspect(segcap::ue4::Engine& engine, const std::vector<std::string>& classes) {
    engine.RefreshMemoryMap();
    engine.CalibrateFieldLayout();

    std::unordered_map<std::string, int> histogram;
    const int32_t total = engine.NumObjects();
    for (int32_t i = 0; i < total; ++i) {
        segcap::ue4::ObjectRef ref;
        if (!engine.GetObject(i, ref)) continue;
        ++histogram[ref.className];
    }

    std::vector<std::pair<std::string, int>> sorted(histogram.begin(), histogram.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    segcap::LogInfo("introspect: %zu DISTINCT CLASSES across %d objects",
                    sorted.size(), total);
    const size_t cap = sorted.size() < 250 ? sorted.size() : 250;
    for (size_t i = 0; i < cap; ++i) {
        segcap::LogInfo("introspect:   %6d  %s", sorted[i].second, sorted[i].first.c_str());
    }

    for (const std::string& cls : classes) {
        void* uclass = engine.FindClass(cls.c_str());
        if (!uclass) {
            segcap::LogWarn("introspect: class '%s' not found", cls.c_str());
            continue;
        }
        const auto props = engine.ListProperties(uclass, true);
        segcap::LogInfo("introspect: --- %s : %zu reflected properties ---",
                        cls.c_str(), props.size());
        for (const auto& p : props) {
            segcap::LogInfo("introspect:   +0x%04X %-4d %-22s %s",
                            p.offset, p.size, p.type.c_str(), p.name.c_str());
        }

        // Zero properties means the property CHAIN offsets are wrong for this
        // engine build, not that the class has no fields. Dump the raw UStruct
        // so the correct offset can be read off rather than guessed.
        //
        // This is the situation DumpStructLayout was written for: it prints
        // each qword and tries to interpret it under BOTH the pre-4.25
        // (UProperty: a UObject with its FName at +0x18) and post-4.25
        // (FProperty: an FField with its FName at +0x28) layouts. Whichever
        // one decodes into readable property names is the layout this build
        // uses. Guessing the layout once already cost this project a day.
        if (props.empty()) {
            segcap::LogWarn("introspect: %s has 0 properties -- dumping raw UStruct "
                            "to locate the property chain on this engine version",
                            cls.c_str());
            engine.DumpStructLayout(uclass, cls.c_str());
            engine.ProbeFieldNameOffset(uclass, cls.c_str());
        }
    }
}

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

        // Opt-in: attempt ProcessEvent discovery even in census mode. Census is
        // read-only by contract and hooking is not a read, so this stays an
        // explicit switch rather than something census quietly started doing.
        std::wstring pt(marker);
        const size_t dotp = pt.find_last_of(L'.');
        if (dotp != std::wstring::npos) pt = pt.substr(0, dotp);
        pt += L".petriage";
        g_peTriageInCensus = GetFileAttributesW(pt.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (g_peTriageInCensus) {
            segcap::LogInfo("pe-triage marker %ls: PRESENT -- census will attempt "
                            "ProcessEvent discovery on the triage shortlist", pt.c_str());
        }

        std::wstring ar(marker);
        const size_t dota = ar.find_last_of(L'.');
        if (dota != std::wstring::npos) ar = ar.substr(0, dota);
        ar += L".requirearm";
        if (GetFileAttributesW(ar.c_str()) != INVALID_FILE_ATTRIBUTES) {
            segcap::Hooks::Get().SetRequireArm(true);
            segcap::LogInfo("require-arm marker PRESENT -- readback holds until "
                            "segcap.arm appears");
        }

        // Read before Install(), which is before the game's device exists.
        std::wstring dd(marker);
        const size_t dotd = dd.find_last_of(L'.');
        if (dotd != std::wstring::npos) dd = dd.substr(0, dotd);
        dd += L".d3ddebug";
        if (GetFileAttributesW(dd.c_str()) != INVALID_FILE_ATTRIBUTES) {
            segcap::Hooks::Get().SetD3DDebug(true);
            segcap::LogInfo("d3d-debug marker %ls: PRESENT -- validation layer on", dd.c_str());
        }

        std::wstring nr(marker);
        const size_t dotn = nr.find_last_of(L'.');
        if (dotn != std::wstring::npos) nr = nr.substr(0, dotn);
        nr += L".noreadback";
        if (GetFileAttributesW(nr.c_str()) != INVALID_FILE_ATTRIBUTES) {
            segcap::Hooks::Get().SetNoReadback(true);
            segcap::LogInfo("no-readback marker %ls: PRESENT -- marking runs, "
                            "no GPU work is issued", nr.c_str());
        }

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

        // Probe a scene-resolution integer target -- the mutation-free route to
        // per-pixel identity. Read-only: it copies a buffer the game already
        // writes and never touches UObject state.
        std::wstring idb(marker);
        const size_t dot5 = idb.find_last_of(L'.');
        if (dot5 != std::wstring::npos) idb = idb.substr(0, dot5);
        idb += L".idbuf";
        if (GetFileAttributesW(idb.c_str()) != INVALID_FILE_ATTRIBUTES) {
            segcap::Hooks::Get().SetProbeIdBuffer(true);
            segcap::LogInfo("ID-BUFFER PROBE enabled (read-only)");
        }

        // Optional capture profile. Each file holds a single integer.
        auto readInt = [&](const wchar_t* ext) -> unsigned long {
            std::wstring q(marker);
            const size_t d = q.find_last_of(L'.');
            if (d != std::wstring::npos) q = q.substr(0, d);
            q += ext;
            std::FILE* fp = nullptr;
            if (_wfopen_s(&fp, q.c_str(), L"r") != 0 || !fp) return 0;
            unsigned long v = 0;
            if (fwscanf_s(fp, L"%lu", &v) != 1) v = 0;
            std::fclose(fp);
            return v;
        };
        // Read-only introspection: each line of segcap.introspect names a class
        // whose reflected properties should be dumped.
        std::wstring intro(marker);
        const size_t dot4 = intro.find_last_of(L'.');
        if (dot4 != std::wstring::npos) intro = intro.substr(0, dot4);
        intro += L".introspect";
        std::FILE* ifp = nullptr;
        if (_wfopen_s(&ifp, intro.c_str(), L"r") == 0 && ifp) {
            g_introspect = true;
            char line[256];
            while (std::fgets(line, sizeof(line), ifp)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
                    s.pop_back();
                }
                if (!s.empty()) g_introspectClasses.push_back(s);
            }
            std::fclose(ifp);
            segcap::LogInfo("introspect marker present: %zu class(es) requested",
                            g_introspectClasses.size());
        }

        const unsigned long caps = readInt(L".captures");
        const unsigned long strd = readInt(L".stride");
        if (caps || strd) {
            segcap::Hooks::Get().SetCaptureProfile(static_cast<uint32_t>(caps),
                                                   static_cast<uint64_t>(strd));
            segcap::LogInfo("capture profile: max=%u stride=%llu",
                            segcap::Hooks::Get().maxCaptures(),
                            segcap::Hooks::Get().captureStride());
        }
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
            // Work out this build's FField layout immediately, before anything
            // reads a property. It must run on EVERY run, not just census ones:
            // it was originally called only from the introspection dump, which
            // meant a real capture run on UE5 would have gone on using the
            // UE4.25 offsets and found no properties at all -- the exact bug
            // the calibration exists to fix, still present on the path that
            // matters.
            g_engine.CalibrateFieldLayout();
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
    //
    // CENSUS MODE MUST NOT REACH HERE.
    //
    // Census was described, in this file and to its users, as a read-only mode
    // for first contact with an unfamiliar title. It suppressed GPU work and
    // UObject writes -- and not this, which is neither. Finding ProcessEvent
    // means installing a hook on UObject vtable slots 60..80 one at a time and
    // seeing which one behaves like ProcessEvent. On a familiar engine that is
    // a measured risk. On an unfamiliar one it is a loaded gun: the search
    // calls whatever occupies each slot using ProcessEvent's signature.
    //
    // It killed inZOI on the second census run, at candidate 70, and the log
    // shows why -- the validator was reporting 810 valid against 432,080
    // invalid, i.e. the slot was a hot function that is not ProcessEvent. UE5
    // does not have it where UE4 does.
    //
    // The mode was doing most of what it promised, which is exactly why nobody
    // noticed it was not doing all of it.
    // The id-buffer probe needs GPU reads (so it is not census mode) but must
    // NOT go looking for ProcessEvent. That search hooks arbitrary UObject
    // vtable slots and calls them, and on UE5 it killed inZOI outright -- which
    // is precisely the blocker the id-buffer route exists to go around. Running
    // it here would reintroduce the crash while trying to avoid needing it.
    //
    // UPDATE: both halves of that gun have since been disarmed -- the search
    // now tries only slots that read-only triage shortlisted (3, not 21), and
    // the validator stops after 2000 samples instead of running on every one of
    // UE5's ~300k calls per second. Census still defaults to skipping it,
    // because census means read-only and hooking is not a read; drop a
    // `segcap.petriage` marker to opt in.
    if (segcap::Hooks::Get().censusOnly() || segcap::Hooks::Get().probeIdBuffer()) {
        segcap::LogInfo("%s: skipping ProcessEvent discovery "
                        "(probing vtable slots is not a read-only act)",
                        segcap::Hooks::Get().censusOnly() ? "census mode"
                                                          : "id-buffer probe");
        // Still do everything that IS read-only -- sampling and introspection
        // are the whole point of a census run.
        if (g_engine.namesResolved()) {
            const int kSampleAt[] = {30, 60, 120, 180};
            int elapsed = 0;
            for (int stage : kSampleAt) {
                if (stage > elapsed) Sleep((stage - elapsed) * 1000);
                elapsed = stage;
                char label[32];
                _snprintf_s(label, sizeof(label), _TRUNCATE, "t+%ds", stage);
                g_engine.ReportSample(label);
            }
            g_engine.CountDerivedFrom("PrimitiveComponent");
            // No known index here -- this is the mode used on engines where the
            // brute-force search is unsafe, so the ranking is the output rather
            // than a calibration.
            g_engine.ProbeVTablePrologues(-1);
            if (g_introspect) Introspect(g_engine, g_introspectClasses);

            if (g_peTriageInCensus) {
                // Explicitly requested. Install() now tries only the handful of
                // slots the read-only triage shortlisted, and its validator
                // stops after 2000 samples -- the two things that made the
                // original sweep lethal here.
                segcap::LogInfo("pe-triage: attempting ProcessEvent discovery "
                                "(shortlist only, bounded validation)");
                auto& pe = segcap::ue4::GetProcessEventHook();
                if (pe.Install(g_engine)) {
                    segcap::LogInfo("pe-triage: ProcessEvent FOUND at vtable %d",
                                    pe.vtableIndex());
                    // Prove the queue actually runs on the game thread before
                    // anything is allowed to depend on it.
                    // Static, not a stack local. If the slot dispatches but the
                    // queue drains later than we wait, the task would otherwise
                    // write through a dangling reference into a dead frame --
                    // a use-after-free whose trigger is the hook working, just
                    // slowly.
                    static std::atomic<bool> ran{false};
                    pe.RunOnGameThread([](segcap::ue4::Engine&) {
                        ran.store(true, std::memory_order_release);
                    });
                    Sleep(2000);
                    segcap::LogInfo("pe-triage: game-thread task %s",
                                    ran.load(std::memory_order_acquire)
                                        ? "RAN -- execution point is live"
                                        : "did NOT run -- slot dispatches but the "
                                          "queue never drained");
                    Sleep(15000);   // let it idle under the hook before we judge it healthy
                    segcap::LogInfo("pe-triage: still alive 15s after install");
                } else {
                    segcap::LogWarn("pe-triage: no candidate validated as ProcessEvent");
                }
            }
        }
        segcap::LogInfo("census complete");
        return 0;
    }

    if (g_engine.namesResolved()) {
        Sleep(25000);
        auto& pe = segcap::ue4::GetProcessEventHook();
        if (pe.Install(g_engine)) {
            segcap::LogInfo("ue4: game-thread execution point ready (vtable %d)",
                            pe.vtableIndex());

            // Calibrate the read-only vtable heuristic against the answer the
            // brute-force search just proved. This is the whole point: the
            // heuristic is only worth running on an engine where the answer is
            // unknown if it can be shown to find the answer on one where it is.
            g_engine.ProbeVTablePrologues(pe.vtableIndex());
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

        // Candidate collection deliberately does NOT happen here any more.
        //
        // It used to run on this thread as well as on the marking thread, so
        // two threads walked the object array and rebuilt the shared readable-
        // memory map concurrently. The map is now published safely, but the
        // duplication was also just waste: a full walk of ~350,000 slots twice
        // over, to produce the same pool. The marking loop owns collection.
    }
    if (g_engine.namesResolved()) g_engine.CountDerivedFrom("PrimitiveComponent");

    // ---- optional read-only introspection dump -----------------------------
    //
    // Answers "what can we actually read out of this game" with evidence rather
    // than speculation. Two passes, both pure reads:
    //
    //   1. every distinct class name in the object array, with instance counts.
    //      On an unfamiliar title this is the map: it shows the engine classes,
    //      the game's own classes (inZOI prefixes everything B1, Stray uses
    //      Toyo), and which of them actually exist at runtime rather than in
    //      the headers.
    //   2. the full reflected property list for any class named in the marker
    //      file, so the fields available for extraction are enumerated instead
    //      of guessed.
    //
    // Gated behind segcap.introspect because it logs thousands of lines.
    if (g_engine.namesResolved() && g_introspect) {
        Introspect(g_engine, g_introspectClasses);
    }
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

    // Perception rides along on the same game-thread tick as marking.
    //
    // It reads the player pawn's transform so an external agent can tell
    // whether it is actually moving. Deliberately on this thread and not a new
    // one: it chases UObject pointers, which is only safe on the game thread,
    // and RunOnGameThread already exists here.
    segcap::GetPerception().StartPublishing();

    int tick = 0;
    int32_t lastObjectCount = 0;
    int stableTicks = 0;
    constexpr int kStableTicksRequired = 12;   // 12 x 250ms = 3s of a quiet array

    for (;;) {
        Sleep(250);
        ++tick;

        auto& pe = segcap::ue4::GetProcessEventHook();
        if (!pe.verified()) continue;

        // ---- world-stability gate ------------------------------------------
        //
        // Do not touch UObjects while the world is being torn down and rebuilt.
        //
        // inZOI reproducibly died ~25 seconds into loading a save, every time,
        // with the log ending mid-sentence and no error. Census runs survived
        // the identical load, so it was not injection, the D3D12 hooks, or
        // ProcessEvent -- the only difference was that marking runs call into
        // components and write to them. The generational handle check added
        // alongside this fixed a genuine use-after-free (it reported 106
        // objects destroyed under us in one pass, then 252 stale of 300
        // scanned) and the game still died, which says the remaining problem is
        // not stale pointers but reaching into an engine that is mid-transition
        // at all.
        //
        // So infer "the world is settled" from the thing that actually moves:
        // the object array's size. During a transition it swings continuously
        // as one level is destroyed and the next streams in. Three seconds of
        // it changing by less than 0.5% is a level that has finished arriving.
        //
        // Measured, not assumed -- no hardcoded "is this a menu" test and
        // nothing specific to either title. Stray sits in a stable array while
        // in a level, so it clears the gate immediately and behaves as before.
        //
        // Everything below this point touches the engine, including the
        // resolve retry's array walk, so the gate goes first.
        {
            const int32_t count = g_engine.NumObjects();
            const int32_t delta = count > lastObjectCount ? count - lastObjectCount
                                                          : lastObjectCount - count;
            if (lastObjectCount > 0 && delta * 200 < lastObjectCount) {
                ++stableTicks;
            } else {
                if (stableTicks >= kStableTicksRequired) {
                    segcap::LogInfo("customdepth: world is changing (%d -> %d objects) "
                                    "-- pausing marking until it settles",
                                    lastObjectCount, count);
                }
                stableTicks = 0;
            }
            lastObjectCount = count;
            if (stableTicks < kStableTicksRequired) continue;
            if (stableTicks == kStableTicksRequired) {
                segcap::LogInfo("customdepth: world settled at %d objects -- marking", count);
            }
        }

        // Keep trying to resolve, instead of spinning forever on a decision
        // made too early.
        //
        // This is the fourth time this exact bug has appeared in this project,
        // and the first line of RESUME.md's trap list warns about it: anything
        // checked once, at startup, will be checked at the wrong moment.
        //
        // On inZOI the field-layout calibration ran 12 seconds after injection,
        // while a 35 GB title was still loading, and reported
        // "FindClass(PrimitiveComponent): only found 1 candidate(s), all with
        // null property chains". Calibration correctly refused to guess and
        // returned false -- and nothing ever asked it again. Resolve() then
        // failed for the same reason, ready() stayed false, and this loop ran
        // for the rest of the session doing nothing at all. The capture
        // pipeline meanwhile happily dumped the game's OWN stencil buffer,
        // 154 distinct values of it, which looks exactly like a working
        // segmentation mask until you check whether anything was ever marked.
        if (!g_marker.ready()) {
            if (tick % 20 == 0) {   // every ~5s
                if (!g_engine.fieldLayoutCalibrated()) {
                    if (g_engine.CalibrateFieldLayout()) {
                        segcap::LogInfo("customdepth: field layout calibrated on retry "
                                        "(tick %d) -- classes were not loaded at startup",
                                        tick);
                    }
                }
                if (g_engine.fieldLayoutCalibrated()) {
                    // Resolve on the game thread (it chases UObject pointers),
                    // but NOT the collection pass -- that walks all ~500,000
                    // array slots, and doing that inside the engine's dispatch
                    // would stall a frame for seconds. It is read-only CPU
                    // work, so it belongs on this thread, which is where the
                    // original install path already ran it.
                    pe.RunOnGameThread([](segcap::ue4::Engine& e) { g_marker.Resolve(e); });
                    Sleep(500);   // give the queued task a tick to drain
                    if (g_marker.ready()) {
                        segcap::LogInfo("customdepth: RESOLVED on retry at tick %d -- "
                                        "collecting candidates", tick);
                        g_marker.CollectCandidates(g_engine, true);
                    }
                }
            }
            continue;
        }

        pe.RunOnGameThread([](segcap::ue4::Engine& e) {
            auto& p = segcap::GetPerception();
            if (!p.ready() && !p.Resolve(e)) return;
            segcap::AgentState s = {};
            p.Sample(e, s);          // hasPawn=0 is a normal answer, not a failure
            s.liveSlots = static_cast<uint32_t>(g_marker.markedCount());
            p.Publish(s);
        });

        // Do not queue another marking pass while one is still waiting.
        //
        // This loop queues on a 250ms timer, but the queue only drains when the
        // game thread reaches ProcessEvent -- and during streaming it can stall
        // for seconds and then drain everything at once. That is how four
        // MarkBatch passes ran inside 15 milliseconds and took the working set
        // from 0 to 255 in one frame. Rate-limiting the producer is not enough
        // if the consumer batches; the flag makes the pass self-limiting.
        if (!g_markPassPending.exchange(true, std::memory_order_acq_rel)) {
            pe.RunOnGameThread([](segcap::ue4::Engine& e) {
                g_marker.RefreshVisibility(e, 120);
                g_marker.MarkBatch(e, 300);
                g_markPassPending.store(false, std::memory_order_release);
            });
        }

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
