#include "customdepth.h"

#include <windows.h>

#include <algorithm>

#include "log.h"

namespace segcap {
namespace {

// Component classes that actually render. Stray's 38,479 PrimitiveComponent
// descendants include ~4,300 that draw nothing -- BoxComponent (2235),
// CapsuleComponent (577), ArrowComponent (613), BillboardComponent (480),
// SphereComponent (296), DrawSphereComponent (118). Those are collision volumes
// and editor gizmos. Marking them would consume stencil slots for objects that
// produce no pixels, and the channel only has 255.
bool IsRenderableComponentClass(const std::string& name) {
    static const char* kRenderable[] = {
        "StaticMeshComponent",
        "SkeletalMeshComponent",
        "InstancedStaticMeshComponent",
        "HierarchicalInstancedStaticMeshComponent",
        "SplineMeshComponent",
        "ProceduralMeshComponent",
        "ToyoSplineMeshComponent",   // Stray's own; "Toyo" is its internal prefix
        "ToyoJointMeshComponent",
        "SplineRailComponent",
        "TextRenderComponent",
    };
    for (const char* r : kRenderable) {
        if (name == r) return true;
    }
    return false;
}

}  // namespace

bool CustomDepthMarker::Resolve(ue4::Engine& engine) {
    if (resolved_) return true;

    void* primClass = engine.FindClass("PrimitiveComponent");
    if (!primClass) {
        LogError("customdepth: UPrimitiveComponent class not found");
        return false;
    }

    propRenderCustomDepth_ = engine.FindProperty(primClass, "bRenderCustomDepth");
    propStencilValue_ = engine.FindProperty(primClass, "CustomDepthStencilValue");

    if (!propRenderCustomDepth_.valid()) {
        LogError("customdepth: bRenderCustomDepth not found -- refusing to write blind");
        return false;
    }
    if (!propStencilValue_.valid()) {
        LogError("customdepth: CustomDepthStencilValue not found -- refusing to write blind");
        return false;
    }
    if (!propRenderCustomDepth_.isBool) {
        LogError("customdepth: bRenderCustomDepth is [%s], expected BoolProperty",
                 propRenderCustomDepth_.type.c_str());
        return false;
    }

    LogInfo("customdepth: bRenderCustomDepth byte +0x%X mask 0x%02X%s",
            propRenderCustomDepth_.offset + propRenderCustomDepth_.byteOffset,
            propRenderCustomDepth_.fieldMask,
            propRenderCustomDepth_.isPackedBit() ? " (packed bit)" : "");
    LogInfo("customdepth: CustomDepthStencilValue +0x%X size %d",
            propStencilValue_.offset, propStencilValue_.size);

    // The engine's setters, which do the side effects a raw write cannot.
    fnSetRenderCustomDepth_ = engine.FindFunction(primClass, "SetRenderCustomDepth");
    fnSetCustomDepthStencilValue_ =
        engine.FindFunction(primClass, "SetCustomDepthStencilValue");

    LogInfo("customdepth: SetRenderCustomDepth       UFunction = %p",
            fnSetRenderCustomDepth_);
    LogInfo("customdepth: SetCustomDepthStencilValue UFunction = %p",
            fnSetCustomDepthStencilValue_);

    if (!fnSetRenderCustomDepth_ || !fnSetCustomDepthStencilValue_) {
        LogWarn("customdepth: setter UFunction(s) not found -- will fall back to a "
                "raw property write, which sets the flag but leaves the render "
                "proxy stale, so nothing will actually render");
    }

    resolved_ = true;
    return true;
}

bool CustomDepthMarker::WriteMarked(void* component, uint8_t stencilValue,
                                    MarkedPrimitive& out) {
    ++writesAttempted_;

    // Prefer the engine's own setters.
    //
    // Writing bRenderCustomDepth directly is necessary but NOT SUFFICIENT: the
    // renderer does not read that property per frame, it reads a
    // FPrimitiveSceneProxy built on the render thread.
    // UPrimitiveComponent::SetRenderCustomDepth sets the flag AND calls
    // MarkRenderStateDirty(), which is what rebuilds the proxy.
    //
    // The first attempt at this marked 64 primitives with 64 verified writes and
    // the CustomDepth target still showed binds=0 -- the game-thread property
    // changed and the render thread never found out.
    auto* preBase = reinterpret_cast<uint8_t*>(component);
    const size_t preBoolOff =
        static_cast<size_t>(propRenderCustomDepth_.offset) + propRenderCustomDepth_.byteOffset;
    const uint8_t beforeCall =
        ue4::IsReadable(preBase + preBoolOff, 1) ? *(preBase + preBoolOff) : 0;

    if (fnSetRenderCustomDepth_ && fnSetCustomDepthStencilValue_) {
        auto& pe = ue4::GetProcessEventHook();

        // UFunction parameter structs are just the arguments laid out in order.
        // A single bool is passed as one byte.
        struct { uint32_t bValue; } depthParams = {1};
        pe.CallFunction(component, fnSetRenderCustomDepth_, &depthParams);

        struct { int32_t Value; } stencilParams = {static_cast<int32_t>(stencilValue)};
        pe.CallFunction(component, fnSetCustomDepthStencilValue_, &stencilParams);

        // Did the engine call actually do anything? Checked BEFORE our own
        // fallback write, so the two cannot be confused. The previous run could
        // not distinguish "the setter ran and the pass is off" from "the setter
        // silently did nothing" -- and those need completely different fixes.
        const uint8_t afterCall =
            ue4::IsReadable(preBase + preBoolOff, 1) ? *(preBase + preBoolOff) : 0;
        const bool bitNowSet = (afterCall & propRenderCustomDepth_.fieldMask) != 0;
        if (setterEffective_ == 0) {
            LogInfo("customdepth: setter check -- byte 0x%02X -> 0x%02X, bit %s",
                    beforeCall, afterCall, bitNowSet ? "SET by the engine call"
                                                     : "NOT set (call had no effect)");
        }
        setterEffective_ += bitNowSet ? 1 : 0;
    }

    auto* base = reinterpret_cast<uint8_t*>(component);
    const size_t boolByteOff =
        static_cast<size_t>(propRenderCustomDepth_.offset) + propRenderCustomDepth_.byteOffset;
    const uint8_t mask = propRenderCustomDepth_.fieldMask;

    uint8_t* boolByte = base + boolByteOff;
    auto* stencilField = reinterpret_cast<int32_t*>(base + propStencilValue_.offset);

    if (!ue4::IsReadable(boolByte, 1) || !ue4::IsReadable(stencilField, 4)) {
        ++writesRejected_;
        return false;
    }

    out.originalByte = *boolByte;
    out.originalStencil = *stencilField;

    // Read-modify-write on the bit only. Writing the whole byte would clobber
    // the other bools packed into it.
    const uint8_t desired = static_cast<uint8_t>((out.originalByte & ~mask) | mask);
    *boolByte = desired;
    *stencilField = static_cast<int32_t>(stencilValue);

    // Verify: read back and confirm ONLY the masked bit changed. This is a
    // cheap, direct check on the exact failure mode the packed-bit discovery
    // warned about -- and it makes "we did not change what the player sees" a
    // measurement rather than a claim.
    const uint8_t after = *boolByte;
    const uint8_t changed = static_cast<uint8_t>(after ^ out.originalByte);
    if ((changed & ~mask) != 0) {
        LogError("customdepth: write touched bits outside the mask "
                 "(before 0x%02X after 0x%02X mask 0x%02X) -- reverting",
                 out.originalByte, after, mask);
        *boolByte = out.originalByte;
        *stencilField = out.originalStencil;
        ++writesRejected_;
        return false;
    }
    if (*stencilField != static_cast<int32_t>(stencilValue)) {
        LogError("customdepth: stencil value did not take (wanted %u got %d) -- reverting",
                 stencilValue, *stencilField);
        *boolByte = out.originalByte;
        *stencilField = out.originalStencil;
        ++writesRejected_;
        return false;
    }

    ++writesVerified_;
    return true;
}

int CustomDepthMarker::MarkPrimitives(ue4::Engine& engine, int limit, bool renderableOnly) {
    if (!resolved_) {
        LogError("customdepth: Resolve() must succeed first");
        return 0;
    }

    // limit <= 0 means "mark everything", cycling slot ids 1..255.
    //
    // Marking a bounded first-N is the wrong experiment for a first proof. The
    // first 64 renderable components by array index turned out to be a poor
    // sample -- they may sit in unloaded sublevels, be unregistered with the
    // scene, or simply be off-camera, in which case CustomDepth renders nothing
    // no matter how correct the flag is. Marking everything removes visibility
    // as a variable, so a still-empty mask points squarely at the pass being
    // disabled or the elected target being the wrong buffer.
    //
    // Slot ids necessarily repeat past 255: the stencil channel is 8 bits and 0
    // means unmarked. That is fine here because this run is a yes/no test of
    // whether ANYTHING renders. Turning repeated slots back into distinct
    // identities is task 9's problem, and the reason a per-frame sidecar table
    // exists at all.
    // Refresh the readable-memory map first. This is the SIXTH bug caused by
    // that snapshot going stale: ReportSample and CountDerivedFrom both refresh,
    // MarkPrimitives did not, so GetObject rejected most of the array and only
    // 267 of ~29,000 renderable components were ever marked.
    engine.RefreshMemoryMap();

    const bool markAll = limit <= 0;
    if (!markAll) limit = std::min(limit, 255);

    // Bound the work per pass. This runs inside ProcessEvent on the game thread,
    // and each mark issues two UFunction calls -- walking 29,000 objects in one
    // go would stall the engine's dispatch and hitch the game, which is exactly
    // the "not breaking the game" failure we are trying to avoid. Marking
    // repeats every 30s as the level streams, so a bounded pass still converges.
    constexpr int kMaxPerPass = 3000;

    int assigned = 0;
    int examined = 0;
    const int32_t total = engine.NumObjects();

    for (int32_t i = markResumeIndex_; i < total && (markAll || assigned < limit); ++i) {
        // Resume where the previous pass stopped, so successive passes sweep the
        // whole array rather than re-walking the same prefix each time.
        markResumeIndex_ = i;
        if (++examined > kMaxPerPass) break;

        ue4::ObjectRef ref;
        if (!engine.GetObject(i, ref)) continue;
        if (renderableOnly && !IsRenderableComponentClass(ref.className)) continue;
        if (!renderableOnly && !engine.IsDerivedFrom(ref.object, "PrimitiveComponent")) continue;

        // Skip class-default objects: they are templates, not things in the
        // world, and marking them would affect every future instance.
        if (ref.name.rfind("Default__", 0) == 0) continue;

        // Already marked on a previous pass. Re-marking is cheap but would
        // reassign a different slot to the same object every pass, which would
        // make identities unstable across frames for no reason.
        if (alreadyMarked_.count(ref.object)) continue;

        const uint8_t slot = static_cast<uint8_t>((assigned % 255) + 1);  // 0 = unmarked
        MarkedPrimitive mp;
        mp.component = ref.object;
        mp.objectIndex = ref.index;
        mp.serialNumber = ref.serialNumber;
        mp.className = ref.className;
        mp.stencilValue = slot;

        if (!WriteMarked(ref.object, slot, mp)) continue;

        marked_.push_back(mp);
        slotTable_[slot] = mp;
        alreadyMarked_.insert(ref.object);
        ++assigned;

        if (assigned <= 8) {
            LogInfo("customdepth:   slot %3u -> %s (%s) idx=%d",
                    slot, ref.name.c_str(), ref.className.c_str(), ref.index);
        }
    }

    LogInfo("customdepth: marked %d primitives "
            "(attempted %llu, verified %llu, rejected %llu)",
            assigned, writesAttempted_, writesVerified_, writesRejected_);
    LogInfo("customdepth: engine setter flipped the bit on %llu/%llu -- %s",
            setterEffective_, writesAttempted_,
            setterEffective_ > 0 ? "the UFunction calls ARE taking effect"
                                 : "the UFunction calls are doing NOTHING");
    return assigned;
}

int CustomDepthMarker::RestoreAll() {
    int restored = 0;
    for (const MarkedPrimitive& mp : marked_) {
        auto* base = reinterpret_cast<uint8_t*>(mp.component);
        const size_t boolByteOff = static_cast<size_t>(propRenderCustomDepth_.offset) +
                                   propRenderCustomDepth_.byteOffset;
        if (!ue4::IsReadable(base + boolByteOff, 1)) continue;
        *(base + boolByteOff) = mp.originalByte;
        *reinterpret_cast<int32_t*>(base + propStencilValue_.offset) = mp.originalStencil;
        ++restored;
    }
    LogInfo("customdepth: restored %d/%zu primitives", restored, marked_.size());
    marked_.clear();
    slotTable_.clear();
    return restored;
}

}  // namespace segcap
