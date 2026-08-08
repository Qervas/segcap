#include "ue4.h"

#include <psapi.h>

#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

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

// Decodes an FNameEntry at `entry` into text. Returns false if the header does
// not describe a plausible name, which is also how candidate validation works.
static bool DecodeNameEntry(const uint8_t* entry, std::string& out) {
    if (!IsReadable(entry, 2)) return false;
    const uint16_t header = *reinterpret_cast<const uint16_t*>(entry);
    const uint32_t len = header >> kNameLenShift;
    const bool wide = (header & 1) != 0;

    // FName length is capped at 1024 by the engine; a header claiming more is
    // not a name.
    if (len == 0 || len > 1024) return false;

    const size_t bytes = wide ? len * 2 : len;
    if (!IsReadable(entry + 2, bytes)) return false;

    out.clear();
    out.reserve(len);
    if (wide) {
        const auto* w = reinterpret_cast<const wchar_t*>(entry + 2);
        for (uint32_t i = 0; i < len; ++i) out.push_back(static_cast<char>(w[i] & 0x7F));
    } else {
        const auto* a = reinterpret_cast<const char*>(entry + 2);
        for (uint32_t i = 0; i < len; ++i) {
            const char c = a[i];
            // Names are identifiers and paths; a control byte means we are not
            // looking at a name.
            if (static_cast<unsigned char>(c) < 0x20) return false;
            out.push_back(c);
        }
    }
    return true;
}

bool Engine::FindNamePool() {
    // The signature here is unusually strong: FName id 0 is always NAME_None,
    // which lives at block 0 offset 0. So we look for a pointer whose target
    // decodes to a 4-character narrow name spelling exactly "None". Random data
    // passing that is vanishingly unlikely, and unlike a byte-pattern signature
    // it is a property of the engine's semantics rather than of one build.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase_);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase_ + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);

    const ULONGLONG started = GetTickCount64();

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const bool writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        const bool executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (!writable || executable) continue;

        const uintptr_t begin = moduleBase_ + section->VirtualAddress;
        const uintptr_t end = begin + section->Misc.VirtualSize - sizeof(void*);

        for (uintptr_t addr = begin; addr < end; addr += sizeof(void*)) {
            if (!IsReadable(reinterpret_cast<void*>(addr), sizeof(void*))) continue;
            auto* block0 = *reinterpret_cast<uint8_t**>(addr);
            if (!IsReadable(block0, 8)) continue;

            std::string name;
            if (!DecodeNameEntry(block0, name)) continue;
            if (name != "None") continue;

            // Corroborate before accepting: the next few blocks should also be
            // readable pointers holding decodable names. A lone "None" could be
            // a coincidence; a whole table of names is not.
            auto** blocks = reinterpret_cast<uint8_t**>(addr);
            int corroborated = 0;
            for (int b = 1; b < 6; ++b) {
                if (!IsReadable(&blocks[b], sizeof(void*))) break;
                if (!IsReadable(blocks[b], 8)) break;
                std::string probe;
                if (DecodeNameEntry(blocks[b], probe)) ++corroborated;
            }
            if (corroborated < 2) continue;

            nameBlocks_ = blocks;
            LogInfo("ue4: FOUND FName blocks at %p (FNamePool ~%p) after %llums",
                    static_cast<void*>(blocks),
                    reinterpret_cast<void*>(addr - kNameBlocksOffsetInPool),
                    GetTickCount64() - started);
            LogInfo("ue4:   block0[0]=\"%s\", %d further blocks corroborated",
                    name.c_str(), corroborated);
            return true;
        }
    }

    LogError("ue4: FNamePool not found (%llums)", GetTickCount64() - started);
    return false;
}

std::string Engine::NameToString(uint32_t comparisonIndex) const {
    if (!nameBlocks_) return std::string();

    const uint32_t block = comparisonIndex >> kNameBlockOffsetBits;
    const uint32_t offset = comparisonIndex & kNameBlockOffsetMask;
    if (block >= 8192) return std::string();

    if (!IsReadable(&nameBlocks_[block], sizeof(void*))) return std::string();
    const uint8_t* base = nameBlocks_[block];
    if (!base) return std::string();

    std::string out;
    if (!DecodeNameEntry(base + static_cast<size_t>(offset) * kNameEntryStride, out)) {
        return std::string();
    }
    return out;
}

void Engine::ReportSample(const char* label) {
    if (!objects_) return;

    // Rebuild the readable-region map before every walk.
    //
    // It was previously built once at discovery. When Stray's level streamed in,
    // the object array grew 196k -> 319k slots and UE allocated new chunks in
    // regions that did not exist when the map was made -- so IsReadable rejected
    // them and 8165 objects of a 21273 sample silently disappeared. The symptom
    // was a resolved count that DROPPED as the game loaded more content, which
    // is the opposite of what should happen.
    BuildReadableMap();

    // Rejection counters, because "340 of 21699 resolved" is a symptom and not
    // a diagnosis. Knowing WHICH check rejected an entry is the difference
    // between "the array is mostly empty" and "my validation is too strict".
    int nullChunk = 0, unreadableChunk = 0, nullObject = 0, failedValidation = 0, ok = 0;

    std::vector<std::pair<std::string, int>> counts;
    const int32_t total = objects_->NumElements;
    const int32_t stride = total > 20000 ? total / 20000 : 1;
    int sampled = 0;

    for (int32_t i = 0; i < total; i += stride) {
        ++sampled;
        const int32_t chunkIndex = i / kElementsPerChunk;
        const int32_t withinChunk = i % kElementsPerChunk;
        if (chunkIndex >= objects_->NumChunks) { ++nullChunk; continue; }

        FUObjectItem* chunk = objects_->Objects[chunkIndex];
        if (!chunk) { ++nullChunk; continue; }
        if (!IsReadable(&chunk[withinChunk], sizeof(FUObjectItem))) { ++unreadableChunk; continue; }

        void* obj = chunk[withinChunk].Object;
        if (!obj) { ++nullObject; continue; }
        if (!LooksLikeUObject(obj)) { ++failedValidation; continue; }
        ++ok;

        ObjectRef ref;
        if (!GetObject(i, ref) || ref.className.empty()) continue;
        bool found = false;
        for (auto& c : counts) {
            if (c.first == ref.className) { ++c.second; found = true; break; }
        }
        if (!found && counts.size() < 512) counts.emplace_back(ref.className, 1);
    }

    std::sort(counts.begin(), counts.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    LogInfo("ue4: [%s] array has %d slots; sampled %d (every %d)", label, total, sampled, stride);
    LogInfo("ue4:   resolved=%d  nullObject=%d  failedValidation=%d  nullChunk=%d  unreadable=%d",
            ok, nullObject, failedValidation, nullChunk, unreadableChunk);
    for (size_t i = 0; i < counts.size() && i < 14; ++i) {
        LogInfo("ue4:     %6d  %s", counts[i].second, counts[i].first.c_str());
    }
}

int Engine::CountDerivedFrom(const char* target) {
    if (!objects_ || !nameBlocks_) return 0;
    BuildReadableMap();  // see ReportSample: the map goes stale as the level streams in

    int matches = 0;
    const int32_t total = objects_->NumElements;
    // Breakdown by concrete class, because "622 primitives" does not tell us
    // whether they are static meshes worth marking or invisible collision
    // volumes. Task 8 needs to know what it is actually flagging.
    std::vector<std::pair<std::string, int>> byClass;

    for (int32_t i = 0; i < total; ++i) {
        ObjectRef ref;
        if (!GetObject(i, ref)) continue;

        // Walk the UClass superclass chain. UStruct::SuperStruct sits at 0x40 in
        // UE4 x64 shipping; if that offset is wrong the walk terminates
        // immediately rather than misreporting, because the pointer will fail
        // the UObject-shape test.
        auto klass = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(ref.object) + UObjectLayout::kClassPrivate);
        for (int depth = 0; depth < 24 && klass; ++depth) {
            if (!LooksLikeUObject(reinterpret_cast<void*>(klass))) break;
            const auto nameIdx = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(klass) + UObjectLayout::kNamePrivate);
            if (NameToString(nameIdx) == target) {
                ++matches;
                bool found = false;
                for (auto& c : byClass) {
                    if (c.first == ref.className) { ++c.second; found = true; break; }
                }
                if (!found && byClass.size() < 256) byClass.emplace_back(ref.className, 1);
                break;
            }
            klass = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(klass) + 0x40);
        }
    }

    std::sort(byClass.begin(), byClass.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    LogInfo("ue4: %d objects derive from %s:", matches, target);
    for (size_t i = 0; i < byClass.size() && i < 16; ++i) {
        LogInfo("ue4:     %5d  %s", byClass[i].second, byClass[i].first.c_str());
    }
    return matches;
}

bool Engine::IsDerivedFrom(void* obj, const char* target) const {
    if (!obj || !nameBlocks_ || !LooksLikeUObject(obj)) return false;
    auto klass = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uint8_t*>(obj) + UObjectLayout::kClassPrivate);
    for (int depth = 0; depth < 24 && klass; ++depth) {
        if (!LooksLikeUObject(reinterpret_cast<void*>(klass))) return false;
        const auto nameIdx = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(klass) + UObjectLayout::kNamePrivate);
        if (NameToString(nameIdx) == target) return true;
        klass = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(klass) + 0x40);
    }
    return false;
}

void* Engine::AnyObject() const {
    if (!objects_) return nullptr;
    for (int32_t i = 0; i < objects_->NumElements && i < 4096; ++i) {
        const int32_t chunkIndex = i / kElementsPerChunk;
        if (chunkIndex >= objects_->NumChunks) break;
        FUObjectItem* chunk = objects_->Objects[chunkIndex];
        if (!IsReadable(chunk, sizeof(FUObjectItem) * static_cast<size_t>(i + 1))) continue;
        void* obj = chunk[i].Object;
        if (obj && LooksLikeUObject(obj)) return obj;
    }
    return nullptr;
}

void* Engine::FindClass(const char* className) {
    if (!objects_ || !nameBlocks_) return nullptr;

    void* stub = nullptr;
    int matches = 0;

    for (int32_t i = 0; i < objects_->NumElements; ++i) {
        ObjectRef ref;
        if (!GetObject(i, ref)) continue;
        if (ref.className != "Class") continue;   // must BE a UClass, not an instance
        if (ref.name != className) continue;
        ++matches;

        // The name is not enough. Stray's object array contains a UClass named
        // "PrimitiveComponent" whose Children AND ChildProperties are both null
        // -- structurally coherent (its inheritance depth is correct) but
        // unpopulated. Returning it produced "0 properties declared" and sent
        // me looking for a layout bug that did not exist.
        //
        // A real native class has a property chain. Require one.
        const auto base = reinterpret_cast<uintptr_t>(ref.object);
        const auto children = *reinterpret_cast<uintptr_t*>(base + 0x48);
        const auto childProps =
            *reinterpret_cast<uintptr_t*>(base + UStructLayout::kChildProperties);
        if (!children && !childProps) {
            if (!stub) stub = ref.object;
            continue;
        }
        if (matches > 1) {
            LogInfo("ue4: FindClass(%s): skipped %d stub(s) with null chains",
                    className, matches - 1);
        }
        return ref.object;
    }

    if (stub) {
        LogWarn("ue4: FindClass(%s): only found %d candidate(s), all with null "
                "property chains -- returning nothing rather than a stub",
                className, matches);
    }
    return nullptr;
}

std::vector<PropertyInfo> Engine::ListProperties(void* uclass, bool includeInherited) {
    std::vector<PropertyInfo> out;
    if (!uclass || !nameBlocks_) return out;

    auto structPtr = reinterpret_cast<uintptr_t>(uclass);
    for (int depth = 0; depth < 32 && structPtr; ++depth) {
        if (!LooksLikeUObject(reinterpret_cast<void*>(structPtr))) break;

        auto field = *reinterpret_cast<uintptr_t*>(structPtr + UStructLayout::kChildProperties);
        // Bounded: a corrupt or misinterpreted Next pointer would otherwise spin
        // forever, and we are walking memory whose layout is a hypothesis.
        for (int n = 0; n < 4096 && field; ++n) {
            if (!IsReadable(reinterpret_cast<void*>(field), 0x50)) break;

            const auto nameIdx =
                *reinterpret_cast<uint32_t*>(field + FFieldLayout::kNamePrivate);
            const int32_t offset =
                *reinterpret_cast<int32_t*>(field + FPropertyLayout::kOffsetInternal);
            const int32_t size =
                *reinterpret_cast<int32_t*>(field + FPropertyLayout::kElementSize);
            const int32_t dim =
                *reinterpret_cast<int32_t*>(field + FPropertyLayout::kArrayDim);

            std::string name = NameToString(nameIdx);
            // Sanity-bound the values. A property at offset 900000 or of size
            // -3 means the layout hypothesis is wrong, and it is far better to
            // stop than to hand back a plausible-looking address to write to.
            if (!name.empty() && offset >= 0 && offset < 0x40000 && size > 0 && size <= 0x10000) {
                PropertyInfo info;
                info.name = std::move(name);
                info.offset = offset;
                info.size = size;
                info.arrayDim = dim;

                // Identify the property kind from FFieldClass::Name rather than
                // any hardcoded type tag.
                const auto fieldClass =
                    *reinterpret_cast<uintptr_t*>(field + kFFieldClassPrivate);
                if (IsReadable(reinterpret_cast<void*>(fieldClass), 8)) {
                    info.type = NameToString(*reinterpret_cast<uint32_t*>(fieldClass));
                }

                if (info.type == "BoolProperty" &&
                    IsReadable(reinterpret_cast<void*>(field + FBoolPropertyLayout::kFieldMask), 1)) {
                    info.isBool = true;
                    info.byteOffset =
                        *reinterpret_cast<uint8_t*>(field + FBoolPropertyLayout::kByteOffset);
                    info.fieldMask =
                        *reinterpret_cast<uint8_t*>(field + FBoolPropertyLayout::kFieldMask);
                }

                out.push_back(std::move(info));
            }
            field = *reinterpret_cast<uintptr_t*>(field + FFieldLayout::kNext);
        }

        if (!includeInherited) break;
        structPtr = *reinterpret_cast<uintptr_t*>(structPtr + UStructLayout::kSuperStruct);
    }
    return out;
}

void Engine::DumpStructLayout(void* ustruct, const char* label) {
    if (!ustruct || !nameBlocks_) return;
    auto base = reinterpret_cast<uintptr_t>(ustruct);
    LogInfo("ue4: --- layout probe: %s @ %p ---", label, ustruct);

    // Print EVERY qword, including ones neither interpretation decodes. An
    // earlier version only printed successful decodes, which meant a null
    // pointer and a present-but-misinterpreted pointer looked identical -- the
    // same class of filter-hides-the-evidence mistake this probe exists to
    // avoid. "Children is null" and "Children decodes wrong" need completely
    // different fixes.
    for (size_t off = 0; off < 0x120; off += 8) {
        if (!IsReadable(reinterpret_cast<void*>(base + off), 8)) {
            LogInfo("ue4:   +0x%03zX  <unreadable>", off);
            continue;
        }
        const auto value = *reinterpret_cast<uintptr_t*>(base + off);
        if (value == 0) continue;   // null is the one uninformative case

        const bool ptr = IsReadable(reinterpret_cast<void*>(value), 0x40);
        std::string asUObject, asFField;

        // (a) UObject (pre-4.25 UProperty is one): FName at +0x18.
        if (ptr && LooksLikeUObject(reinterpret_cast<void*>(value))) {
            asUObject = NameToString(
                *reinterpret_cast<uint32_t*>(value + UObjectLayout::kNamePrivate));
        }
        // (b) FField (4.25+ FProperty): FName at +0x28, NOT a UObject.
        if (ptr) {
            const std::string s =
                NameToString(*reinterpret_cast<uint32_t*>(value + 0x28));
            if (!s.empty() && s.size() < 64) asFField = s;
        }

        LogInfo("ue4:   +0x%03zX = %016llX %s U:\"%s\" F:\"%s\"", off,
                static_cast<unsigned long long>(value), ptr ? "ptr" : "   ",
                asUObject.empty() ? "-" : asUObject.c_str(),
                asFField.empty() ? "-" : asFField.c_str());
    }
    LogInfo("ue4: --- end probe ---");
}

PropertyInfo Engine::FindProperty(void* uclass, const char* propertyName) {
    for (const PropertyInfo& p : ListProperties(uclass, true)) {
        if (p.name == propertyName) return p;
    }
    return PropertyInfo{};
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

    // Name resolution is not fatal if it fails: object discovery still works,
    // and the failure is far more useful reported than papered over.
    if (!FindNamePool()) {
        LogWarn("ue4: continuing without name resolution");
    }

    LogInfo("ue4: discovery complete, %d slots in the array", NumObjects());
    return true;
}

// ---------------------------------------------------------------------------
// ProcessEvent
// ---------------------------------------------------------------------------

namespace {

// Candidate vtable indices for UObject::ProcessEvent in UE4 x64 shipping. The
// exact slot moves with engine version and with which virtuals the game's build
// includes, so a window is tried rather than a single number -- and every
// candidate must PROVE itself before being kept.
constexpr int kProcessEventCandidateFirst = 60;
constexpr int kProcessEventCandidateLast = 80;

// How many correctly-shaped calls a candidate must produce before we believe it.
// One could be luck; a steady stream through the engine's dispatch path is not.
constexpr uint64_t kProofThreshold = 24;

Engine* g_engine = nullptr;
ProcessEventHook* g_hook = nullptr;

// Four parameters, not three. The real signature is
// ProcessEvent(this, UFunction*, void*), but while SEARCHING we may briefly be
// attached to some other virtual that takes more arguments. Declaring four
// keeps RCX/RDX/R8/R9 faithfully passed through, so a wrong candidate is a
// no-op rather than a corrupted call. Virtuals with more than four register
// arguments are the residual risk, and are rare.
using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*);
ProcessEventFn g_origProcessEvent = nullptr;

std::mutex g_queueMutex;
std::vector<ProcessEventHook::GameThreadTask> g_queue;
uint64_t g_validCalls = 0;
uint64_t g_invalidCalls = 0;

void __fastcall ProcessEventDetour(void* self, void* function, void* parms, void* spare) {
    // The proof: ProcessEvent's second argument is always a UFunction. If this
    // slot is some other virtual, that argument will be an int, a pointer to
    // something else, or garbage -- and the class-chain walk will reject it.
    if (g_engine && g_engine->IsDerivedFrom(function, "Function")) {
        ++g_validCalls;
        if (g_hook) {
            // Drain queued work here: we are on the game thread, inside the
            // engine's own dispatch, which is exactly the safe point we needed.
            std::vector<ProcessEventHook::GameThreadTask> todo;
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                todo.swap(g_queue);
            }
            for (auto& task : todo) task(*g_engine);
        }
    } else {
        ++g_invalidCalls;
    }
    g_origProcessEvent(self, function, parms, spare);
}

}  // namespace

bool ProcessEventHook::TryIndex(Engine& engine, int index) {
    void* sample = engine.AnyObject();
    if (!sample) return false;

    auto** vtable = *reinterpret_cast<void***>(sample);
    if (!IsReadable(&vtable[index], sizeof(void*))) return false;
    void* target = vtable[index];
    if (!IsReadable(target, 16)) return false;

    g_validCalls = 0;
    g_invalidCalls = 0;

    if (MH_CreateHook(target, reinterpret_cast<void*>(&ProcessEventDetour),
                      reinterpret_cast<void**>(&g_origProcessEvent)) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }

    // Observe. A correct candidate produces a steady stream of calls whose
    // second argument validates as a UFunction.
    Sleep(1500);

    const uint64_t valid = g_validCalls;
    const uint64_t invalid = g_invalidCalls;

    // Accept only on a clear signal: enough proven calls, and overwhelmingly
    // more valid than invalid. A slot that is called often with arguments that
    // do not validate is a different virtual, not a noisy ProcessEvent.
    const bool accept = valid >= kProofThreshold && valid > invalid * 4;
    LogInfo("ue4: PE candidate %d -> %llu valid, %llu invalid %s", index, valid, invalid,
            accept ? "ACCEPTED" : "(rejected)");

    if (!accept) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_origProcessEvent = nullptr;
        return false;
    }

    vtableIndex_ = index;
    verified_ = true;
    return true;
}

bool ProcessEventHook::Install(Engine& engine) {
    if (verified_) return true;
    if (!engine.ready() || !engine.namesResolved()) {
        LogError("ue4: ProcessEvent needs discovery and name resolution first");
        return false;
    }

    g_engine = &engine;
    g_hook = this;

    LogInfo("ue4: searching vtable slots %d..%d for ProcessEvent",
            kProcessEventCandidateFirst, kProcessEventCandidateLast);

    for (int i = kProcessEventCandidateFirst; i <= kProcessEventCandidateLast; ++i) {
        if (TryIndex(engine, i)) {
            gameThreadId_ = GetCurrentThreadId();  // provisional; corrected on first call
            LogInfo("ue4: ProcessEvent CONFIRMED at vtable index %d", i);
            return true;
        }
    }

    LogError("ue4: ProcessEvent not found in slots %d..%d",
             kProcessEventCandidateFirst, kProcessEventCandidateLast);
    return false;
}

void ProcessEventHook::Uninstall() {
    // Deliberately left installed for the process lifetime -- see dllmain's
    // note on removing trampolines out from under running threads.
    verified_ = false;
}

void ProcessEventHook::RunOnGameThread(GameThreadTask task) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(std::move(task));
}

ProcessEventHook& GetProcessEventHook() {
    static ProcessEventHook instance;
    return instance;
}

}  // namespace ue4
}  // namespace segcap
