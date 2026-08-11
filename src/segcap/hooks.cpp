#include "hooks.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

// Linker-provided base of this module; avoids needing the HMODULE passed around
// just to find where our own DLL lives.
extern "C" IMAGE_DOS_HEADER __ImageBase;

#include "customdepth.h"
#include "hooks_format.h"
#include "log.h"

namespace segcap {
namespace {






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

}  // namespace

// ---- abandoning a reference to a destroyed resource -------------------------
//
// There are exactly two things you can do with an owned COM pointer, and the
// usual one is wrong here.
//
// We AddRef two resources: the elected depth-stencil (pinnedTarget_) and the
// proven id buffer (maskSource_). When D3D12 creates a NEW resource at an
// address we hold, that is proof the old occupant was destroyed -- a live
// reference makes address reuse impossible. So our count is already gone with
// it, and the pointer now names somebody else's live object.
//
// Release() at that point does not free our reference. It decrements the GAME'S,
// on a resource the game is still using. That is what killed inZOI at t=254.359:
// we released 0x1DFA4016C40 thirty-one milliseconds after the game created it,
// then faulted inside the NVIDIA driver while forwarding the game's own barrier
// for that very resource (nvwgf2umx `test byte [r10+0x3c],1`, r10 = 0 because the
// backing allocation was gone). The full chain is in the comment at the pin site.
//
// The correct response to "the object you referenced was destroyed" is to forget
// the pointer. Never to release it.
//
// NOTE the deliberate omission: heap ALIASING is not destruction. An aliased
// resource is still a live COM object and our reference is still genuinely ours,
// so the alias path in NoteResourceCreated must keep releasing normally. Calling
// this from there would leak.
void Hooks::AbandonOwnedRefs(ID3D12Resource* dead, const char* why) {
    if (!dead) return;

    if (pinnedTarget_ == dead) {
        ++refsAbandoned_;
        LogWarn("abandoning (NOT releasing) our pin on %p: %s. The AddRef we took is "
                "already gone with the object; releasing now would steal a reference "
                "from the resource that inherited this address (abandon #%llu)",
                static_cast<void*>(dead), why, refsAbandoned_);
        pinnedTarget_ = nullptr;
        electedDesc_ = {};
    }

    if (maskSource_ == dead) {
        ++refsAbandoned_;
        LogWarn("abandoning (NOT releasing) the id buffer %p: %s. Re-opening the probe "
                "(abandon #%llu)",
                static_cast<void*>(dead), why, refsAbandoned_);
        maskSource_ = nullptr;
        maskChannel_ = -1;
        maskSourceMissing_ = 0;
        idChannelFound_ = false;
        idDumped_ = 0;
        loggedIdExhausted_ = false;
        idRejected_.clear();
        warnedUnleasedIds_ = false;
        warnedMaskSourceUnshadowed_ = false;
    }

    // Borrowed pointers -- no reference to abandon, but still dangling, and
    // idTarget_ in particular was fed to Readback::Prepare -> GetDesc() every
    // frame for 197 s after its last sighting.
    if (idTarget_ == dead) idTarget_ = nullptr;
    ID3D12Resource* expected = dead;
    injectTarget_.compare_exchange_strong(expected, nullptr);
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
    if (foreignInject_) {
        injectFrame_.store(frameIndex_, std::memory_order_relaxed);
        // One ring serves both phases. While the probe is still deciding which
        // buffer carries our ids, completed copies go to the channel test; once
        // a buffer is proven, the same ring feeds the mask writer.
        if (maskSource_) {
            maskInjectRing_.Drain([this](const MaskFrame& f) { OnMaskReady(f); });
        } else {
            maskInjectRing_.Drain([this](const MaskFrame& f) { OnIdBufferReady(f); });
        }
        // A list we recorded into can be Reset or simply abandoned without ever
        // being submitted; without reclaiming, the ring starves to nothing and
        // capture stops with no error anywhere.
        maskInjectRing_.ReclaimStaleRecordings(frameIndex_, 240);
    }

    // Election runs every frame too. It is cheap (a handful of targets) and the
    // elected target can legitimately change -- resolution changes, or a pass
    // that only appears in some frames. Electing only on logging frames would
    // mean reading a stale or never-set target for 299 frames out of 300.
    const std::vector<TargetFingerprint> snapshot = SnapshotTargets();
    const std::vector<ElectionScore> ranked = ScoreTargets(snapshot);
    ID3D12Resource* const scored =
        (!ranked.empty() && ranked.front().score > 0) ? ranked.front().resource : nullptr;

    // A candidate is only real if we saw it THIS frame. SnapshotTargets returns
    // evidence_, which keeps entries for 600 frames after last sighting, so the
    // top-scoring address can name a resource the game destroyed long ago.
    // Electing one means GetDesc and CopyTextureRegion on freed memory; pinning
    // one means an AddRef that holds nothing and a later Release that steals a
    // reference from whoever inherits the address. Both happened -- see below.
    bool winnerSeenThisFrame = false;
    if (scored) {
        for (const TargetFingerprint& t : snapshot) {
            if (t.resource == scored) {
                winnerSeenThisFrame = (t.lastSeenFrame == frameIndex_);
                break;
            }
        }
        if (!winnerSeenThisFrame) {
            ++pinRefusedStale_;
            if (pinRefusedStale_ <= 8 || (pinRefusedStale_ % 200) == 0) {
                LogWarn("election: top candidate %p was NOT observed this frame -- refusing to "
                        "elect, pin or even GetDesc it; it may already be destroyed "
                        "(refusal #%llu)",
                        static_cast<void*>(scored), pinRefusedStale_);
            }
        }
    }
    ID3D12Resource* const winner = winnerSeenThisFrame ? scored : nullptr;
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
        // WRONG, and it cost a crash. The winner comes from SnapshotTargets(),
        // which returns evidence_ -- and evidence_ retains an entry for up to
        // kStaleAfterFrames = 600 frames after the resource was last seen
        // (hooks_election.cpp). So the winner may have been destroyed ten
        // seconds ago, and this AddRef lands on freed memory and holds nothing.
        //
        // The proof it held nothing is that D3D12 then reissued the address --
        // impossible while a real reference is outstanding. What followed:
        //   t=250.984  AddRef 0x1DFA4016C40                       (log:13339)
        //   t=254.328  address recycled, electedTarget_ nulled     (log:14123)
        //   t=254.359  pinnedTarget_->Release() on the NEW owner   <- theft
        //   +ms        game barriers 0x1DFA4016C40; allocation gone
        //              nvwgf2umx: test byte [r10+0x3c],1  with r10 = 0
        // We destroyed a resource the game had created 31 ms earlier, then
        // faulted inside the driver while forwarding the game's own barrier.
        //
        // So `winner` above is already gated on being observed this frame, and
        // the second half of the repair lives in AbandonOwnedRefs: once an
        // address is recycled the pin must be FORGOTTEN, never released.
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
        // DROP A CANDIDATE WE NEVER SEE BARRIERED.
        //
        // Candidate selection is gated on `!idTarget_`, so a stale idTarget_ pins
        // the probe forever. AbandonOwnedRefs clears it when a resource CHANGES
        // IDENTITY -- a new resource appearing at the same address -- but a
        // resource that simply dies and whose address is never reused trips
        // nothing at all. The pointer stays, the gate stays shut, and the probe
        // is finished for the session.
        //
        // Observed: a candidate elected at t=39s in the MENU, whose resource the
        // world transition destroyed. The run then went 639 seconds with
        // `inject: attempts=0` -- not one barrier ever matched, because the
        // target no longer existed -- while the probe sat on it and 74 live slots
        // went unread. Zero attempts is the signal: injection fires from the
        // game's own barriers, so a target that draws none in ten seconds of
        // gameplay is one we cannot copy from, whatever it used to be.
        //
        // Dropped, NOT blacklisted: the resource may be legitimately idle rather
        // than wrong, and idRejected_ is permanent.
        if (idTarget_ && frameIndex_ > idTargetSetFrame_ + 600 &&
            injAttempts_ == injAttemptsAtSet_) {
            LogWarn("idbuf: candidate %p drew ZERO barriers in %llu frames -- it is gone or "
                    "unused; dropping it and picking another",
                    static_cast<void*>(idTarget_), frameIndex_ - idTargetSetFrame_);
            idTarget_ = nullptr;
            idDumped_ = 0;
        }
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
                // Once a format has been PROVEN to carry our ids, only consider
                // that format. Re-walking R32_UINT and R8_UINT decoys after a
                // world transition is how a converged session loses its buffer
                // and never gets it back -- and the R8_UINT decoy in particular
                // matches trivially, because a flag buffer of {0,1,2} overlaps
                // leased slots 1, 2 and 3.
                if (provenFormat_ != DXGI_FORMAT_UNKNOWN && t.format != provenFormat_) continue;
                idCandidates_.push_back(t.resource);
            }
            // Deterministic order so a re-run probes the same sequence, and so
            // "candidate 2 of 5" in the log means the same thing twice.
            std::sort(idCandidates_.begin(), idCandidates_.end());
            idCandidateIndex_ = 0;
            if (idCandidateIndex_ < idCandidates_.size()) {
                idTarget_ = idCandidates_[idCandidateIndex_];
                // Stamped so the zero-barrier watchdog above can time it out.
                idTargetSetFrame_ = frameIndex_;
                injAttemptsAtSet_ = injAttempts_;
                D3D12_RESOURCE_DESC d = idTarget_->GetDesc();
                LogInfo("idbuf: probing candidate %zu of %zu: %p %llux%u %s",
                        idCandidateIndex_ + 1, idCandidates_.size(),
                        static_cast<void*>(idTarget_), d.Width, d.Height,
                        FormatName(d.Format));
            } else if (!idRejected_.empty()) {
                // EXHAUSTED IS NOT FINAL. This set loggedIdExhausted_ and closed
                // the probe for the rest of the process -- a permanent verdict
                // drawn from whichever targets happened to exist at one instant.
                //
                // The comment forty lines up says why that cannot hold: the
                // Nanite CombinedCustomStencil DOES NOT EXIST until something
                // first requests the CustomDepth pass. Testing every target
                // present at t=49s and concluding "the ids are not in an integer
                // target on this title" is concluding it before the buffer has
                // been allocated. Observed exactly that: two candidates rejected
                // at t=49.8s, then silence for 190 seconds while capture sat
                // armed and 255 slots went unread.
                //
                // Forget the rejections and look again. A target that genuinely
                // holds nothing costs one cheap re-test per sweep; the target we
                // need appears only after the pass runs, and it is the only
                // reason this probe exists.
                if (frameIndex_ > idSweepFrame_ + 1800) {
                    idSweepFrame_ = frameIndex_;
                    LogWarn("idbuf: all %zu present integer targets rejected -- clearing and "
                            "re-sweeping. The Nanite CombinedCustomStencil is allocated only "
                            "once the CustomDepth pass runs, so 'not here yet' is not 'not here'.",
                            idRejected_.size());
                    idRejected_.clear();
                }
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
        // With foreign injection on, the probe does NOT copy at Present either.
        // The probe's own copy is what killed inZOI at t=46.8s in the dry run --
        // the same unknowable-state problem as the capture path, just reached
        // earlier. One rule for the whole process: on this engine, every copy
        // out of a transient resource is recorded into the game's own list.
        // OnPresent publishes the probe candidate as injectTarget_ below.
        if (foreignInject_) {
            // nothing to do here; injection drives it
        } else if (idTarget_) {
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

    // LIVE RE-PROBE. Touch "segcap.reprobe" next to the DLL and discovery starts
    // over, in the running game, without a relaunch.
    //
    // Getting inZOI to a loaded save costs six minutes of menu and streaming, and
    // every probe experiment was paying that toll to test a decision the DLL makes
    // in the first two seconds after arriving. Worse, a failed probe was terminal
    // for the session: idRejected_ is permanent and idChannelFound_ never resets,
    // so the ONLY way to try again was to destroy the world and rebuild it.
    //
    // This resets exactly the state discovery accumulates -- rejections, the
    // current candidate, the proven channel and the pinned mask source -- and
    // leaves the world, the hooks and the marked slots untouched. The marker is
    // deleted as it is consumed so it fires once per touch. Code changes still
    // need a relaunch (the DLL is mapped and MinHook's detours point into it),
    // but the decisions we actually iterate on no longer do.
    if ((frameIndex_ % 30) == 0) {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
        std::wstring p(self);
        const size_t dot = p.find_last_of(L'.');
        if (dot != std::wstring::npos) p = p.substr(0, dot);
        p += L".reprobe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(p.c_str());
            LogWarn("RE-PROBE requested -- discarding %zu rejection(s), candidate %p, "
                    "proven channel %d and mask source %p; discovery restarts now",
                    idRejected_.size(), static_cast<void*>(idTarget_), maskChannel_,
                    static_cast<void*>(maskSource_));
            if (maskSource_) {
                maskSource_->Release();
                maskSource_ = nullptr;
            }
            maskChannel_ = -1;
            idChannelFound_ = false;
            idRejected_.clear();
            idCandidates_.clear();
            idCandidateIndex_ = 0;
            idTarget_ = nullptr;
            idDumped_ = 0;
            loggedIdExhausted_ = false;
            warnedProbeTooEarly_ = false;
            loggedInjectArmed_ = false;
            injectTarget_.store(nullptr, std::memory_order_relaxed);
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
    // ---- is the pinned id buffer still the one the game renders into? -------
    //
    // AddRef keeps the COM OBJECT alive; it does not keep the game using it, and
    // under UE5's transient allocator it does not even keep the heap memory.
    // Measured: the pinned resource was last bound by the game 7.3s after we
    // pinned it, and in the following 894 census emissions 891 contained a live
    // 1280x800 R16G16_UINT and NONE contained ours. Every one of 67,547
    // "captured" frames was copied from a resource the game had abandoned, whose
    // memory was meanwhile being rewritten by other passes -- live garbage rather
    // than a stale snapshot, which is why the content kept changing and looked
    // superficially plausible.
    //
    // So the id buffer needs what the depth-stencil election already has: it must
    // be re-confirmed against what is actually being bound, and dropped the
    // moment it stops appearing. Dropping it re-opens the probe, which then
    // re-identifies the current buffer by the same leased-slot test.
    if (maskSource_) {
        bool stillBound = false;
        for (const TargetFingerprint& t : snapshot) {
            if (t.resource == maskSource_) { stillBound = true; break; }
        }
        if (stillBound) {
            maskSourceMissing_ = 0;
        } else if (++maskSourceMissing_ > kMaskSourceMissingLimit) {
            LogWarn("capture: id buffer %p has not been bound for %u frames -- the game has "
                    "moved to a different resource. Dropping it and re-probing rather than "
                    "copying memory nobody is writing our ids into.",
                    static_cast<void*>(maskSource_), maskSourceMissing_);
            maskSource_->Release();
            maskSource_ = nullptr;
            maskChannel_ = -1;
            maskSourceMissing_ = 0;
            // Re-open the probe. The candidate we just dropped is NOT added to
            // idRejected_: it was right once, and its successor is a different
            // resource that must be judged on its own evidence.
            idChannelFound_ = false;
            idTarget_ = nullptr;
            idDumped_ = 0;
            loggedIdExhausted_ = false;
            idRejected_.clear();
            warnedUnleasedIds_ = false;
            warnedMaskSourceUnshadowed_ = false;
        }
    }

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

    // Foreign-list injection takes over ONLY for the proven id buffer. Stray and
    // the depth-stencil path below are byte-for-byte unchanged.
    //
    // Note it does NOT require `canCopy`: that gate exists because the
    // Present-time path has to declare a StateBefore from our shadow, and it
    // refuses when the shadow has no evidence. Injection declares a StateBefore
    // copied out of the game's own barrier, so the gate it was protecting
    // against does not apply -- the arm/census/noReadback conditions still do.
    // Injection covers BOTH phases: the probe's candidate before a buffer is
    // proven, and the proven buffer afterwards. Whichever it is, the copy is
    // recorded into the game's list rather than issued here.
    ID3D12Resource* const injectWant = maskSource_ ? maskSource_ : idTarget_;
    // The probe reads colour targets (plane 0); so does the proven id buffer.
    const UINT injectPlane = maskSource_ ? copyPlane : 0u;
    // Probing must not wait for the capture arm -- it has to converge BEFORE
    // capture can begin, which is the ordering the whole id-buffer route needs.
    const bool injectArmOk = maskSource_ ? armReady : true;

    const bool useInject = foreignInject_ && injectWant &&
                           (maskSource_ ? copySource == maskSource_ : true);
    if (useInject) {
        if (!censusOnly_ && !noReadback_ && injectArmOk && device_ &&
            maskInjectRing_.Prepare(device_, injectWant, injectPlane)) {
            // Publish the gate ResourceBarrier_ reads. Written every frame so a
            // dropped maskSource_ (staleness, aliasing, recycling) disarms
            // injection on the very next Present.
            injectTarget_.store(injectWant, std::memory_order_relaxed);
            if (!loggedInjectArmed_) {
                loggedInjectArmed_ = true;
                LogWarn("inject ARMED for %p (%s) -- the copy will be recorded into the "
                        "game's own command list, with StateBefore taken from the game's "
                        "barrier rather than from our shadow",
                        static_cast<void*>(injectWant),
                        maskSource_ ? "proven id buffer" : "probe candidate");
            }
        } else {
            injectTarget_.store(nullptr, std::memory_order_relaxed);
        }
        // Say WHY injection is off, once per transition.
        //
        // A run reported 0 masks with `inject: attempts` frozen at 58 for its
        // last 300 seconds and no other line to explain it. Injection turning
        // itself off is the single most consequential state change in this file
        // and it was completely silent, so every diagnosis of it was a guess
        // about which of five conditions had gone false. Log the actual one.
        {
            const bool onNow = injectTarget_.load(std::memory_order_relaxed) != nullptr;
            if (onNow != injectWasOn_) {
                injectWasOn_ = onNow;
                if (onNow) {
                    LogWarn("inject: ON (target %p, plane %u)",
                            static_cast<void*>(injectWant), injectPlane);
                } else {
                    LogWarn("inject: OFF -- want=%p maskSource=%p idTarget=%p | "
                            "census=%d noReadback=%d armOk=%d device=%d prepare=%s",
                            static_cast<void*>(injectWant), static_cast<void*>(maskSource_),
                            static_cast<void*>(idTarget_), censusOnly_ ? 1 : 0,
                            noReadback_ ? 1 : 0, injectArmOk ? 1 : 0, device_ ? 1 : 0,
                            injectWant ? "failed-or-not-reached" : "n/a (no target)");
                }
            }
        }
        if (auto sc = GetMarker().publishedSidecar()) {
            sidecarHistory_.emplace_back(frameIndex_, std::move(sc));
            while (sidecarHistory_.size() > 16) sidecarHistory_.pop_front();
        }
        frameStamps_.emplace_back(frameIndex_, NowMs());
        while (frameStamps_.size() > 16) frameStamps_.pop_front();
    } else if (!censusOnly_ && !noReadback_ && canCopy && copySource && device_ && queue_) {
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
    if (foreignInject_) {
        // Every refusal reason, so a run that injects nothing says WHICH
        // precondition failed instead of going quiet. Silence has cost this
        // project several runs.
        LogInfo("    inject: attempts=%llu recorded=%llu submitted=%llu | refused: "
                "renderpass=%llu flags=%llu subres=%llu not-write-to-read=%llu noslot=%llu "
                "| ring submitted=%llu delivered=%llu dropped=%llu",
                injAttempts_, injRecorded_, injSubmitted_, injRefusedRenderPass_,
                injRefusedFlags_, injRefusedSubresource_, injRefusedNotWriteToRead_,
                injRefusedNoSlot_, maskInjectRing_.submitted(), maskInjectRing_.delivered(),
                maskInjectRing_.dropped());
    }

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
    //
    // With ONE exception, collapsed to a count: "no stencil plane". That line
    // was 63,000 of the 88,000 lines a 109-second session wrote -- roughly 12x
    // the previous rate -- all of it emitted synchronously on the Present thread,
    // which is a real cost paid every frame to restate a fact about a FORMAT that
    // cannot change between frames. Nothing is lost: a target rejected on its
    // format never had dimensions or bind counts printed here anyway, and every
    // integer target worth knowing about is already enumerated above with its
    // size. The runners-up that actually competed still print in full.
    LogInfo("  election:");
    size_t noStencil = 0;
    for (const ElectionScore& e : ranked) {
        if (std::strncmp(e.reason, "rejected: no stencil plane", 26) == 0) {
            ++noStencil;
            continue;
        }
        LogInfo("    %+5d %p  %s", e.score, static_cast<void*>(e.resource), e.reason);
    }
    if (noStencil) {
        LogInfo("    (%zu more rejected: no stencil plane)", noStencil);
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
