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

    // World position, for the distance gate. Declared on USceneComponent, which
    // UPrimitiveComponent derives from -- try the derived class first in case
    // FindFunction does not walk the super chain, then the base explicitly.
    fnGetComponentLocation_ = engine.FindFunction(primClass, "K2_GetComponentLocation");
    if (!fnGetComponentLocation_) {
        if (void* sceneClass = engine.FindClass("SceneComponent")) {
            fnGetComponentLocation_ = engine.FindFunction(sceneClass,
                                                          "K2_GetComponentLocation");
        }
    }
    if (fnGetComponentLocation_) {
        // MEASURE the return width. A UFunction is a UStruct, so its parameters
        // are its properties; the return value is the one marked ReturnValue.
        // UE4 gives 12 bytes (three floats), UE5 with Large World Coordinates
        // gives 24 (three doubles). Assuming either one silently produces
        // coordinates that look like coordinates and are not, which is the
        // failure this project keeps paying for.
        for (const auto& p : engine.ListProperties(fnGetComponentLocation_, false)) {
            if (p.name == "ReturnValue") { locationSize_ = p.size; break; }
        }
        LogInfo("customdepth: K2_GetComponentLocation UFunction = %p, returns %d bytes "
                "(%s)", fnGetComponentLocation_, locationSize_,
                locationSize_ == 24  ? "3 doubles -- UE5 Large World Coordinates"
                : locationSize_ == 12 ? "3 floats -- UE4"
                                      : "UNRECOGNISED, distance gate disabled");
        if (locationSize_ != 12 && locationSize_ != 24) fnGetComponentLocation_ = nullptr;
    } else {
        LogWarn("customdepth: K2_GetComponentLocation not found -- no distance gate, "
                "slots will go to whatever the engine drew anywhere");
    }

    resolved_ = true;
    return true;
}

// Ask the engine where this component is. GAME THREAD ONLY.
bool CustomDepthMarker::GetWorldLocation(void* component, double out[3]) {
    if (!component || !fnGetComponentLocation_ || !locationSize_) return false;
    // The parameter block for a no-argument function is just its return value.
    // Oversized and zeroed so a wider signature than we measured cannot write
    // past the end.
    alignas(8) uint8_t params[64] = {};
    ue4::GetProcessEventHook().CallFunction(component, fnGetComponentLocation_, params);
    if (locationSize_ == 24) {
        double v[3];
        std::memcpy(v, params, sizeof(v));
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    } else {
        float v[3];
        std::memcpy(v, params, sizeof(v));
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    }
    // A component at exactly the origin is far more likely to be a failed call
    // than a real position, and treating it as real would drag the anchor to
    // (0,0,0) and reject the entire world.
    return !(out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0);
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

void* CustomDepthMarker::UnmarkSlotForGroundTruth(ue4::Engine& engine, uint8_t slot) {
    // Deliberately does NOT release the slot back to the pool, and does NOT
    // remove the entry from marked_. Both would confound the measurement: a
    // reissued slot would put a DIFFERENT object's pixels under the same id, and
    // "the pixels went away" would no longer mean what we want it to mean.
    //
    // The point is to change exactly one thing -- this primitive's opt-in -- and
    // watch the mask. Leaving the lease held means any pixels that survive under
    // this id are unambiguously a failure of the claim, not a re-lease.
    for (auto& mp : marked_) {
        if (mp.stencilValue != slot) continue;
        if (!engine.StillLive(mp.component, mp.objectIndex, mp.serialNumber)) {
            LogWarn("groundtruth: slot %u's component was destroyed before the "
                    "intervention could run; nothing to prove",
                    static_cast<unsigned>(slot));
            return nullptr;
        }
        if (!fnSetRenderCustomDepth_) {
            LogError("groundtruth: no SetRenderCustomDepth UFunction, so the flag cannot "
                     "be cleared the way the engine would clear it");
            return nullptr;
        }
        // Pin BEFORE clearing the flag. From the moment the primitive stops
        // writing, its slot renders zero pixels -- which is exactly the signal
        // the coverage evictor in RefreshVisibility counts. Without the pin the
        // test cannot complete on a title that keeps the pool full: the evictor
        // needs 8 tiny masks, the verdict needs 30, and both tick once per mask,
        // so the slot is always reclaimed and reissued ~22 masks before it is
        // judged. The measurement's own success condition is what triggers it.
        //
        // It never showed up on Stray because that gate also requires
        // marked_.size() >= 247, and Stray's alleys never fill the pool.
        groundTruthPin_ = slot;
        struct { uint32_t bValue; } off = {0};
        ue4::GetProcessEventHook().CallFunction(mp.component, fnSetRenderCustomDepth_, &off);
        LogWarn("groundtruth: UNMARKED slot %u (%s) -- from here its pixels must vanish "
                "and no others may move",
                static_cast<unsigned>(slot), mp.className.c_str());
        return mp.component;
    }
    LogWarn("groundtruth: slot %u is not held by any marked primitive",
            static_cast<unsigned>(slot));
    return nullptr;
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
    if (!resolved_ || !fnWasRecentlyRendered_) return 0;
    if (marked_.empty()) return 0;

    int released = 0;
    const size_t n = std::min(static_cast<size_t>(limit > 0 ? limit : 0), marked_.size());

    for (size_t i = 0; i < n; ++i) {
        if (refreshCursor_ >= marked_.size()) refreshCursor_ = 0;
        const MarkedPrimitive mp = marked_[refreshCursor_];

        // Is this object still there? MarkedPrimitive has carried objectIndex
        // and serialNumber since the first commit, under a comment saying they
        // "guard against GC slot reuse" -- and nothing ever read them. The data
        // was collected and the check was never written.
        //
        // It went unnoticed because on Stray the marker only ever ran inside
        // one already-loaded level. Marking in inZOI's main menu and then
        // loading a save destroys all 122 marked components at once, and the
        // next visibility sweep called WasRecentlyRendered on every freed
        // pointer -- through ProcessEvent, on the game thread, during a level
        // transition. The game died at t=89 with nothing in the log.
        //
        // Drop it without unmarking: the object is gone, so there is nothing
        // left to restore, and touching it to "clean up" is the crash itself.
        if (!engine.StillLive(mp.component, mp.objectIndex, mp.serialNumber)) {
            {
                std::lock_guard<std::mutex> lock(identityMutex_);
                identity_.ReleaseSlot(mp.stencilValue);
            }
            slotTable_.erase(mp.stencilValue);
            slotIsCharacter_[mp.stencilValue] = 0;
            alreadyMarked_.erase(mp.component);
            pooled_.erase(mp.component);
            marked_.erase(marked_.begin() + static_cast<ptrdiff_t>(refreshCursor_));
            ++released;
            ++slotsReleased_;
            ++staleDropped_;
            continue;   // erase shifted the next element into this cursor
        }

        // A tolerance wider than the one used for acquisition, on purpose.
        // Equal thresholds make an object hovering at the edge of visibility
        // acquire and release a slot every pass, which is thrash by another
        // name. The gap is hysteresis.
        // Rendered, but is it rendering ANYTHING? Four consecutive masks under
        // the area threshold means this slot is labelling a speck, and a speck
        // costs exactly as much as the foreground object that has no label. Hand
        // it back so the next scan can lease it to something that fills pixels.
        // The stable id survives, so if the object comes closer it resumes its
        // identity rather than appearing as a new one.
        // ONLY UNDER PRESSURE. Evicting whenever a slot looks small shrank the
        // working set from 255 live slots to 57: slots were handed back faster
        // than the object scan could re-lease them, and every re-leased
        // candidate was itself liable to be small, so the pool drained instead
        // of migrating. A free slot costs nothing, so there is no reason to take
        // one back until the budget is actually full -- the point of this is to
        // choose BETWEEN objects when 255 is not enough, not to have fewer.
        // The slot under ground-truth measurement is exempt from THIS eviction
        // and only this one. That asymmetry is the point, not an oversight:
        //
        //   - the coverage evictor fires BECAUSE the intervention worked, so
        //     leaving it live makes the test unable to ever reach a verdict;
        //   - the stale-object drop above and the not-rendered release below
        //     fire for reasons that have nothing to do with our flag, and they
        //     are what STOPS this test reporting a false PASS. If the object is
        //     destroyed or walks off camera, its pixels vanish for a reason we
        //     did not cause; those two paths remove it from marked_, and the
        //     verdict's "is the slot still held by the component we unmarked?"
        //     check then correctly reports INCONCLUSIVE instead of success.
        //
        // Pinning against all three would have silently converted every
        // camera-cut into proof.
        const bool pinned = mp.stencilValue == groundTruthPin_.load();

        // HAND BACK SLOTS THAT HAVE DRIFTED OUT OF RANGE.
        //
        // The gate in MarkBatch stops NEW slots going to the street; this is
        // what frees the ones already there. Without it the road and trees keep
        // their ids for as long as they stay nominally rendered, and the room
        // never gets the budget -- which is the symptom that started this.
        //
        // 1.25x the acquisition radius, for the same reason the visibility test
        // uses a wider tolerance than acquisition: equal thresholds make an
        // object at the boundary acquire and release every pass, which is thrash
        // with extra steps.
        if (!pinned && markRadius_ > 0.0 && anchorValid_) {
            double p[3];
            if (GetWorldLocation(mp.component, p)) {
                const double dx = p[0] - anchor_[0];
                const double dy = p[1] - anchor_[1];
                const double dz = p[2] - anchor_[2];
                const double limit = markRadius_ * 1.25;
                if (dx * dx + dy * dy + dz * dz > limit * limit) {
                    UnmarkPrimitive(mp);
                    {
                        std::lock_guard<std::mutex> lock(identityMutex_);
                        identity_.ReleaseSlot(mp.stencilValue);
                    }
                    if (mp.className.find("InstancedStaticMesh") != std::string::npos ||
                        mp.className.find("Foliage") != std::string::npos) {
                        if (instancedLeased_) --instancedLeased_;
                    }
                    slotTable_.erase(mp.stencilValue);
                    slotIsCharacter_[mp.stencilValue] = 0;
                    alreadyMarked_.erase(mp.component);
                    marked_.erase(marked_.begin() + static_cast<ptrdiff_t>(refreshCursor_));
                    ++released;
                    ++slotsReleased_;
                    ++releasedFar_;
                    continue;   // erase shifted the next element into this cursor
                }
            }
        }

        if (!pinned && marked_.size() >= IdentityRegistry::kSlotCount - 8 &&
            tinyStreak_[mp.stencilValue] >= 8) {
            tinyStreak_[mp.stencilValue] = 0;
            UnmarkPrimitive(mp);
            {
                std::lock_guard<std::mutex> lock(identityMutex_);
                identity_.ReleaseSlot(mp.stencilValue);
            }
            if (mp.className.find("InstancedStaticMesh") != std::string::npos ||
                mp.className.find("Foliage") != std::string::npos) {
                if (instancedLeased_) --instancedLeased_;
            }
            slotTable_.erase(mp.stencilValue);
            slotIsCharacter_[mp.stencilValue] = 0;
            alreadyMarked_.erase(mp.component);
            marked_.erase(marked_.begin() + static_cast<ptrdiff_t>(refreshCursor_));
            ++released;
            ++slotsReleased_;
            ++slotsFreedForCoverage_;
            continue;
        }

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
    // Require a non-trivial number of rejects before reporting, not just a low
    // `found`. The first version fired during inZOI's main menu, where nothing
    // renderable exists yet and nothing was rejected either, and printed a
    // header promising a list followed by no list at all -- a log line that
    // announces evidence and delivers none is worse than silence, because the
    // next reader spends time deciding whether the empty list means something.
    size_t rejectedTotal = 0;
    for (const auto& kv : rejectedByClass) rejectedTotal += static_cast<size_t>(kv.second);

    if (found < 8 && rejectedTotal >= 50 && !reportedRejects_) {
        reportedRejects_ = true;
        std::vector<std::pair<std::string, int>> top(rejectedByClass.begin(),
                                                     rejectedByClass.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        LogWarn("customdepth: only %d renderable candidates, but %zu *Component "
                "objects were REJECTED as non-renderable:", found, rejectedTotal);
        for (size_t i = 0; i < top.size() && i < 15; ++i) {
            LogWarn("customdepth:   %6d  %s", top[i].second, top[i].first.c_str());
        }
    }
    return found;
}

int CustomDepthMarker::MarkBatch(ue4::Engine& engine, int limit) {
    if (!resolved_) return 0;

    // Re-locate the anchor once per pass, on the game thread where calling a
    // UFunction is legal. The render thread chose which SLOT is the character;
    // this turns that into a position.
    //
    // Re-read every pass rather than cached, because the character walks. A
    // stale anchor would keep the radius centred on where she was, which is the
    // same bug as WasRecentlyRendered's time tolerance, just slower.
    if (markRadius_ > 0.0 && fnGetComponentLocation_) {
        const uint8_t slot = anchorSlot_.load(std::memory_order_relaxed);
        void* anchorComponent = nullptr;
        if (slot) {
            for (const auto& mp : marked_) {
                if (mp.stencilValue == slot) {
                    if (engine.StillLive(mp.component, mp.objectIndex, mp.serialNumber)) {
                        anchorComponent = mp.component;
                    }
                    break;
                }
            }
        }
        if (anchorComponent) {
            double p[3];
            if (GetWorldLocation(anchorComponent, p)) {
                anchor_[0] = p[0]; anchor_[1] = p[1]; anchor_[2] = p[2];
                if (!anchorValid_) {
                    anchorValid_ = true;
                    LogInfo("customdepth: distance gate ACTIVE -- anchored on slot %u at "
                            "(%.0f, %.0f, %.0f), radius %.0f units",
                            static_cast<unsigned>(slot), p[0], p[1], p[2], markRadius_);
                }
            }
        }
        // anchorValid_ is never cleared once set. Losing the anchor for a frame
        // -- the character occluded, the slot reissued -- would otherwise switch
        // the gate off and let the street back in, which is exactly the flicker
        // this is meant to remove. The last known position is a better centre
        // than no centre.
    }

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
    int skippedInstancedQuota = 0;
    int skippedAlready = 0;

    // Cap how many primitives are newly opted in per pass.
    //
    // Marking is not a cheap write. SetRenderCustomDepth dirties the render
    // state, which makes the engine tear down and rebuild that component's
    // scene proxy. Doing that to 255 components inside a single frame is a
    // burst of proxy recreation on the render thread, and on UE5 with Nanite
    // it killed inZOI every time -- the log showed four MarkBatch passes
    // landing within 15 milliseconds ("marked 70 / 14 / 84 / 87", 0 to 255
    // live slots), with the process gone about two seconds later.
    //
    // Stray never hit this only because its scenes hand back one to seven
    // newly-visible primitives per pass; the burst was always latent. Filling
    // the working set over ~8 seconds instead of one frame costs nothing --
    // the slots are held once acquired -- and turns a spike into a trickle.
    constexpr int kMaxNewMarksPerPass = 8;

    int skippedStale = 0;
    for (const Candidate& c : batch) {
        if (freeSlots == 0) break;
        if (assigned >= kMaxNewMarksPerPass) break;

        // The candidate pool is walked round-robin and re-collected only every
        // ~30 seconds, so entries routinely outlive the objects they name --
        // level streaming alone guarantees it. Validate the generational handle
        // before calling into the component, for the same reason as in
        // RefreshVisibility: the alternative is a ProcessEvent call on freed
        // memory.
        if (!engine.StillLive(c.component, c.index, c.serial)) {
            ++skippedStale;
            continue;
        }

        // Already holds a slot: nothing to do, and re-leasing would only churn.
        if (alreadyMarked_.find(c.component) != alreadyMarked_.end()) {
            ++skippedAlready;
            continue;
        }

        // THE DISTANCE GATE, before the visibility one on purpose.
        //
        // Both cost a UFunction call, and indoors this one rejects far more of
        // the batch -- the whole city is a candidate while the room is a few
        // dozen objects -- so testing it first spends one call on a distant
        // object instead of two.
        //
        // WasRecentlyRendered has no notion of where a thing is. A tree eighty
        // metres down the street passes it exactly as well as the chair the
        // character is sitting on, and indoors that is most of the budget.
        if (markRadius_ > 0.0 && anchorValid_) {
            double p[3];
            if (GetWorldLocation(c.component, p)) {
                const double dx = p[0] - anchor_[0];
                const double dy = p[1] - anchor_[1];
                const double dz = p[2] - anchor_[2];
                if (dx * dx + dy * dy + dz * dz > markRadius_ * markRadius_) {
                    ++skippedFar_;
                    continue;
                }
            }
            // A component with no readable position is NOT rejected. "I could
            // not measure it" is not "it is far away", and silently dropping
            // everything the call fails on would empty the mask for a reason
            // no counter would show.
        }

        // The visibility gate. Tested BEFORE leasing, because a slot spent on
        // an off-screen primitive is a slot that cannot label a visible one,
        // and there are only 255 of them against ~32,800 candidates.
        if (!WasRecentlyRendered(c.component, 0.3f)) {
            ++skippedInvisible;
            continue;
        }

        // QUOTA THE INSTANCED MESHES.
        //
        // One InstancedStaticMeshComponent covers EVERY instance of a mesh --
        // all the paving, all the trees of a type -- so it is a single slot
        // buying an enormous screen area. Measured, they took 51% of all slots
        // (38.2% ISM + 11.8% HISM + 1.0% foliage) while SkeletalMesh, i.e. the
        // people, got 5.9%. The result labels ground, buildings and foliage and
        // misses chairs, cars and characters.
        //
        // The area-based eviction added for coverage made this worse, not
        // better: it rewards whatever fills pixels, which is exactly these. A
        // chair is small BY NATURE, and optimising coverage evicts it in favour
        // of a road. Coverage and object diversity are different goals and the
        // budget has to be split deliberately rather than handed to whichever
        // metric was measured last.
        //
        // A third of the budget is plenty to keep the environment legible; the
        // rest goes to discrete objects, which is what a segmentation map is
        // actually for.
        const bool instanced =
            c.className.find("InstancedStaticMesh") != std::string::npos ||
            c.className.find("Foliage") != std::string::npos;
        if (instanced && instancedLeased_ >= IdentityRegistry::kSlotCount / 3) {
            ++skippedInstancedQuota;
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
        if (slot != 0 && instanced) ++instancedLeased_;
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
        // Tag it for the anchor picker on the render thread. Skeletal meshes are
        // the people -- inZOI's Zois come through as SkeletalMeshComponent and
        // SkeletalMeshComponentBudgeted, Stray's cat the same way.
        slotIsCharacter_[slot] =
            c.className.find("SkeletalMesh") != std::string::npos ? 1 : 0;
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
    if (assigned > 0 || refused > 0 || skippedStale > 0) {
        std::lock_guard<std::mutex> lock(identityMutex_);
        // far-skipped / far-released are logged so the radius can be CHOSEN from
        // a run rather than guessed: near-zero means it is too generous to be
        // doing anything, and a mask that has lost the room means too tight.
        //
        // ONLY WHEN THE NUMBERS MOVE. The first version printed every pass --
        // three lines a second saying nothing changed. The log is this project's
        // primary instrument and tail() reads a fixed 256 KB window, so padding
        // it with restatements pushes real signals out of view; that mechanism
        // has already cost a day here. A counter worth logging is worth logging
        // when it changes.
        if (markRadius_ > 0.0 &&
            (skippedFar_ != lastLoggedSkippedFar_ ||
             releasedFar_ != lastLoggedReleasedFar_ ||
             anchorValid_ != lastLoggedAnchorValid_)) {
            lastLoggedSkippedFar_ = skippedFar_;
            lastLoggedReleasedFar_ = releasedFar_;
            lastLoggedAnchorValid_ = anchorValid_;
            LogInfo("customdepth: distance gate -- %zu candidates skipped as far, "
                    "%zu marked objects handed back after drifting out of range "
                    "(radius %.0f, anchor %s)",
                    skippedFar_, releasedFar_, markRadius_,
                    anchorValid_ ? "locked" : "NOT YET FOUND -- gate idle");
        }
        LogInfo("customdepth: marked %d (refused %d, invisible %d, already %d, stale %d "
                "of %zu scanned); %zu live slots, %llu identities, %llu evictions, "
                "%llu dropped-destroyed, visible %llu/%llu tested",
                assigned, refused, skippedInvisible, skippedAlready, skippedStale,
                batch.size(), identity_.liveSlots(), identity_.totalIdentities(),
                identity_.evictions(), staleDropped_, visibilityHits_, visibilityTested_);
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

// Slots are a budget of 255 for a world of half a million objects, so the only
// question that matters is whether a slot is buying screen. This is the reply
// from the mask: how much area each id actually covered.
void CustomDepthMarker::NoteMaskCoverage(const uint32_t* pixelsPerSlot) {
    if (!pixelsPerSlot) return;
    // ~0.02% of a 1280x800 frame. Below this an object is a speck at the far end
    // of the street, and a speck is not worth one of 255 labels while the chair
    // in the foreground has none.
    constexpr uint32_t kMinPixels = 200;

    // Pick the point everything else is measured from: the biggest character on
    // screen. The mask is the only place that knows what is actually large in
    // view, so the choice is made here and the position is looked up later on
    // the game thread -- a slot number survives that hop, a component pointer
    // does not.
    //
    // Falls back to the largest object of any class so an empty room still has
    // an anchor. It is a worse anchor -- indoors that is usually the apartment
    // shell, whose origin can be anywhere -- but a wrong-ish centre is better
    // than switching the gate off entirely.
    uint8_t bestCharacter = 0, bestAny = 0;
    uint32_t bestCharacterPx = 0, bestAnyPx = 0;
    for (int slot = 1; slot < 256; ++slot) {
        const uint32_t px = pixelsPerSlot[slot];
        if (px > bestAnyPx) { bestAnyPx = px; bestAny = static_cast<uint8_t>(slot); }
        if (slotIsCharacter_[slot] && px > bestCharacterPx) {
            bestCharacterPx = px;
            bestCharacter = static_cast<uint8_t>(slot);
        }
    }
    anchorSlot_.store(bestCharacter ? bestCharacter : bestAny, std::memory_order_relaxed);

    for (int slot = 1; slot < 256; ++slot) {
        if (pixelsPerSlot[slot] >= kMinPixels) {
            tinyStreak_[slot] = 0;
        } else if (tinyStreak_[slot] < 255) {
            ++tinyStreak_[slot];
        }
    }

}

}  // namespace segcap
