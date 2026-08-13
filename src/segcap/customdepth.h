// customdepth.h -- opt primitives into Unreal's CustomDepth pass.
//
// This is the first code in the project that WRITES to the game. Everything
// before it only read memory, which is why several live Stray sessions were
// risk-free. Three properties of the design keep that true:
//
//   1. Writes happen on the GAME THREAD, via the proven ProcessEvent hook.
//      UObject state belongs to that thread; writing from the render thread
//      (where the D3D hooks run) is a data race that can work a thousand times
//      and then corrupt something.
//
//   2. bRenderCustomDepth is a PACKED BIT, not a byte. Reflection reported it at
//      byte +0x216 with mask 0x40 -- bit 6 of a byte shared with bUseAsOccluder,
//      bSelectable and other flags. A byte store would silently flip those,
//      which is precisely the "did you change what the player sees" failure, and
//      it would have looked like it worked. Every write is read-modify-write.
//
//   3. Every write is VERIFIED and REVERSIBLE. The prior byte is recorded, the
//      write is read back, and we assert only the masked bit changed. Originals
//      are kept so the whole thing can be undone.
//
// No offsets are hardcoded. Every one comes from runtime reflection.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "identity.h"
#include "ue4.h"

namespace segcap {

// One primitive we have opted in, and everything needed to undo it.
struct MarkedPrimitive {
    void* component = nullptr;
    int32_t objectIndex = 0;
    int32_t serialNumber = 0;      // guards against GC slot reuse
    std::string className;
    uint8_t stencilValue = 0;      // the slot written to CustomDepthStencilValue
    uint8_t originalByte = 0;      // byte holding bRenderCustomDepth, before us
    int32_t originalStencil = 0;
};

class CustomDepthMarker {
public:
    // Resolves the property offsets via reflection. Returns false (and logs the
    // specific missing property) rather than proceeding with any guessed value.
    bool Resolve(ue4::Engine& engine);

    bool ready() const { return resolved_; }

    // Walks the whole object array and records renderable primitives that are
    // not yet marked. Safe to call from ANY thread -- it only reads.
    //
    // Split from marking deliberately. The array walk is cheap CPU work over
    // ~350,000 slots; the expensive part is the two UFunction calls per match,
    // which must happen on the game thread. An earlier version bounded the WALK
    // at 3000 entries per pass, which swept only 4% of the array -- and the low
    // indices are almost all engine bootstrap objects (Class, Package,
    // Function), so it found 16 primitives where a full sweep found 267.
    //
    // `renderableOnly` skips collision volumes and editor gizmos. Stray has
    // ~2235 BoxComponents, 577 CapsuleComponents, 613 ArrowComponents and 480
    // BillboardComponents that render nothing at all; marking them would spend
    // stencil slots on objects that produce no pixels, and there are only 255.
    int CollectCandidates(ue4::Engine& engine, bool renderableOnly = true);

    // Scans up to `scanLimit` candidates and marks the ones the renderer has
    // actually drawn recently. MUST run on the game thread.
    //
    // Bounded because every candidate tested costs a UFunction call inside the
    // engine's own dispatch, and each mark costs two more.
    //
    // Testing visibility BEFORE spending a slot is the difference between a
    // working mask and an empty one. An earlier version marked the first 255
    // candidates it found. In the menu that happened to produce content, because
    // there were few enough objects that some of them were on screen. In a real
    // level there are 32,836 markable primitives and maybe a few hundred visible
    // at once, so 255 arbitrary picks contained no visible object at all and
    // every mask came back empty -- while the logs cheerfully reported "255 live
    // slots, 0 evictions", which is a perfectly healthy-looking way to describe
    // labelling 255 things nobody can see.
    int MarkBatch(ue4::Engine& engine, int scanLimit);

    // Re-tests up to `limit` currently-marked primitives and hands back the
    // slots of any the renderer has stopped drawing. MUST run on the game
    // thread. This is what lets the 255-slot working set follow the camera
    // instead of being decided once and frozen.
    int RefreshVisibility(ue4::Engine& engine, int limit);

    size_t pendingCount() const { return pending_.size(); }
    size_t markedCount() const { return marked_.size(); }

    // How many PIXELS each slot actually covered in the last mask.
    //
    // Slots were held purely on UE's WasRecentlyRendered, which is a visibility
    // test: a playground two blocks away is "recently rendered" exactly as much
    // as the chair the Zoi is sitting on. With only 255 ids for a world of
    // 560,000 objects, that spends the entire budget on whatever the object
    // scan reached first, and nearby objects never get labelled at all --
    // measured at 15-64% of pixels covered, with the seat under the character
    // unlabelled while distant palms were not.
    //
    // Screen area is the thing we actually care about and the mask measures it
    // for free. A slot whose id keeps rendering to almost nothing is paying for
    // itself in nothing.
    void NoteMaskCoverage(const uint32_t* pixelsPerSlot);
    size_t slotsFreedForCoverage() const { return slotsFreedForCoverage_; }

    // Thread-safe snapshot of the current slot table, for emitting alongside a
    // captured mask. Marking runs on the game thread; mask dumps happen on the
    // render thread, so the table cannot simply be read across.
    FrameSidecar SnapshotSidecar(uint64_t frameIndex, uint32_t w, uint32_t h) const;

    // The current slot table as an immutable, shareable snapshot.
    //
    // Published by the marking thread whenever the table actually changes,
    // which is a few times a second -- not per frame. The render thread can
    // therefore record "which table produced this frame" by copying a
    // shared_ptr, at no per-frame cost, and a mask that lands several frames
    // later is still written out with the table that was live when its pixels
    // were rendered.
    std::shared_ptr<const FrameSidecar> publishedSidecar() const;

    IdentityRegistry& identity() { return identity_; }

    // Restores every marked primitive to its recorded original bytes.
    int RestoreAll();

    const std::vector<MarkedPrimitive>& marked() const { return marked_; }

    // ---- ground truth by intervention --------------------------------------
    //
    // Turn ONE slot off, deliberately, and see whether exactly its pixels vanish.
    //
    // Every validator in tools/ is necessary and not sufficient, and they say so
    // in their own output: a label that is wrong in a spatially and temporally
    // coherent way passes all of them. That is not hypothetical here -- a full
    // session of masks once passed verify_labels while containing another render
    // target's bytes entirely.
    //
    // This is the one test that cannot be satisfied by coherent-but-wrong data.
    // If our claim "slot S means that object" is true, clearing S's flag removes
    // that object's pixels and nothing else. If the mask is really some other
    // buffer, or the slot table is a fiction, the pixels do not move.
    //
    // Returns the component that was unmarked, or nullptr if the slot was not
    // held. Must run on the game thread: it calls the engine's own setter.
    void* UnmarkSlotForGroundTruth(ue4::Engine& engine, uint8_t slot);

    // Release the ground-truth slot back to ordinary slot management once the
    // verdict has been recorded. Held only for the duration of the measurement.
    void ClearGroundTruthPin() { groundTruthPin_ = 0; }

    // Spend slots on what is NEAR, not merely on what the engine last drew.
    //
    // WasRecentlyRendered is a visibility test with no notion of distance, so a
    // tree eighty metres down the street passes it exactly as well as the chair
    // the character is sitting on. Indoors that is most of the budget: 95 of 255
    // ids in view while the road outside holds slots.
    //
    // Radius is in world units (centimetres in both titles). 0 disables the gate
    // entirely, which is the behaviour before this existed.
    void SetMarkRadius(double units) { markRadius_ = units; }
    double markRadius() const { return markRadius_; }
    size_t skippedFar() const { return skippedFar_; }
    size_t releasedFar() const { return releasedFar_; }

    // The slot whose flag was ACTUALLY cleared, or 0 if the unmark has not run
    // or found nothing to unmark. The intervention is scheduled onto the game
    // thread, so this is the only way the render thread can tell "the experiment
    // ran" from "the experiment was requested".
    uint8_t GroundTruthPinnedSlot() const { return groundTruthPin_.load(); }

    // Slot -> object identity, for the per-frame sidecar table. The 8-bit
    // stencil value is a lease, not an identity: it only means something joined
    // against this map.
    const std::unordered_map<uint8_t, MarkedPrimitive>& slotTable() const {
        return slotTable_;
    }

private:
    bool WriteMarked(void* component, uint8_t stencilValue, MarkedPrimitive& out);

    // Asks the ENGINE whether it drew this primitive recently, via the
    // UPrimitiveComponent::WasRecentlyRendered UFunction.
    //
    // Deliberately the engine's own answer rather than a reimplementation.
    // Computing visibility ourselves would mean frustum culling against the
    // active camera, then occlusion, then LOD and distance culling -- and any
    // disagreement with the renderer produces slots leased to objects that
    // contribute no pixels. The engine already knows; asking it is both less
    // code and more correct.
    bool WasRecentlyRendered(void* component, float toleranceSeconds);

    // Turns the CustomDepth flag back off and forgets the primitive.
    bool UnmarkPrimitive(const MarkedPrimitive& mp);

    bool resolved_ = false;
    bool reportedRejects_ = false;   // the discard histogram is logged once
    ue4::PropertyInfo propRenderCustomDepth_;
    ue4::PropertyInfo propStencilValue_;

    // The engine's own setters. Calling these rather than writing the property
    // is what triggers MarkRenderStateDirty() and rebuilds the render-thread
    // proxy -- without it the renderer never learns the flag changed.
    void* fnSetRenderCustomDepth_ = nullptr;
    void* fnSetCustomDepthStencilValue_ = nullptr;
    // UPrimitiveComponent::WasRecentlyRendered(float Tolerance) -> bool.
    void* fnWasRecentlyRendered_ = nullptr;

    std::vector<MarkedPrimitive> marked_;
    std::unordered_map<uint8_t, MarkedPrimitive> slotTable_;
    // Two DIFFERENT sets, which were one set until the visibility sweep made the
    // distinction matter.
    //
    // `pooled_`       -- component is already in `pending_`; stops repeated
    //                    collection passes queueing it twice as the level
    //                    streams in.
    // `alreadyMarked_`-- component currently holds a stencil slot.
    //
    // Sharing one set for both meant "queued" implied "marked", so once
    // collection had run, the visibility sweep skipped every candidate as
    // already marked and could never mark anything at all.
    std::unordered_set<void*> pooled_;
    std::unordered_set<void*> alreadyMarked_;

    // Candidates found by CollectCandidates, waiting to be marked on the game
    // thread. Guarded because collection runs off-thread from marking.
    struct Candidate {
        void* component;
        int32_t index;
        int32_t serial;
        std::string className;
        std::string name;
    };
    // The candidate POOL, not a queue.
    //
    // Candidates are no longer consumed when scanned. A primitive that is off
    // screen now may be the subject of the shot ten seconds from now, so
    // discarding it on first inspection would mean it could never be labelled.
    // `scanCursor_` sweeps the pool round-robin, which also spreads the cost of
    // visibility testing evenly instead of hammering the same prefix.
    std::vector<Candidate> pending_;
    mutable std::mutex pendingMutex_;
    size_t scanCursor_ = 0;
    size_t refreshCursor_ = 0;

    // Slots are LEASED through the registry rather than handed out by a counter.
    // A counter gives every object a different number each pass and reuses
    // numbers with no record of what they meant -- the mask becomes undecodable.
    // The registry keeps (pointer, serial) -> stable id, so a repeated slot is
    // still resolvable through the frame's sidecar.
    IdentityRegistry identity_;
    mutable std::mutex identityMutex_;
    uint64_t markPass_ = 0;

    uint64_t writesAttempted_ = 0;
    uint64_t writesVerified_ = 0;
    uint64_t writesRejected_ = 0;
    // How many times the engine's setter actually flipped the bit. Distinguishes
    // "the call ran and the pass is disabled" from "the call did nothing",
    // which need entirely different fixes.
    uint64_t setterEffective_ = 0;

    // Visibility accounting. The ratio of tested to visible is the interesting
    // number: it says how much of the level is on screen at once, and therefore
    // whether 255 slots is a real constraint or a comfortable one.
    uint64_t visibilityTested_ = 0;
    uint64_t visibilityHits_ = 0;
    uint64_t slotsReleased_ = 0;
    size_t slotsFreedForCoverage_ = 0;
    // Slots currently held by instanced (environmental) components, capped so
    // the budget cannot be consumed by ground, foliage and building batches.
    size_t instancedLeased_ = 0;
    // The slot currently under ground-truth measurement, or 0. Exempt from the
    // coverage evictor below and from nothing else -- see RefreshVisibility.
    // Atomic because it is set and read on the game thread but cleared on the
    // render thread when the verdict is recorded.
    std::atomic<uint8_t> groundTruthPin_{0};

    // ---- distance gating ---------------------------------------------------
    // USceneComponent::K2_GetComponentLocation. Asking the engine for a world
    // position rather than reading FTransform ourselves: the component may be
    // attached several levels deep, so RelativeLocation is the wrong number, and
    // UE5's Large World Coordinates make FVector three DOUBLES where UE4 has
    // three floats -- read at the wrong width that returns plausible nonsense
    // instead of failing.
    void* fnGetComponentLocation_ = nullptr;
    // 12 or 24, MEASURED from the UFunction's own return parameter. Never
    // assumed per engine version.
    int32_t locationSize_ = 0;
    // Must be called on the GAME THREAD -- it invokes a UFunction.
    bool GetWorldLocation(void* component, double out[3]);

    // The point distances are measured from: the character on screen. Chosen on
    // the render thread by mask area (a slot number, not a pointer -- the
    // component behind it can be destroyed between threads), resolved to a
    // location on the game thread.
    // Is the object holding this slot a character? Written on the game thread
    // when a slot is leased or released, read on the render thread when the mask
    // arrives. A plain byte array rather than a lookup into slotTable_, because
    // that map is game-thread state and the mask callback is not on it.
    uint8_t slotIsCharacter_[256] = {};
    std::atomic<uint8_t> anchorSlot_{0};
    double anchor_[3] = {0, 0, 0};
    bool anchorValid_ = false;
    double markRadius_ = 0.0;
    size_t skippedFar_ = 0;
    size_t releasedFar_ = 0;
    // Last values actually printed, so the gate line is emitted on change rather
    // than every pass.
    size_t lastLoggedSkippedFar_ = 0;
    size_t lastLoggedReleasedFar_ = 0;
    bool lastLoggedAnchorValid_ = false;
    // Consecutive masks in which a slot rendered below the area threshold.
    // A streak, not a single frame: an object can legitimately be occluded for
    // a moment, and evicting on one bad frame is thrash.
    uint8_t tinyStreak_[256] = {};
    uint64_t staleDropped_ = 0;   // marked objects that were destroyed under us

    // Rebuilt and republished whenever the slot table changes. Guarded by
    // identityMutex_ because the render thread reads it while the marking
    // thread replaces it.
    void PublishSidecar();
    std::shared_ptr<const FrameSidecar> published_;
};

// The process-wide marker instance, defined in dllmain.cpp.
//
// Declared here rather than with a block-scope `extern` at the use site: a
// block-scope extern resolves against the GLOBAL namespace, so it silently
// referred to ::GetMarker and failed at link time with a mangled name that took
// a moment to read.
CustomDepthMarker& GetMarker();

}  // namespace segcap
