// identity.h -- turning an 8-bit channel into stable object identity.
//
// THE PROBLEM, stated plainly because it is the crux of the brief:
//
//   The stencil channel is 8 bits. That is 255 usable values (0 means
//   "unmarked"). Stray has ~29,000 renderable primitives. A stencil value is
//   therefore NOT an identity -- it is a per-frame LEASE on one of 255 slots.
//
// Two objects will inevitably share value 42 at different times, and possibly
// in different frames of the same recording. A mask alone is ambiguous. It only
// becomes meaningful when joined against the slot table that was live for THAT
// frame.
//
// So identity is split into three layers:
//
//   slot       uint8, 1..255. A lease. Recycled aggressively. Meaningless alone.
//   stable id  uint64, assigned on first sighting, never reused. This is what a
//              downstream consumer actually joins on.
//   sidecar    per-frame map slot -> stable id (+ class, name). Emitted with
//              every frame. The mask is only decodable with it.
//
// WHY THE STABLE ID IS NOT JUST THE POINTER:
//
// UObject slots are recycled after garbage collection. A raw UObject* can refer
// to object A in frame 100 and a completely unrelated object B in frame 900.
// Keying identity on the pointer alone would silently merge two different things
// into one track -- the worst possible failure for training data, because it is
// invisible and corrupts the labels rather than losing them. UE stores a serial
// number alongside each slot precisely to detect this, so the key is
// (pointer, serial). When the serial changes, it is a NEW object and gets a NEW
// stable id.
//
// SLOT ALLOCATION POLICY:
//
// With more objects than slots, something must be prioritised. Visible-and-large
// beats distant-and-tiny: a segmentation label on an object covering 3 pixels is
// worth far less than one covering 30,000. Slots are therefore leased to the
// primitives most recently seen contributing pixels, and reclaimed LRU from
// those that have not appeared for a while.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace segcap {

// What a slot number means for one specific frame.
struct SlotBinding {
    uint8_t slot = 0;
    uint64_t stableId = 0;
    void* component = nullptr;
    int32_t serialNumber = 0;
    std::string className;
    std::string objectName;
};

// Everything needed to decode one frame's mask, emitted alongside it.
struct FrameSidecar {
    uint64_t frameIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<SlotBinding> bindings;
};

class IdentityRegistry {
public:
    // Slots 1..255 are leasable; 0 is reserved for "unlabelled" in the mask, so
    // it can never be handed out. Callers need this to know when the working
    // set is full -- marking more objects than there are slots does not label
    // more of the scene, it just evicts labels that were already working.
    static constexpr size_t kSlotCount = 255;

    // Returns the stable id for an object, allocating one on first sighting.
    // The (pointer, serial) pair is the key: a recycled UObject slot has a
    // different serial and therefore becomes a genuinely new identity rather
    // than silently inheriting the previous occupant's track.
    uint64_t StableIdFor(void* component, int32_t serialNumber,
                         const std::string& className, const std::string& objectName);

    // Leases a slot to an object. Returns 0 if none could be allocated.
    // Re-leasing an object that already holds a slot returns the same slot, so
    // identities stay stable across frames rather than churning.
    uint8_t LeaseSlot(void* component, int32_t serialNumber,
                      const std::string& className, const std::string& objectName,
                      uint64_t currentFrame);

    // Marks a slot as still in use this frame, protecting it from LRU eviction.
    void Touch(uint8_t slot, uint64_t currentFrame);

    // Hands a slot back voluntarily, so it can be leased to something else.
    //
    // Distinct from eviction: eviction is the registry deciding under pressure,
    // whereas this is the caller reporting that the object no longer deserves a
    // slot -- in practice, that the renderer has stopped drawing it. The stable
    // id is KEPT, so if the object becomes visible again it resumes its previous
    // identity rather than being logged as a new object. That is the whole point
    // of separating a 64-bit identity from an 8-bit lease.
    void ReleaseSlot(uint8_t slot);

    // The table needed to decode this frame's mask.
    FrameSidecar BuildSidecar(uint64_t frameIndex, uint32_t width, uint32_t height) const;

    size_t liveSlots() const { return bySlot_.size(); }
    uint64_t totalIdentities() const { return nextStableId_ - 1; }
    uint64_t evictions() const { return evictions_; }
    uint64_t recycleCollisions() const { return recycleCollisions_; }

private:
    struct Entry {
        uint64_t stableId = 0;
        void* component = nullptr;
        int32_t serialNumber = 0;
        std::string className;
        std::string objectName;
        uint64_t lastSeenFrame = 0;
        uint8_t slot = 0;
    };

    // Key is pointer + serial, so a recycled slot does not inherit an identity.
    struct Key {
        void* ptr;
        int32_t serial;
        bool operator==(const Key& o) const { return ptr == o.ptr && serial == o.serial; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<void*>()(k.ptr) ^ (std::hash<int32_t>()(k.serial) << 1);
        }
    };

    uint8_t EvictLeastRecentlyUsed(uint64_t currentFrame);

    std::unordered_map<Key, Entry, KeyHash> byObject_;
    std::unordered_map<uint8_t, Key> bySlot_;

    uint64_t nextStableId_ = 1;   // 0 reserved for "no identity"
    uint64_t evictions_ = 0;
    // Counts objects whose pointer was reused with a different serial -- i.e.
    // exactly the case that would have corrupted identity had we keyed on the
    // pointer alone. Reported because it turns a hypothetical risk into a
    // measured one.
    uint64_t recycleCollisions_ = 0;
};

}  // namespace segcap
