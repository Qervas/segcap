#include "ue4.h"

#include <psapi.h>

#include "log.h"

namespace segcap {
namespace ue4 {
namespace {

// A private cache of committed, readable regions. VirtualQuery per dereference
// would be far too slow for a scan that touches millions of addresses, and
// structured exception handling around every read is both slower and easier to
// get wrong.
struct ReadableRange {
    uintptr_t begin;
    uintptr_t end;
};
std::vector<ReadableRange> g_readable;

void BuildReadableMap() {
    g_readable.clear();
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);

    auto addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION mbi = {};
    while (addr < maxAddr && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const bool committed = mbi.State == MEM_COMMIT;
        const bool readable =
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

        if (committed && readable && !guarded) {
            const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (!g_readable.empty() && g_readable.back().end == base) {
                g_readable.back().end = base + mbi.RegionSize;  // coalesce
            } else {
                g_readable.push_back({base, base + mbi.RegionSize});
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.RegionSize == 0) break;
    }
    LogInfo("ue4: %zu readable regions mapped", g_readable.size());
}

}  // namespace

bool IsReadable(const void* p, size_t bytes) {
    if (!p) return false;
    const auto a = reinterpret_cast<uintptr_t>(p);
    // Reject obviously bogus low addresses before searching.
    if (a < 0x10000) return false;

    // Binary search, not a linear walk. Discovery performs millions of
    // dereference checks; at a few thousand regions a linear scan turns this
    // into tens of billions of comparisons and the thread appears to hang.
    size_t lo = 0, hi = g_readable.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (g_readable[mid].end <= a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < g_readable.size() && a >= g_readable[lo].begin &&
           a + bytes <= g_readable[lo].end;
}

bool Engine::LooksLikeUObject(void* obj) const {
    if (!IsReadable(obj, 0x30)) return false;

    // A UObject's first field is a vtable pointer, which must point into the
    // host module's executable section. This single check eliminates almost
    // every false positive: random heap data rarely begins with a pointer into
    // the game's own code.
    const auto vtable = *reinterpret_cast<uintptr_t*>(obj);
    if (!IsReadable(reinterpret_cast<void*>(vtable), 8)) return false;
    const auto firstMethod = *reinterpret_cast<uintptr_t*>(vtable);
    if (firstMethod < textStart_ || firstMethod >= textEnd_) return false;

    // ClassPrivate must itself be a UObject-shaped thing.
    const auto klass = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uint8_t*>(obj) + UObjectLayout::kClassPrivate);
    if (!IsReadable(reinterpret_cast<void*>(klass), 0x30)) return false;
    const auto classVTable = *reinterpret_cast<uintptr_t*>(klass);
    if (!IsReadable(reinterpret_cast<void*>(classVTable), 8)) return false;
    const auto classFirstMethod = *reinterpret_cast<uintptr_t*>(classVTable);
    if (classFirstMethod < textStart_ || classFirstMethod >= textEnd_) return false;

    return true;
}

bool Engine::ValidateArrayCandidate(const FChunkedFixedUObjectArray* c) const {
    // --- field plausibility -------------------------------------------------
    if (c->NumElements <= 0 || c->MaxElements <= 0) return false;
    if (c->NumChunks <= 0 || c->MaxChunks <= 0) return false;
    if (c->NumElements > c->MaxElements) return false;
    if (c->NumChunks > c->MaxChunks) return false;

    // A real game has thousands of objects, not five. This rejects small
    // coincidental structures cheaply.
    if (c->NumElements < 1000) return false;
    // Sanity ceiling: 100M objects would be absurd and indicates garbage.
    if (c->MaxElements > 100 * 1000 * 1000) return false;

    // The chunk arithmetic is the strongest structural invariant available:
    // MaxElements must be exactly MaxChunks * kElementsPerChunk, and NumChunks
    // must be the number of chunks NumElements actually spans.
    if (c->MaxElements != c->MaxChunks * kElementsPerChunk) return false;
    const int32_t expectedChunks =
        (c->NumElements + kElementsPerChunk - 1) / kElementsPerChunk;
    if (c->NumChunks != expectedChunks) return false;

    // --- dereference chain --------------------------------------------------
    if (!IsReadable(c->Objects, sizeof(void*) * static_cast<size_t>(c->NumChunks))) return false;

    FUObjectItem* chunk0 = c->Objects[0];
    if (!IsReadable(chunk0, sizeof(FUObjectItem) * 4)) return false;

    // Walk a few early slots; some are legitimately null, so require that a
    // handful of the first entries look like real UObjects rather than all.
    int good = 0;
    for (int i = 0; i < 32; ++i) {
        void* obj = chunk0[i].Object;
        if (!obj) continue;
        if (LooksLikeUObject(obj)) ++good;
    }
    return good >= 8;
}

bool Engine::FindObjectArray() {
    // Scan only WRITABLE data sections, not the whole image. GUObjectArray is a
    // mutable global, so .text and .rdata cannot contain it, and skipping them
    // removes the overwhelming majority of a ~200MB module. Scanning everything
    // was not just slower -- at 8-byte stride with a readability check per
    // address it was slow enough to look like a hang.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase_);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase_ + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);

    size_t examined = 0;
    const ULONGLONG started = GetTickCount64();

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const bool writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        const bool executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (!writable || executable) continue;

        const uintptr_t begin = moduleBase_ + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < sizeof(FChunkedFixedUObjectArray)) continue;

        char name[9] = {};
        memcpy(name, section->Name, 8);
        LogInfo("ue4: scanning section %-8s %p (%.2f MB)", name,
                reinterpret_cast<void*>(begin), size / (1024.0 * 1024.0));

        const uintptr_t end = begin + size - sizeof(FChunkedFixedUObjectArray);
        for (uintptr_t addr = begin; addr < end; addr += sizeof(void*)) {
            if (!IsReadable(reinterpret_cast<void*>(addr), sizeof(FChunkedFixedUObjectArray)))
                continue;
            ++examined;
            auto* candidate = reinterpret_cast<FChunkedFixedUObjectArray*>(addr);
            if (!ValidateArrayCandidate(candidate)) continue;

            objects_ = candidate;
            // FUObjectArray places the chunked array at +0x10, after three
            // int32s and a bool. Reported for cross-checking against public
            // offsets, but nothing here depends on it.
            arrayAddress_ = reinterpret_cast<void*>(addr - 0x10);
            LogInfo("ue4: FOUND object array at %p (GUObjectArray ~%p) after %llums",
                    static_cast<void*>(candidate), arrayAddress_,
                    GetTickCount64() - started);
            LogInfo("ue4:   NumElements=%d MaxElements=%d NumChunks=%d MaxChunks=%d",
                    candidate->NumElements, candidate->MaxElements, candidate->NumChunks,
                    candidate->MaxChunks);
            return true;
        }
    }

    LogError("ue4: no FChunkedFixedUObjectArray found (%zu addresses examined, %llums)",
             examined, GetTickCount64() - started);
    return false;
}

int32_t Engine::NumObjects() const {
    if (!objects_) return 0;
    return objects_->NumElements;
}

bool Engine::GetObject(int32_t index, ObjectRef& out) const {
    if (!objects_ || index < 0 || index >= objects_->NumElements) return false;

    const int32_t chunkIndex = index / kElementsPerChunk;
    const int32_t withinChunk = index % kElementsPerChunk;
    if (chunkIndex >= objects_->NumChunks) return false;

    FUObjectItem* chunk = objects_->Objects[chunkIndex];
    if (!IsReadable(chunk, sizeof(FUObjectItem) * static_cast<size_t>(withinChunk + 1)))
        return false;

    const FUObjectItem& item = chunk[withinChunk];
    if (!item.Object || !LooksLikeUObject(item.Object)) return false;

    auto* base = reinterpret_cast<uint8_t*>(item.Object);
    out.object = item.Object;
    out.index = index;
    // Serial number is carried alongside the pointer because the engine reuses
    // slots after GC. Pointer identity alone would silently merge two different
    // objects into one -- the exact failure the mask stream must not have.
    out.serialNumber = item.SerialNumber;
    out.nameIndex = *reinterpret_cast<uint32_t*>(base + UObjectLayout::kNamePrivate);
    out.name = NameToString(out.nameIndex);

    const auto klass = *reinterpret_cast<uintptr_t*>(base + UObjectLayout::kClassPrivate);
    if (IsReadable(reinterpret_cast<void*>(klass), 0x30)) {
        const auto classNameIndex = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(klass) + UObjectLayout::kNamePrivate);
        out.className = NameToString(classNameIndex);
    }
    return true;
}

std::string Engine::NameToString(uint32_t /*comparisonIndex*/) const {
    // Deliberately unimplemented for now rather than guessed at. FName storage
    // changed shape in 4.23 (TNameEntryArray -> FNamePool) and the block/offset
    // encoding differs again by minor version. Returning a fabricated string
    // here would produce a mask stream whose labels look right and are wrong,
    // which is worse than having no labels.
    //
    // Object discovery does not depend on this; names are needed for the sidecar
    // table, and are the next piece of work.
    return std::string();
}

bool Engine::Discover() {
    BuildReadableMap();

    moduleBase_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase_) {
        LogError("ue4: GetModuleHandle(nullptr) failed");
        return false;
    }

    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(),
                              reinterpret_cast<HMODULE>(moduleBase_), &mi, sizeof(mi))) {
        LogError("ue4: GetModuleInformation failed");
        return false;
    }
    moduleSize_ = mi.SizeOfImage;

    // Locate the executable section so vtable pointers can be validated as
    // pointing at real code in this module.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase_);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase_ + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            textStart_ = moduleBase_ + section->VirtualAddress;
            textEnd_ = textStart_ + section->Misc.VirtualSize;
            break;
        }
    }
    if (!textStart_) {
        LogError("ue4: no executable section found");
        return false;
    }

    LogInfo("ue4: module %p size %.1f MB, code %p..%p",
            reinterpret_cast<void*>(moduleBase_), moduleSize_ / (1024.0 * 1024.0),
            reinterpret_cast<void*>(textStart_), reinterpret_cast<void*>(textEnd_));

    if (!FindObjectArray()) return false;

    // Report a sample so the discovery can be sanity-checked by eye rather than
    // trusted. Names are empty until FName resolution lands; class pointers and
    // indices are still meaningful.
    int reported = 0;
    for (int32_t i = 0; i < objects_->NumElements && reported < 8; ++i) {
        ObjectRef ref;
        if (!GetObject(i, ref)) continue;
        LogInfo("ue4:   [%d] obj=%p serial=%d nameIdx=%u", ref.index, ref.object,
                ref.serialNumber, ref.nameIndex);
        ++reported;
    }
    LogInfo("ue4: discovery complete, %d live objects", NumObjects());
    return true;
}

}  // namespace ue4
}  // namespace segcap
