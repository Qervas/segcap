#include "ue4.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

namespace segcap {
namespace ue4 {

// ---------------------------------------------------------------------------
// ProcessEvent
// ---------------------------------------------------------------------------

namespace {

// Candidate vtable indices for UObject::ProcessEvent in UE4 x64 shipping. The
// exact slot moves with engine version and with which virtuals the game's build
// includes, so a window is tried rather than a single number -- and every
// candidate must PROVE itself before being kept.
constexpr int kProcessEventCandidateFirst = 60;
constexpr int kProcessEventCandidateLast = 80;

// How many correctly-shaped calls a candidate must produce before we believe it.
// One could be luck; a steady stream through the engine's dispatch path is not.
constexpr uint64_t kProofThreshold = 24;

Engine* g_engine = nullptr;
ProcessEventHook* g_hook = nullptr;

// Four parameters, not three. The real signature is
// ProcessEvent(this, UFunction*, void*), but while SEARCHING we may briefly be
// attached to some other virtual that takes more arguments. Declaring four
// keeps RCX/RDX/R8/R9 faithfully passed through, so a wrong candidate is a
// no-op rather than a corrupted call. Virtuals with more than four register
// arguments are the residual risk, and are rare.
using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*);
ProcessEventFn g_origProcessEvent = nullptr;

std::mutex g_queueMutex;
std::vector<ProcessEventHook::GameThreadTask> g_queue;
std::atomic<uint64_t> g_validCalls{0};
std::atomic<uint64_t> g_invalidCalls{0};

// How many calls a candidate may be validated over before validation switches
// itself off. THIS IS THE FIX FOR THE UE5 HANG, and it is worth being explicit
// about why, because the first diagnosis was wrong.
//
// The original detour ran IsDerivedFrom -- a shared_mutex acquisition plus a
// binary search over the module map plus a class-chain walk -- on EVERY call.
// On Stray that was survivable; UE4's dispatch rate through this slot is a few
// thousand per second. On UE5 the same slot fires ~300,000 times per second,
// and the game froze inside two seconds. That looked like memory corruption and
// was diagnosed as such twice. It was not. It was a hook doing real work at
// 300 kHz, which no amount of correctness in the work itself would have saved.
//
// 2000 samples is far more than the 24 needed to prove a candidate, and it is a
// fixed cost rather than one scaling with the game's dispatch rate. A title
// that calls the slot ten million times a second pays exactly the same.
constexpr uint64_t kValidationBudget = 2000;
std::atomic<uint64_t> g_validationSpent{0};

// Set once, after a candidate is proven. Read on every call, so it must stay an
// atomic load and nothing more.
std::atomic<bool> g_slotProven{false};

// Lets the steady-state path skip the mutex entirely when there is no work,
// which is the overwhelmingly common case.
std::atomic<uint32_t> g_queueDepth{0};

void __fastcall ProcessEventDetour(void* self, void* function, void* parms, void* spare) {
    if (g_slotProven.load(std::memory_order_relaxed)) {
        // Steady state. The slot is already known to be correct, so there is
        // nothing left to validate -- re-proving it on every call was pure
        // overhead. An atomic load, and a lock only when work actually exists.
        if (g_queueDepth.load(std::memory_order_acquire) != 0 && g_hook && g_engine) {
            std::vector<ProcessEventHook::GameThreadTask> todo;
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                todo.swap(g_queue);
                g_queueDepth.store(0, std::memory_order_release);
            }
            // We are on the game thread, inside the engine's own dispatch,
            // which is exactly the safe point this hook exists to provide.
            for (auto& task : todo) task(*g_engine);
        }
    } else if (g_validationSpent.fetch_add(1, std::memory_order_relaxed) < kValidationBudget) {
        // Search mode, and still within budget. The proof: ProcessEvent's
        // second argument is always a UFunction. If this slot is some other
        // virtual, that argument will be an int, a pointer to something else,
        // or garbage -- and the class-chain walk will reject it.
        if (g_engine && g_engine->IsDerivedFrom(function, "Function")) {
            g_validCalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_invalidCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_origProcessEvent(self, function, parms, spare);
}

}  // namespace

bool ProcessEventHook::TryIndex(Engine& engine, int index) {
    // Refresh before every candidate. The search runs for ~33 seconds while the
    // game allocates continuously, and a stale memory map turns "UFunction I
    // cannot currently see" into "not a UFunction" -- which is counted as an
    // invalid call and can sink a correct candidate. Candidate 68 scored
    // 30145 valid / 0 invalid in one run and 742 / 654 in the next for exactly
    // this reason.
    engine.RefreshMemoryMap();

    void* sample = engine.AnyObject();
    if (!sample) return false;

    auto** vtable = *reinterpret_cast<void***>(sample);
    if (!IsReadable(&vtable[index], sizeof(void*))) return false;
    void* target = vtable[index];
    if (!IsReadable(target, 16)) return false;

    g_validCalls.store(0);
    g_invalidCalls.store(0);
    g_validationSpent.store(0);
    g_slotProven.store(false);

    if (MH_CreateHook(target, reinterpret_cast<void*>(&ProcessEventDetour),
                      reinterpret_cast<void**>(&g_origProcessEvent)) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }

    // Observe. A correct candidate produces a steady stream of calls whose
    // second argument validates as a UFunction. Poll rather than sleeping the
    // full window: a hot slot spends its whole 2000-sample budget in a few
    // milliseconds, and once the budget is gone the extra wait buys nothing.
    for (int waited = 0; waited < 1500; waited += 50) {
        if (g_validationSpent.load(std::memory_order_relaxed) >= kValidationBudget) break;
        Sleep(50);
    }

    const uint64_t valid = g_validCalls.load();
    const uint64_t invalid = g_invalidCalls.load();

    // Accept only on a clear signal: enough proven calls, and overwhelmingly
    // more valid than invalid. A slot that is called often with arguments that
    // do not validate is a different virtual, not a noisy ProcessEvent.
    const bool accept = valid >= kProofThreshold && valid > invalid * 4;
    LogInfo("ue4: PE candidate %d -> %llu valid, %llu invalid (%llu sampled) %s",
            index, valid, invalid, g_validationSpent.load(),
            accept ? "ACCEPTED" : "(rejected)");

    if (!accept) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_origProcessEvent = nullptr;
        return false;
    }

    vtableIndex_ = index;
    verified_ = true;
    // Only now does the detour stop validating. Ordering matters: set this
    // after verified_ so no call can see a half-committed state.
    g_slotProven.store(true, std::memory_order_release);
    return true;
}

bool ProcessEventHook::Install(Engine& engine) {
    if (verified_) return true;
    if (!engine.ready() || !engine.namesResolved()) {
        LogError("ue4: ProcessEvent needs discovery and name resolution first");
        return false;
    }

    g_engine = &engine;
    g_hook = this;

    // Read-only triage first. This costs nothing and cannot crash anything: it
    // reads vtables from ~40 live objects and measures each slot. On Stray it
    // takes 78 slots down to 3 with the known answer among them, so the blind
    // 60..80 sweep below is now a fallback rather than the plan.
    //
    // The slot numbers themselves do not transfer between engine versions --
    // that was the original mistake. What transfers is the property: the slot
    // holding ProcessEvent contains the same function pointer in every class's
    // vtable, and that function is large. Measure the property, not the index.
    std::vector<int> candidates = engine.ProcessEventCandidates();
    if (!candidates.empty()) {
        std::string list;
        for (int c : candidates) list += " " + std::to_string(c);
        LogInfo("ue4: triage shortlisted %zu slot(s) for ProcessEvent:%s",
                candidates.size(), list.c_str());
        for (int i : candidates) {
            if (TryIndex(engine, i)) {
                gameThreadId_ = GetCurrentThreadId();  // provisional; corrected on first call
                LogInfo("ue4: ProcessEvent CONFIRMED at vtable index %d (from triage)", i);
                return true;
            }
        }
        LogWarn("ue4: no shortlisted slot validated; falling back to the blind sweep");
    } else {
        LogWarn("ue4: triage produced no candidates; falling back to the blind sweep");
    }

    LogInfo("ue4: searching vtable slots %d..%d for ProcessEvent",
            kProcessEventCandidateFirst, kProcessEventCandidateLast);

    for (int i = kProcessEventCandidateFirst; i <= kProcessEventCandidateLast; ++i) {
        if (std::find(candidates.begin(), candidates.end(), i) != candidates.end()) continue;
        if (TryIndex(engine, i)) {
            gameThreadId_ = GetCurrentThreadId();  // provisional; corrected on first call
            LogInfo("ue4: ProcessEvent CONFIRMED at vtable index %d", i);
            return true;
        }
    }

    LogError("ue4: ProcessEvent not found in slots %d..%d",
             kProcessEventCandidateFirst, kProcessEventCandidateLast);
    return false;
}

void ProcessEventHook::Uninstall() {
    // Deliberately left installed for the process lifetime -- see dllmain's
    // note on removing trampolines out from under running threads.
    verified_ = false;
}

void ProcessEventHook::CallFunction(void* object, void* function, void* params) {
    if (!verified_ || !g_origProcessEvent || !object || !function) return;
    // Calls the ORIGINAL, not the detour: re-entering our own hook here would
    // recurse through the validation path for no benefit.
    g_origProcessEvent(object, function, params, nullptr);
}

void ProcessEventHook::RunOnGameThread(GameThreadTask task) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(std::move(task));
    // Published after the push, and read with acquire in the detour, so a
    // non-zero depth always implies the task is visible.
    g_queueDepth.store(static_cast<uint32_t>(g_queue.size()), std::memory_order_release);
}

ProcessEventHook& GetProcessEventHook() {
    static ProcessEventHook instance;
    return instance;
}

}  // namespace ue4
}  // namespace segcap
