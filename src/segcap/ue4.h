// ue4.h -- runtime discovery of Unreal Engine 4 internals in a stripped
// shipping binary.
//
// APPROACH: structural search, not byte-pattern signatures.
//
// The usual technique is to disassemble the target, find a function that
// references GUObjectArray, and hardcode a byte pattern with wildcards. That
// works exactly once: the pattern is keyed to one build, one compiler version,
// and one set of inlining decisions, and it breaks on the next patch. It also
// requires symbols or a disassembler for a binary we deliberately have neither
// for.
//
// Instead we search the module's writable data for a memory layout that
// SATISFIES THE INVARIANTS of FChunkedFixedUObjectArray, then prove the
// candidate by dereferencing it:
//
//   1. plausible field values (counts ordered, chunk arithmetic consistent)
//   2. Objects[0] is a readable pointer
//   3. Objects[0][0].Object is a readable pointer
//   4. that object's vtable points into the host module's code section
//   5. its ClassPrivate is itself a UObject whose vtable also points into code
//
// A false positive would have to satisfy all five. This is version-independent
// in a way a byte pattern is not, and when it fails it fails loudly rather than
// returning a plausible wrong address.
//
// Layouts below are UE4 x64 shipping (no editor). Where a size is ambiguous
// across minor versions, the code probes rather than assumes.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace segcap {
namespace ue4 {

// UE4 allocates UObject storage in fixed chunks of this many elements. It is a
// compile-time constant in the engine, not a runtime value, so it doubles as a
// strong invariant for validating a candidate array.
constexpr int32_t kElementsPerChunk = 64 * 1024;

// FName storage, UE 4.23+ (FNamePool). An FNameEntryId encodes a block index in
// the high bits and a byte offset in the low bits.
//
//   class FNameEntryAllocator {
//       FRWLock Lock;              // +0x00, SRWLOCK is 8 bytes
//       uint32  CurrentBlock;      // +0x08
//       uint32  CurrentByteCursor; // +0x0C
//       uint8*  Blocks[8192];      // +0x10   <- what we search for
//   };
//
// Entries are aligned to 2 bytes, so the offset field is scaled by the stride.
constexpr uint32_t kNameBlockOffsetBits = 16;
constexpr uint32_t kNameBlockOffsetMask = (1u << kNameBlockOffsetBits) - 1;
constexpr uint32_t kNameEntryStride = 2;
constexpr size_t kNameBlocksOffsetInPool = 0x10;

// struct FNameEntryHeader { uint16 bIsWide:1; uint16 LowercaseProbeHash:5;
//                           uint16 Len:10; };
// MSVC packs bitfields LSB-first, so Len occupies the top 10 bits.
constexpr uint32_t kNameLenShift = 6;

// struct FUObjectItem { UObject* Object; int32 Flags; int32 ClusterRootIndex;
//                       int32 SerialNumber; /* 4 bytes padding */ };
struct FUObjectItem {
    void* Object;
    int32_t Flags;
    int32_t ClusterRootIndex;
    int32_t SerialNumber;
    int32_t Padding;
};
static_assert(sizeof(FUObjectItem) == 24, "FUObjectItem layout");

// struct FChunkedFixedUObjectArray -- the payload inside FUObjectArray.
struct FChunkedFixedUObjectArray {
    FUObjectItem** Objects;
    FUObjectItem* PreAllocatedObjects;
    int32_t MaxElements;
    int32_t NumElements;
    int32_t MaxChunks;
    int32_t NumChunks;
};
static_assert(sizeof(FChunkedFixedUObjectArray) == 32, "FChunkedFixedUObjectArray layout");

// UObjectBase, x64 shipping:
//   0x00 vtable
//   0x08 EObjectFlags ObjectFlags
//   0x0C int32 InternalIndex
//   0x10 UClass* ClassPrivate
//   0x18 FName NamePrivate   (int32 ComparisonIndex; int32 Number)
//   0x20 UObject* OuterPrivate
struct UObjectLayout {
    static constexpr size_t kVTable = 0x00;
    static constexpr size_t kObjectFlags = 0x08;
    static constexpr size_t kInternalIndex = 0x0C;
    static constexpr size_t kClassPrivate = 0x10;
    static constexpr size_t kNamePrivate = 0x18;
    static constexpr size_t kOuterPrivate = 0x20;
};

// UE 4.25+ reflection layout.
//
// Properties moved from UProperty (a UObject) to FProperty (an FField) in 4.25,
// so they live on UStruct::ChildProperties rather than UStruct::Children.
//
//   class UStruct : UField {
//       UStruct* SuperStruct;       // +0x40
//       UField*  Children;          // +0x48  (UFunction etc.)
//       FField*  ChildProperties;   // +0x50  (FProperty chain)
//       int32    PropertiesSize;    // +0x58
//   };
//   class FField {
//       void*        vtable;        // +0x00
//       FFieldClass* ClassPrivate;  // +0x08
//       FFieldVariant Owner;        // +0x10  (16 bytes)
//       FField*      Next;          // +0x20
//       FName        NamePrivate;   // +0x28
//   };
//   class FProperty : FField {
//       int32 ArrayDim;             // +0x38
//       int32 ElementSize;          // +0x3C
//       uint64 PropertyFlags;       // +0x40
//       ...
//       int32 Offset_Internal;      // +0x4C   <- what we are actually after
//   };
//
// These describe the reflection system itself, which is fixed by engine version.
// What they let us avoid is hardcoding where bRenderCustomDepth sits inside
// UPrimitiveComponent -- that offset is looked up at runtime, and it is the part
// that varies per game and per build.
struct UStructLayout {
    static constexpr size_t kSuperStruct = 0x40;
    static constexpr size_t kChildProperties = 0x50;
    static constexpr size_t kPropertiesSize = 0x58;
};
struct FFieldLayout {
    static constexpr size_t kNext = 0x20;
    static constexpr size_t kNamePrivate = 0x28;
};
struct FPropertyLayout {
    static constexpr size_t kArrayDim = 0x38;
    static constexpr size_t kElementSize = 0x3C;
    static constexpr size_t kOffsetInternal = 0x4C;
};

// One reflected property: where it lives in an instance, and how big it is.
struct PropertyInfo {
    std::string name;
    int32_t offset = -1;
    int32_t size = 0;
    int32_t arrayDim = 0;
    bool valid() const { return offset >= 0; }
};

// A discovered object, flattened into something safe to hold across frames.
// Raw UObject pointers are deliberately paired with their serial number: the
// engine reuses slots after garbage collection, so a pointer alone is not an
// identity.
struct ObjectRef {
    void* object = nullptr;
    int32_t index = 0;
    int32_t serialNumber = 0;
    uint32_t nameIndex = 0;
    std::string name;
    std::string className;
};

class Engine {
public:
    // Locates the host module and searches for GUObjectArray. Returns false and
    // logs the specific validation step that failed.
    bool Discover();

    bool ready() const { return objects_ != nullptr; }
    int32_t NumObjects() const;

    // Resolves one object by index. Returns false for empty slots, which are
    // normal in a live array.
    bool GetObject(int32_t index, ObjectRef& out) const;

    // Resolves an FName comparison index to text.
    std::string NameToString(uint32_t comparisonIndex) const;

    // Walks the array and logs a class histogram plus rejection counts.
    // Callable repeatedly: the discovered addresses are stable, but the array
    // contents are not -- at engine startup it holds only bootstrap classes and
    // packages, and gameplay actors appear later.
    void ReportSample(const char* label);

    // Counts objects whose class chain reaches the named class. This is how we
    // find candidates for CustomDepth: bRenderCustomDepth lives on
    // UPrimitiveComponent, so anything derived from it can be marked.
    int CountDerivedFrom(const char* className);

    // True if obj's class chain reaches `className`. Used both to select
    // primitives and to prove a ProcessEvent candidate by checking its first
    // argument really is a UFunction.
    bool IsDerivedFrom(void* obj, const char* className) const;

    // Address of any live UObject, for reading a vtable. Returns nullptr if
    // discovery has not run.
    void* AnyObject() const;

    // Finds a UClass by name by scanning the object array.
    void* FindClass(const char* className);

    // Every reflected property on a class, including inherited ones (walks the
    // SuperStruct chain). Used both to look properties up and to VERIFY the
    // reflection layout: if the names come back recognisable, the offsets above
    // are right for this build.
    std::vector<PropertyInfo> ListProperties(void* uclass, bool includeInherited = true);

    // Looks up a single property by name. Returns an invalid PropertyInfo if
    // absent -- which is a real answer, not an error: it means this build does
    // not have that property and we must not write to a guessed location.
    PropertyInfo FindProperty(void* uclass, const char* propertyName);

    void* guObjectArray() const { return arrayAddress_; }
    bool namesResolved() const { return nameBlocks_ != nullptr; }

private:
    bool FindObjectArray();
    bool FindNamePool();
    bool ValidateArrayCandidate(const FChunkedFixedUObjectArray* candidate) const;
    bool LooksLikeUObject(void* obj) const;

    FChunkedFixedUObjectArray* objects_ = nullptr;
    void* arrayAddress_ = nullptr;
    uint8_t** nameBlocks_ = nullptr;

    uintptr_t moduleBase_ = 0;
    size_t moduleSize_ = 0;
    uintptr_t textStart_ = 0;
    uintptr_t textEnd_ = 0;
};

// True if the address is readable without faulting. Every dereference during
// discovery goes through this: we are walking a live process's memory looking
// for a structure by shape, so touching a bad address is expected, not
// exceptional, and must not take the game down.
bool IsReadable(const void* p, size_t bytes);

// ---------------------------------------------------------------------------
// ProcessEvent -- a safe execution point on the game thread.
//
// Everything else in this file only READS. Setting bRenderCustomDepth is a
// write, and UObject state is owned by the game thread: writing it from the
// render thread (where our D3D hooks run) is a data race that might work a
// thousand times and then corrupt something.
//
// UObject::ProcessEvent is Unreal's central dispatch for Blueprint and native
// calls. It runs on the game thread, constantly. Hooking it gives us a legal
// moment to touch object state.
//
// HONEST CAVEAT: unlike GUObjectArray and FNamePool, this cannot be found by
// structural search -- a vtable slot is just a function pointer with no shape to
// match. So the vtable index is a HYPOTHESIS, proven at runtime by checking that
// the first argument is a real UFunction (its class chain must reach "Function").
// This is the one version-keyed constant in the project, and it is never trusted
// without that proof.
// ---------------------------------------------------------------------------
class ProcessEventHook {
public:
    // Work to run on the game thread. Called from inside ProcessEvent, so it
    // must be short -- this runs in the middle of the engine's dispatch path.
    using GameThreadTask = std::function<void(Engine&)>;

    // Installs the hook, trying candidate vtable indices until one proves
    // itself. Returns false with the reason logged.
    bool Install(Engine& engine);
    void Uninstall();

    // Queues work for the next ProcessEvent call. Thread-safe.
    void RunOnGameThread(GameThreadTask task);

    bool verified() const { return verified_; }
    int vtableIndex() const { return vtableIndex_; }
    unsigned long gameThreadId() const { return gameThreadId_; }
    uint64_t callCount() const { return callCount_; }

private:
    bool TryIndex(Engine& engine, int index);

    int vtableIndex_ = -1;
    bool verified_ = false;
    unsigned long gameThreadId_ = 0;
    uint64_t callCount_ = 0;
};

ProcessEventHook& GetProcessEventHook();

}  // namespace ue4
}  // namespace segcap
