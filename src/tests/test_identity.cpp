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

void TestVoluntaryReleaseKeepsIdentity() {
    std::printf("\nhanding a slot back voluntarily (object left the screen)\n");
    segcap::IdentityRegistry reg;

    // This covers ReleaseSlot, which is a DIFFERENT path from eviction and was
    // added when marking became visibility-driven. Eviction is the registry
    // deciding under pressure; release is the caller reporting that the
    // renderer has stopped drawing the object. Only the eviction path was
    // tested, and a live 75-frame session happened not to exercise release-and-
    // return at all -- so this property was asserted in the design and
    // demonstrated by nothing.
    const uint64_t catId = reg.StableIdFor(Ptr(0xAAAA), 3, "SkeletalMeshComponent", "Cat");
    const uint8_t catSlot = reg.LeaseSlot(Ptr(0xAAAA), 3, "SkeletalMeshComponent", "Cat", 1);
    Check(catSlot != 0, "the object got a slot while visible");
    Check(reg.liveSlots() == 1, "one slot live");

    reg.ReleaseSlot(catSlot);
    Check(reg.liveSlots() == 0, "releasing frees the slot for someone else");

    // The binding must remain RESOLVABLE while the slot is unclaimed. The
    // renderer can keep drawing the primitive into CustomDepth for a frame or
    // two after the game thread unmarks it, and a mask pixel whose id is not in
    // the table cannot be decoded at all.
    {
        const segcap::FrameSidecar trailing = reg.BuildSidecar(2, 1280, 720);
        bool found = false;
        for (const auto& b : trailing.bindings) {
            if (b.slot == catSlot) found = (b.stableId == catId && b.released);
        }
        Check(found, "a released slot is still decodable, flagged released");
    }

    // Somebody else takes the freed slot. This is the case that would corrupt
    // the mask if identity were carried by the slot number.
    const uint8_t crateSlot =
        reg.LeaseSlot(Ptr(0xBBBB), 7, "StaticMeshComponent", "Crate", 2);
    Check(crateSlot == catSlot, "the freed slot is reissued to a different object");
    Check(reg.StableIdFor(Ptr(0xBBBB), 7, "StaticMeshComponent", "Crate") != catId,
          "the new occupant has its OWN identity, not the previous tenant's");

    // The cat comes back. It must resume its original identity even though the
    // slot it used to hold now belongs to something else, and even though it
    // will be given a different pixel value than before.
    const uint8_t catSlot2 = reg.LeaseSlot(Ptr(0xAAAA), 3, "SkeletalMeshComponent", "Cat", 3);
    Check(catSlot2 != 0, "the returning object gets a slot again");
    Check(catSlot2 != crateSlot, "not the one currently held by the crate");
    Check(reg.StableIdFor(Ptr(0xAAAA), 3, "SkeletalMeshComponent", "Cat") == catId,
          "the returning object resumes its ORIGINAL stable id under a new slot");
    Check(reg.totalIdentities() == 2,
          "two identities total -- the round trip did not invent a third");

    // And the sidecar for the frame after the return must resolve both slots to
    // the right objects, which is the whole point of emitting one per frame.
    const segcap::FrameSidecar sc = reg.BuildSidecar(3, 1280, 720);
    bool catOk = false, crateOk = false;
    for (const auto& b : sc.bindings) {
        if (b.slot == catSlot2) catOk = (b.stableId == catId && b.objectName == "Cat");
        if (b.slot == crateSlot) crateOk = (b.objectName == "Crate");
    }
    Check(catOk && crateOk,
          "the sidecar resolves the recycled slot and the returning object correctly");

    // And the trailing binding must be GONE once the slot is reissued -- two
    // entries for one slot is exactly the ambiguity the sidecar prevents.
    int occurrences = 0;
    for (const auto& b : sc.bindings) {
        if (b.slot == crateSlot) ++occurrences;
    }
    Check(occurrences == 1, "a reissued slot appears exactly once, not twice");
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
    TestVoluntaryReleaseKeepsIdentity();
    TestSidecarDecodesTheMask();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
