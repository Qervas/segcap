#include "identity.h"

#include "log.h"

namespace segcap {

uint64_t IdentityRegistry::StableIdFor(void* component, int32_t serialNumber,
                                       const std::string& className,
                                       const std::string& objectName) {
    const Key key{component, serialNumber};
    auto it = byObject_.find(key);
    if (it != byObject_.end()) return it->second.stableId;

    // Has this pointer been seen before with a DIFFERENT serial? That is a
    // recycled UObject slot: the memory is the same, the object is not. Counting
    // it turns "pointer reuse could corrupt identity" from a hypothesis into a
    // number we can report.
    for (const auto& kv : byObject_) {
        if (kv.first.ptr == component && kv.first.serial != serialNumber) {
            ++recycleCollisions_;
            break;
        }
    }

    Entry e;
    e.stableId = nextStableId_++;
    e.component = component;
    e.serialNumber = serialNumber;
    e.className = className;
    e.objectName = objectName;
    byObject_[key] = e;
    return e.stableId;
}

uint8_t IdentityRegistry::EvictLeastRecentlyUsed(uint64_t currentFrame) {
    uint8_t victimSlot = 0;
    uint64_t oldest = UINT64_MAX;

    for (const auto& kv : bySlot_) {
        auto it = byObject_.find(kv.second);
        if (it == byObject_.end()) {
            // Dangling binding; reclaim immediately.
            victimSlot = kv.first;
            break;
        }
        // Never evict something seen this frame -- that would thrash a slot
        // between two visible objects and make both of their tracks useless.
        if (it->second.lastSeenFrame == currentFrame) continue;
        if (it->second.lastSeenFrame < oldest) {
            oldest = it->second.lastSeenFrame;
            victimSlot = kv.first;
        }
    }

    if (victimSlot != 0) {
        auto binding = bySlot_.find(victimSlot);
        if (binding != bySlot_.end()) {
            auto obj = byObject_.find(binding->second);
            // The stable id survives eviction. Losing a slot means "not
            // currently labelled", not "no longer the same object" -- if it
            // becomes visible again it must resume its original identity, or
            // every occlusion would fragment one object into several tracks.
            if (obj != byObject_.end()) obj->second.slot = 0;
            bySlot_.erase(binding);
        }
        ++evictions_;
    }
    return victimSlot;
}

uint8_t IdentityRegistry::LeaseSlot(void* component, int32_t serialNumber,
                                    const std::string& className,
                                    const std::string& objectName,
                                    uint64_t currentFrame) {
    const Key key{component, serialNumber};
    StableIdFor(component, serialNumber, className, objectName);

    auto it = byObject_.find(key);
    if (it == byObject_.end()) return 0;
    it->second.lastSeenFrame = currentFrame;

    // Already holds a slot -- keep it. Reassigning would change an object's
    // label mid-recording for no reason.
    if (it->second.slot != 0) return it->second.slot;

    // Find a free slot. 0 is reserved for "unmarked".
    uint8_t slot = 0;
    for (uint32_t s = 1; s <= 255; ++s) {
        if (bySlot_.find(static_cast<uint8_t>(s)) == bySlot_.end()) {
            slot = static_cast<uint8_t>(s);
            break;
        }
    }
    if (slot == 0) slot = EvictLeastRecentlyUsed(currentFrame);
    if (slot == 0) return 0;   // everything is in use this frame

    it->second.slot = slot;
    bySlot_[slot] = key;
    return slot;
}

void IdentityRegistry::Touch(uint8_t slot, uint64_t currentFrame) {
    auto binding = bySlot_.find(slot);
    if (binding == bySlot_.end()) return;
    auto obj = byObject_.find(binding->second);
    if (obj != byObject_.end()) obj->second.lastSeenFrame = currentFrame;
}

void IdentityRegistry::ReleaseSlot(uint8_t slot) {
    auto binding = bySlot_.find(slot);
    if (binding == bySlot_.end()) return;

    // Clear the slot on the entry but KEEP the entry itself. The object's stable
    // id survives, so an object that goes off screen and comes back is
    // recognised as the same object rather than counted as a new one -- which
    // is exactly the distinction between a 64-bit identity and an 8-bit lease.
    auto obj = byObject_.find(binding->second);
    if (obj != byObject_.end()) obj->second.slot = 0;
    bySlot_.erase(binding);
}

FrameSidecar IdentityRegistry::BuildSidecar(uint64_t frameIndex, uint32_t width,
                                            uint32_t height) const {
    FrameSidecar sc;
    sc.frameIndex = frameIndex;
    sc.width = width;
    sc.height = height;
    sc.bindings.reserve(bySlot_.size());

    for (const auto& kv : bySlot_) {
        auto obj = byObject_.find(kv.second);
        if (obj == byObject_.end()) continue;
        SlotBinding b;
        b.slot = kv.first;
        b.stableId = obj->second.stableId;
        b.component = obj->second.component;
        b.serialNumber = obj->second.serialNumber;
        b.className = obj->second.className;
        b.objectName = obj->second.objectName;
        sc.bindings.push_back(std::move(b));
    }
    return sc;
}

}  // namespace segcap
