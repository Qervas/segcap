// test_identity.cpp -- proves the slot/identity logic without needing the game.
//
// This is the part of the brief with the least room for hand-waving: "the
// obvious channel is only 8 bits; think about what an ID means across frames and
// how a mask joins back to a real game object." The failure modes here are
// invisible at runtime -- a corrupted identity produces a mask that looks
// perfectly fine and mislabels an object -- so they are worth asserting rather
// than eyeballing.

#include <cstdio>
#include <string>
#include <vector>

#include "../segcap/identity.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

void* Ptr(uintptr_t v) { return reinterpret_cast<void*>(v); }

// ---------------------------------------------------------------------------

void TestStableIdIsStable() {
    std::printf("\nstable ids persist across frames\n");
    segcap::IdentityRegistry reg;

    const uint64_t a1 = reg.StableIdFor(Ptr(0x1000), 7, "StaticMeshComponent", "Crate");
    const uint64_t a2 = reg.StableIdFor(Ptr(0x1000), 7, "StaticMeshComponent", "Crate");
    Check(a1 == a2, "same (pointer, serial) yields the same stable id");

    const uint64_t b = reg.StableIdFor(Ptr(0x2000), 7, "StaticMeshComponent", "Barrel");
    Check(b != a1, "different objects get different stable ids");
    Check(reg.totalIdentities() == 2, "exactly two identities allocated");
}

void TestPointerRecyclingDoesNotMergeIdentities() {
    std::printf("\nGC pointer reuse does NOT merge two objects into one track\n");
    segcap::IdentityRegistry reg;

    // Object A lives at 0x1000 with serial 7.
    const uint64_t a = reg.StableIdFor(Ptr(0x1000), 7, "StaticMeshComponent", "Crate");

    // A is destroyed; the engine recycles the slot for B at the SAME address,
    // with a new serial number. Keying on the pointer alone would silently give
    // B the label of A -- the worst failure available, because the mask looks
    // correct and the labels are wrong.
    const uint64_t b = reg.StableIdFor(Ptr(0x1000), 8, "SkeletalMeshComponent", "Robot");

    Check(a != b, "recycled pointer with a new serial gets a NEW identity");
    Check(reg.recycleCollisions() == 1, "the collision is counted, not silently absorbed");
    Check(reg.totalIdentities() == 2, "two distinct identities recorded");
}

void TestSlotLeaseIsStickyAcrossFrames() {
    std::printf("\na leased slot does not churn between frames\n");
    segcap::IdentityRegistry reg;

    const uint8_t s1 = reg.LeaseSlot(Ptr(0x1000), 1, "StaticMeshComponent", "Crate", 100);
    const uint8_t s2 = reg.LeaseSlot(Ptr(0x1000), 1, "StaticMeshComponent", "Crate", 101);
    const uint8_t s3 = reg.LeaseSlot(Ptr(0x1000), 1, "StaticMeshComponent", "Crate", 102);

    Check(s1 != 0, "a slot was allocated");
    Check(s1 == s2 && s2 == s3, "the same object keeps the same slot across frames");
}

void TestSlotExhaustionEvictsLeastRecentlyUsed() {
    std::printf("\n255 slots, more objects: LRU eviction, never the current frame\n");
    segcap::IdentityRegistry reg;

    // Fill every slot at frame 1.
    std::vector<uint8_t> slots;
    for (uintptr_t i = 1; i <= 255; ++i) {
        slots.push_back(reg.LeaseSlot(Ptr(0x10000 + i * 0x100), 1, "StaticMeshComponent",
                                      "Obj", 1));
    }
    bool allAllocated = true;
    for (uint8_t s : slots) if (s == 0) allAllocated = false;
    Check(allAllocated, "all 255 slots allocated");
    Check(reg.liveSlots() == 255, "registry reports 255 live slots");

    // A new object at a LATER frame must evict someone, because everything else
    // was last seen at frame 1.
    const uint8_t victimSlot = reg.LeaseSlot(Ptr(0xDEAD0000), 1, "StaticMeshComponent",
                                             "Newcomer", 50);
    Check(victimSlot != 0, "a 256th object still gets a slot by eviction");
    Check(reg.evictions() == 1, "exactly one eviction occurred");
    Check(reg.liveSlots() == 255, "still 255 live slots, not 256");
}

void TestCurrentFrameObjectsAreNotEvicted() {
    std::printf("\nobjects visible THIS frame are protected from eviction\n");
    segcap::IdentityRegistry reg;

    // Fill all slots, all seen at frame 500.
    for (uintptr_t i = 1; i <= 255; ++i) {
        reg.LeaseSlot(Ptr(0x10000 + i * 0x100), 1, "StaticMeshComponent", "Obj", 500);
    }
    // Another object in the SAME frame cannot be satisfied without evicting
    // something also visible this frame, which would thrash both labels. The
    // correct answer is to refuse.
    const uint8_t s = reg.LeaseSlot(Ptr(0xBEEF0000), 1, "StaticMeshComponent", "TooMany", 500);
    Check(s == 0, "refuses rather than evicting an object visible this frame");
}

void TestIdentitySurvivesSlotLoss() {
    std::printf("\nlosing a slot does not lose the identity (occlusion)\n");
    segcap::IdentityRegistry reg;

    const uint64_t id = reg.StableIdFor(Ptr(0xAAAA), 3, "StaticMeshComponent", "Crate");
    reg.LeaseSlot(Ptr(0xAAAA), 3, "StaticMeshComponent", "Crate", 1);

    // Fill the rest at frame 1, then force evictions from a later frame.
    for (uintptr_t i = 1; i <= 254; ++i) {
        reg.LeaseSlot(Ptr(0x20000 + i * 0x100), 1, "StaticMeshComponent", "Filler", 1);
    }
    for (uintptr_t i = 1; i <= 60; ++i) {
        reg.LeaseSlot(Ptr(0x90000 + i * 0x100), 1, "StaticMeshComponent", "Later", 900);
    }

    // Whether or not it kept its slot, coming back must resume the SAME
    // identity. If an occlusion split one object into two tracks, every
    // downstream consumer would see them as different things.
    const uint64_t again = reg.StableIdFor(Ptr(0xAAAA), 3, "StaticMeshComponent", "Crate");
    Check(again == id, "the object resumes its original stable id after eviction");
}

void TestSidecarDecodesTheMask() {
    std::printf("\nthe sidecar is what makes a mask decodable\n");
    segcap::IdentityRegistry reg;

    const uint8_t sCrate = reg.LeaseSlot(Ptr(0x1111), 1, "StaticMeshComponent", "Crate", 10);
    const uint8_t sCat = reg.LeaseSlot(Ptr(0x2222), 1, "SkeletalMeshComponent", "Cat", 10);

    const segcap::FrameSidecar sc = reg.BuildSidecar(10, 2560, 1600);
    Check(sc.frameIndex == 10, "sidecar carries its frame index");
    Check(sc.bindings.size() == 2, "one binding per live slot");

    bool foundCat = false;
    for (const auto& b : sc.bindings) {
        if (b.slot == sCat) {
            foundCat = (b.className == "SkeletalMeshComponent" && b.objectName == "Cat");
        }
    }
    Check(foundCat, "a slot resolves back to the real class and object name");
    Check(sCrate != sCat, "distinct objects hold distinct slots in one frame");
}

}  // namespace

int main() {
    std::printf("identity / slot allocation tests\n");
    std::printf("================================\n");

    TestStableIdIsStable();
    TestPointerRecyclingDoesNotMergeIdentities();
    TestSlotLeaseIsStickyAcrossFrames();
    TestSlotExhaustionEvictsLeastRecentlyUsed();
    TestCurrentFrameObjectsAreNotEvicted();
    TestIdentitySurvivesSlotLoss();
    TestSidecarDecodesTheMask();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
