#include "customdepth.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

// The list above is Stray's. "ToyoSplineMeshComponent" is literally that game's
// internal prefix, and a title that names its mesh components anything else --
// which is every other title -- would match nothing and mark nothing.
//
// So ask the class hierarchy instead of the class name: anything descending
// from UMeshComponent renders geometry. The name list stays as an additional
// accept, because a few renderables (TextRenderComponent, and Stray's
// SplineRailComponent) derive from UPrimitiveComponent directly rather than
// from UMeshComponent, and dropping them would be a silent regression on the
// one game where this is known to work.
//
// Memoised on the class NAME, not the object. A gameplay array is ~500k
// objects across a few hundred distinct classes, so this turns one chain walk
// per object into one per class.
bool IsRenderableComponent(ue4::Engine& engine, const ue4::ObjectRef& ref) {
    if (IsRenderableComponentClass(ref.className)) return true;

    static std::mutex mutex;
    static std::unordered_map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(ref.className);
        if (it != cache.end()) return it->second;
    }
    const bool derived = engine.IsDerivedFrom(ref.object, "MeshComponent");
    {
        std::lock_guard<std::mutex> lock(mutex);
        cache[ref.className] = derived;
    }
    return derived;
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

    // Visibility oracle. Without it the marker can still run, but it spends its
    // 255 slots on arbitrary primitives, which in a real level means labelling
    // almost nothing that is on screen.
    fnWasRecentlyRendered_ = engine.FindFunction(primClass, "WasRecentlyRendered");
    LogInfo("customdepth: WasRecentlyRendered        UFunction = %p",
            fnWasRecentlyRendered_);
    if (!fnWasRecentlyRendered_) {
        LogWarn("customdepth: WasRecentlyRendered not found -- slots will be leased "
                "without a visibility test, which in a level with tens of thousands "
                "of primitives will label mostly off-screen objects");
    }

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

bool CustomDepthMarker::WasRecentlyRendered(void* component, float toleranceSeconds) {
    // No oracle available: treat everything as visible rather than as invisible.
    // Marking too broadly degrades to the old behaviour; marking nothing would
    // produce an empty mask and look like a completely different failure.
    if (!fnWasRecentlyRendered_) return true;

    // UFunction parameters are the declared arguments laid out in order,
    // followed by the return value. For
    //   bool WasRecentlyRendered(float Tolerance)
    // that is a 4-byte float then a 1-byte bool at offset 4. The struct is
    // zeroed first so a call that writes nothing reads back as false rather
    // than as whatever was on the stack -- an uninitialised "true" here would
    // silently reproduce the exact bug this function exists to fix.
    struct Params {
        float Tolerance;
        uint8_t ReturnValue;
        uint8_t pad[3];
    } params = {};
    params.Tolerance = toleranceSeconds;

    ue4::GetProcessEventHook().CallFunction(component, fnWasRecentlyRendered_, &params);

    ++visibilityTested_;
    const bool visible = params.ReturnValue != 0;
    if (visible) ++visibilityHits_;
    return visible;
}

bool CustomDepthMarker::UnmarkPrimitive(const MarkedPrimitive& mp) {
    if (!mp.component) return false;

    // Use the engine's setter for the same reason marking does: clearing the
    // property without MarkRenderStateDirty() leaves the render proxy still
    // opted in, so the primitive would keep writing its old stencil value into
    // the mask after we had already leased that slot to something else. That
    // failure would show up as two objects sharing one id -- the single most
    // confusing thing a segmentation mask can do.
    if (fnSetRenderCustomDepth_) {
        struct { uint32_t bValue; } off = {0};
        ue4::GetProcessEventHook().CallFunction(mp.component, fnSetRenderCustomDepth_, &off);
    }

    // Restore the exact original bytes as well, so the primitive is left byte
    // identical to how we found it.
    auto* base = reinterpret_cast<uint8_t*>(mp.component);
    const size_t boolByteOff =
        static_cast<size_t>(propRenderCustomDepth_.offset) + propRenderCustomDepth_.byteOffset;
    uint8_t* boolByte = base + boolByteOff;
    auto* stencilField = reinterpret_cast<int32_t*>(base + propStencilValue_.offset);
    if (!ue4::IsReadable(boolByte, 1) || !ue4::IsReadable(stencilField, 4)) return false;

    *boolByte = mp.originalByte;
    *stencilField = mp.originalStencil;
    return true;
}

int CustomDepthMarker::RefreshVisibility(ue4::Engine& engine, int limit) {
    (void)engine;
    if (!resolved_ || !fnWasRecentlyRendered_) return 0;
    if (marked_.empty()) return 0;

    int released = 0;
    const size_t n = std::min(static_cast<size_t>(limit > 0 ? limit : 0), marked_.size());

    for (size_t i = 0; i < n; ++i) {
        if (refreshCursor_ >= marked_.size()) refreshCursor_ = 0;
        const MarkedPrimitive mp = marked_[refreshCursor_];

        // A tolerance wider than the one used for acquisition, on purpose.
        // Equal thresholds make an object hovering at the edge of visibility
        // acquire and release a slot every pass, which is thrash by another
        // name. The gap is hysteresis.
        if (WasRecentlyRendered(mp.component, 1.0f)) {
            std::lock_guard<std::mutex> lock(identityMutex_);
            identity_.Touch(mp.stencilValue, markPass_);
            ++refreshCursor_;
            continue;
        }

        UnmarkPrimitive(mp);
        {
            std::lock_guard<std::mutex> lock(identityMutex_);
            identity_.ReleaseSlot(mp.stencilValue);
        }
        slotTable_.erase(mp.stencilValue);
        alreadyMarked_.erase(mp.component);
        marked_.erase(marked_.begin() + static_cast<ptrdiff_t>(refreshCursor_));
        ++released;
        ++slotsReleased_;
        // Do not advance: the erase already shifted the next element here.
    }
    if (released > 0) PublishSidecar();
    return released;
}

int CustomDepthMarker::CollectCandidates(ue4::Engine& engine, bool renderableOnly) {
    if (!resolved_) {
        LogError("customdepth: Resolve() must succeed first");
        return 0;
    }

    // Refresh the readable-memory map first. Sixth bug caused by that snapshot
    // going stale: GetObject rejects most of the array without it.
    engine.RefreshMemoryMap();

    // Walk the WHOLE array. This is read-only CPU work with no engine calls, so
    // it does not need bounding -- and bounding it to 3000 per pass swept only
    // 4% of ~350,000 slots, finding 16 primitives where a full sweep found 267.
    // The low indices are almost all bootstrap objects (Class, Package,
    // Function); renderable components live much further in.
    int found = 0;
    const int32_t total = engine.NumObjects();

    // What the renderable filter threw away, by class. A filter that can hide
    // evidence needs a log line upstream of itself -- that rule has been
    // learned the hard way three times in this project (a format bucket that
    // hid 800 integer targets, an election filter that hid every depth target,
    // a census that hid its own scan). Without this, "marked 0" on a new title
    // is indistinguishable between "nothing renderable exists" and "my
    // allowlist does not know this game's class names".
    std::unordered_map<std::string, int> rejectedByClass;

    for (int32_t i = 0; i < total; ++i) {
        ue4::ObjectRef ref;
        if (!engine.GetObject(i, ref)) continue;
        if (renderableOnly && !IsRenderableComponent(engine, ref)) {
            // Only *Component classes are worth reporting; the array is mostly
            // Function/Class/Package metadata and listing that is noise.
            const std::string& cn = ref.className;
            if (cn.size() > 9 && cn.compare(cn.size() - 9, 9, "Component") == 0) {
                ++rejectedByClass[cn];
            }
            continue;
        }
        if (!renderableOnly && !engine.IsDerivedFrom(ref.object, "PrimitiveComponent")) continue;

        // Class-default objects are templates, not things in the world; marking
        // one would affect every future instance.
        if (ref.name.rfind("Default__", 0) == 0) continue;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pooled_.count(ref.object)) continue;
            // Reserve immediately so a later collection pass cannot queue the
            // same object twice while this batch is still pending.
            pooled_.insert(ref.object);
            pending_.push_back({ref.object, ref.index, ref.serialNumber,
                                ref.className, ref.name});
        }
        ++found;
    }

    LogInfo("customdepth: collected %d new candidates (%zu pending, %zu marked)",
            found, pending_.size(), marked_.size());

    // Report the discards once, on the first pass that finds almost nothing.
    // That is the case where the answer matters and the one where the log is
    // otherwise silent.
    if (found < 8 && !reportedRejects_ && !rejectedByClass.empty()) {
        reportedRejects_ = true;
        std::vector<std::pair<std::string, int>> top(rejectedByClass.begin(),
                                                     rejectedByClass.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        LogWarn("customdepth: only %d renderable candidates -- these *Component "
                "classes were REJECTED as non-renderable:", found);
        for (size_t i = 0; i < top.size() && i < 15; ++i) {
            LogWarn("customdepth:   %6d  %s", top[i].second, top[i].first.c_str());
        }
    }
    return found;
}

int CustomDepthMarker::MarkBatch(ue4::Engine& engine, int limit) {
    (void)engine;
    if (!resolved_) return 0;

    // Take only as many candidates as there are FREE slots.
    //
    // This bound is the difference between a stable mask and a strobing one.
    // Without it, every batch pulled another `limit` candidates and leased them
    // slots, evicting whatever held those slots before -- measured at 9,000
    // identities and 8,745 evictions in 150 seconds, with the log cheerfully
    // reporting "marked 250 this batch" each time as though it were progress.
    //
    // The arithmetic was never survivable: the level has ~38,000 markable
    // primitives and the stencil channel has 255 slots. Cycling through them
    // does not label more of the scene, it just guarantees no object keeps an
    // id long enough to be tracked across two frames -- which destroys the one
    // property the identity registry exists to provide.
    //
    // So the working set is capped by the channel width: fill the slots, then
    // hold them.
    size_t freeSlots = 0;
    {
        std::lock_guard<std::mutex> lock(identityMutex_);
        const size_t live = identity_.liveSlots();
        freeSlots = IdentityRegistry::kSlotCount > live
                        ? IdentityRegistry::kSlotCount - live
                        : 0;
    }
    if (freeSlots == 0) return 0;

    // Sweep the candidate POOL round-robin, testing visibility as we go, and
    // stop as soon as the free slots are spent.
    //
    // Candidates are not removed from the pool. An object that is off screen on
    // this sweep may be the subject of the shot on the next one, and a queue
    // that consumes what it inspects could never come back to it.
    std::vector<Candidate> batch;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pending_.empty()) return 0;
        const size_t scan = std::min(static_cast<size_t>(limit > 0 ? limit : 0), pending_.size());
        batch.reserve(scan);
        for (size_t i = 0; i < scan; ++i) {
            if (scanCursor_ >= pending_.size()) scanCursor_ = 0;
            batch.push_back(pending_[scanCursor_]);
            ++scanCursor_;
        }
    }
    if (batch.empty()) return 0;

    ++markPass_;
    int assigned = 0;
    int refused = 0;
    int skippedInvisible = 0;
    int skippedAlready = 0;

    for (const Candidate& c : batch) {
        if (freeSlots == 0) break;

        // Already holds a slot: nothing to do, and re-leasing would only churn.
        if (alreadyMarked_.find(c.component) != alreadyMarked_.end()) {
            ++skippedAlready;
            continue;
        }

        // The visibility gate. Tested BEFORE leasing, because a slot spent on
        // an off-screen primitive is a slot that cannot label a visible one,
        // and there are only 255 of them against ~32,800 candidates.
        if (!WasRecentlyRendered(c.component, 0.3f)) {
            ++skippedInvisible;
            continue;
        }

        // Lease a slot through the registry rather than handing out a counter.
        //
        // The registry assigns a stable 64-bit id keyed on (pointer, serial) and
        // leases one of the 255 stencil slots to it, evicting LRU when they run
        // out -- but never evicting something seen this pass. A counter would
        // give the same object a different number every pass and reuse numbers
        // with no record of what they meant, making the mask undecodable.
        uint8_t slot = 0;
        {
            std::lock_guard<std::mutex> lock(identityMutex_);
            slot = identity_.LeaseSlot(c.component, c.serial, c.className, c.name, markPass_);
        }
        if (slot == 0) {
            // All 255 slots are held by objects seen this pass. Refusing is
            // correct: stealing one would thrash two labels and ruin both.
            ++refused;
            continue;
        }

        MarkedPrimitive mp;
        mp.component = c.component;
        mp.objectIndex = c.index;
        mp.serialNumber = c.serial;
        mp.className = c.className;
        mp.stencilValue = slot;

        if (!WriteMarked(c.component, slot, mp)) {
            // The write failed, so the lease must be given back or the slot is
            // burned for the rest of the session on an object that is not
            // actually marked.
            std::lock_guard<std::mutex> lock(identityMutex_);
            identity_.ReleaseSlot(slot);
            continue;
        }

        marked_.push_back(mp);
        slotTable_[slot] = mp;
        alreadyMarked_.insert(c.component);
        --freeSlots;
        ++assigned;

        if (marked_.size() <= 6) {
            LogInfo("customdepth:   slot %3u -> %s (%s) idx=%d",
                    slot, c.name.c_str(), c.className.c_str(), c.index);
        }
    }

    if (assigned > 0) PublishSidecar();

    // Only log when something happened. The sweep runs several times a second
    // and a line per sweep buried the interesting events under thousands of
    // "marked 0" entries last time.
    if (assigned > 0 || refused > 0) {
        std::lock_guard<std::mutex> lock(identityMutex_);
        LogInfo("customdepth: marked %d (refused %d, invisible %d, already %d of %zu scanned); "
                "%zu live slots, %llu identities, %llu evictions, visible %llu/%llu tested",
                assigned, refused, skippedInvisible, skippedAlready, batch.size(),
                identity_.liveSlots(), identity_.totalIdentities(), identity_.evictions(),
                visibilityHits_, visibilityTested_);
    }
    return assigned;
}

void CustomDepthMarker::PublishSidecar() {
    // Width/height/frameIndex are stamped at write time, not here: the table is
    // a property of the marking state, not of any particular frame.
    auto sc = std::make_shared<FrameSidecar>(SnapshotSidecar(0, 0, 0));
    std::lock_guard<std::mutex> lock(identityMutex_);
    published_ = std::move(sc);
}

std::shared_ptr<const FrameSidecar> CustomDepthMarker::publishedSidecar() const {
    std::lock_guard<std::mutex> lock(identityMutex_);
    return published_;
}

FrameSidecar CustomDepthMarker::SnapshotSidecar(uint64_t frameIndex, uint32_t w,
                                                uint32_t h) const {
    std::lock_guard<std::mutex> lock(identityMutex_);
    return identity_.BuildSidecar(frameIndex, w, h);
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
