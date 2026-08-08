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
#include <string>
#include <unordered_map>
#include <vector>

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

    // Marks up to `limit` primitives, assigning stencil values from 1 upward.
    // Runs on the game thread -- caller must already be there.
    //
    // `renderableOnly` skips collision volumes and editor gizmos. Stray has
    // ~2235 BoxComponents, 577 CapsuleComponents, 613 ArrowComponents and 480
    // BillboardComponents that render nothing at all; marking them would spend
    // stencil slots on objects that produce no pixels, and there are only 255.
    int MarkPrimitives(ue4::Engine& engine, int limit, bool renderableOnly = true);

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

    uint64_t writesAttempted_ = 0;
    uint64_t writesVerified_ = 0;
    uint64_t writesRejected_ = 0;
};

}  // namespace segcap
