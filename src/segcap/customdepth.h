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

#include <cstdint>
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

    // Marks up to `limit` pending candidates. MUST run on the game thread.
    // Bounded because each mark issues two UFunction calls inside ProcessEvent,
    // and a large batch would stall the engine's dispatch.
    int MarkBatch(ue4::Engine& engine, int limit);

    size_t pendingCount() const { return pending_.size(); }

    // Thread-safe snapshot of the current slot table, for emitting alongside a
    // captured mask. Marking runs on the game thread; mask dumps happen on the
    // render thread, so the table cannot simply be read across.
    FrameSidecar SnapshotSidecar(uint64_t frameIndex, uint32_t w, uint32_t h) const;

    IdentityRegistry& identity() { return identity_; }

    // Restores every marked primitive to its recorded original bytes.
    int RestoreAll();

    const std::vector<MarkedPrimitive>& marked() const { return marked_; }

    // Slot -> object identity, for the per-frame sidecar table. The 8-bit
    // stencil value is a lease, not an identity: it only means something joined
    // against this map.
    const std::unordered_map<uint8_t, MarkedPrimitive>& slotTable() const {
        return slotTable_;
    }

private:
    bool WriteMarked(void* component, uint8_t stencilValue, MarkedPrimitive& out);

    bool resolved_ = false;
    ue4::PropertyInfo propRenderCustomDepth_;
    ue4::PropertyInfo propStencilValue_;

    // The engine's own setters. Calling these rather than writing the property
    // is what triggers MarkRenderStateDirty() and rebuilds the render-thread
    // proxy -- without it the renderer never learns the flag changed.
    void* fnSetRenderCustomDepth_ = nullptr;
    void* fnSetCustomDepthStencilValue_ = nullptr;

    std::vector<MarkedPrimitive> marked_;
    std::unordered_map<uint8_t, MarkedPrimitive> slotTable_;
    // Marking runs repeatedly as the level streams in. Without this, each pass
    // would reassign a different slot to the same object, making identities
    // unstable across frames for no reason.
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
    std::vector<Candidate> pending_;
    mutable std::mutex pendingMutex_;

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
};

// The process-wide marker instance, defined in dllmain.cpp.
//
// Declared here rather than with a block-scope `extern` at the use site: a
// block-scope extern resolves against the GLOBAL namespace, so it silently
// referred to ::GetMarker and failed at link time with a mangled name that took
// a moment to read.
CustomDepthMarker& GetMarker();

}  // namespace segcap
