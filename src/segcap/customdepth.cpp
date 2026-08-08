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

    resolved_ = true;
    return true;
}

bool CustomDepthMarker::WriteMarked(void* component, uint8_t stencilValue,
                                    MarkedPrimitive& out) {
    ++writesAttempted_;

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

    // 255 is a hard ceiling: the stencil channel is 8 bits and 0 means
    // "unmarked". Anything above that would wrap and silently alias two objects
    // to one id.
    limit = std::min(limit, 255);

    int assigned = 0;
    const int32_t total = engine.NumObjects();

    for (int32_t i = 0; i < total && assigned < limit; ++i) {
        ue4::ObjectRef ref;
        if (!engine.GetObject(i, ref)) continue;
        if (renderableOnly && !IsRenderableComponentClass(ref.className)) continue;
        if (!renderableOnly && !engine.IsDerivedFrom(ref.object, "PrimitiveComponent")) continue;

        // Skip class-default objects: they are templates, not things in the
        // world, and marking them would affect every future instance.
        if (ref.name.rfind("Default__", 0) == 0) continue;

        const uint8_t slot = static_cast<uint8_t>(assigned + 1);   // 0 = unmarked
        MarkedPrimitive mp;
        mp.component = ref.object;
        mp.objectIndex = ref.index;
        mp.serialNumber = ref.serialNumber;
        mp.className = ref.className;
        mp.stencilValue = slot;

        if (!WriteMarked(ref.object, slot, mp)) continue;

        marked_.push_back(mp);
        slotTable_[slot] = mp;
        ++assigned;

        if (assigned <= 8) {
            LogInfo("customdepth:   slot %3u -> %s (%s) idx=%d",
                    slot, ref.name.c_str(), ref.className.c_str(), ref.index);
        }
    }

    LogInfo("customdepth: marked %d primitives "
            "(attempted %llu, verified %llu, rejected %llu)",
            assigned, writesAttempted_, writesVerified_, writesRejected_);
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
