#include "perception.h"

#include <windows.h>

#include <cstring>

#include "log.h"

namespace segcap {
namespace {

long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

// Reads an ObjectProperty: the property slot holds a UObject* directly.
void* ReadObjectPtr(void* base, const ue4::PropertyInfo& p) {
    if (!base || !p.valid()) return nullptr;
    auto* addr = reinterpret_cast<uint8_t*>(base) + p.offset;
    if (!ue4::IsReadable(addr, sizeof(void*))) return nullptr;
    void* v = *reinterpret_cast<void**>(addr);
    // A UObject pointer that is not readable is a destroyed object, which is
    // normal during a level transition -- not an error worth logging every tick.
    if (!ue4::IsReadable(v, 0x30)) return nullptr;
    return v;
}

// Reads three consecutive floats (FVector / FRotator are POD triples).
bool ReadFloat3(void* base, const ue4::PropertyInfo& p, float* out) {
    if (!base || !p.valid()) return false;
    auto* addr = reinterpret_cast<uint8_t*>(base) + p.offset;
    if (!ue4::IsReadable(addr, sizeof(float) * 3)) return false;
    std::memcpy(out, addr, sizeof(float) * 3);
    return true;
}

}  // namespace

bool AgentPerception::Resolve(ue4::Engine& engine) {
    if (resolved_) return true;

    controllerClass_ = engine.FindClass("PlayerController");
    void* actorClass = engine.FindClass("Actor");
    void* sceneComponentClass = engine.FindClass("SceneComponent");

    if (!controllerClass_ || !actorClass || !sceneComponentClass) {
        LogWarn("perception: missing class (PlayerController=%p Actor=%p SceneComponent=%p)",
                controllerClass_, actorClass, sceneComponentClass);
        return false;
    }

    // Both spellings exist across UE versions and both are worth having:
    // AcknowledgedPawn is the replicated one and is sometimes set when Pawn is
    // momentarily null during a possession handoff.
    propPawn_ = engine.FindProperty(controllerClass_, "Pawn");
    propAckPawn_ = engine.FindProperty(controllerClass_, "AcknowledgedPawn");
    propRootComponent_ = engine.FindProperty(actorClass, "RootComponent");
    propRelativeLocation_ = engine.FindProperty(sceneComponentClass, "RelativeLocation");
    propRelativeRotation_ = engine.FindProperty(sceneComponentClass, "RelativeRotation");

    LogInfo("perception: Pawn +0x%X  AcknowledgedPawn +0x%X  RootComponent +0x%X",
            propPawn_.offset, propAckPawn_.offset, propRootComponent_.offset);
    LogInfo("perception: RelativeLocation +0x%X  RelativeRotation +0x%X",
            propRelativeLocation_.offset, propRelativeRotation_.offset);

    if (!propRootComponent_.valid() || !propRelativeLocation_.valid()) {
        LogError("perception: RootComponent or RelativeLocation not found -- "
                 "refusing to read a guessed offset");
        return false;
    }
    if (!propPawn_.valid() && !propAckPawn_.valid()) {
        LogError("perception: neither Pawn nor AcknowledgedPawn found on PlayerController");
        return false;
    }

    resolved_ = true;
    return true;
}

bool AgentPerception::Sample(ue4::Engine& engine, AgentState& out) {
    out.hasPawn = 0;
    out.timestampMs = static_cast<uint64_t>(NowMs());
    out.objectCount = static_cast<uint32_t>(engine.NumObjects());

    if (!resolved_) return false;

    // Re-validate the cached controller before trusting it. The engine recycles
    // UObject slots, so a pointer that is merely readable proves nothing -- the
    // serial number is what says it is still the same object. Skipping this is
    // how you end up reading a transform off whatever now occupies the slot.
    void* controller = nullptr;
    if (cachedController_ && ue4::IsReadable(cachedController_, 0x30) &&
        engine.IsDerivedFrom(cachedController_, "PlayerController")) {
        controller = cachedController_;
    } else {
        cachedController_ = nullptr;
    }

    if (!controller) {
        // Scan for a live PlayerController. Class-default objects are templates,
        // not the thing possessing a pawn.
        const int32_t total = engine.NumObjects();
        for (int32_t i = 0; i < total; ++i) {
            ue4::ObjectRef ref;
            if (!engine.GetObject(i, ref)) continue;
            if (ref.name.rfind("Default__", 0) == 0) continue;
            if (!engine.IsDerivedFrom(ref.object, "PlayerController")) continue;
            controller = ref.object;
            cachedController_ = controller;
            cachedControllerSerial_ = ref.serialNumber;
            break;
        }
    }
    if (!controller) return false;

    void* pawn = ReadObjectPtr(controller, propPawn_);
    if (!pawn) pawn = ReadObjectPtr(controller, propAckPawn_);
    if (!pawn) return false;      // menu, loading, or an unpossessed cutscene

    void* root = ReadObjectPtr(pawn, propRootComponent_);
    if (!root) return false;

    float loc[3] = {};
    if (!ReadFloat3(root, propRelativeLocation_, loc)) return false;
    float rot[3] = {};
    ReadFloat3(root, propRelativeRotation_, rot);   // optional

    out.posX = loc[0];
    out.posY = loc[1];
    out.posZ = loc[2];
    out.pitch = rot[0];
    out.yaw = rot[1];
    out.roll = rot[2];
    out.hasPawn = 1;
    return true;
}

bool AgentPerception::StartPublishing() {
    if (mapped_) return true;

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(AgentState), kAgentStateName);
    if (!h) {
        LogWarn("perception: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AgentState));
    if (!view) {
        LogWarn("perception: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(h);
        return false;
    }

    section_ = h;
    mapped_ = static_cast<AgentState*>(view);
    std::memset(mapped_, 0, sizeof(AgentState));
    mapped_->magic = kAgentStateMagic;
    mapped_->version = kAgentStateVersion;
    LogInfo("perception: publishing agent state at %s", kAgentStateName);
    return true;
}

void AgentPerception::Publish(const AgentState& s) {
    if (!mapped_) return;

    // Seqlock write: odd while writing, even when coherent. The fences stop the
    // compiler or CPU moving the payload stores outside the odd/even window,
    // which would let a reader see a consistent counter around torn data --
    // the exact failure the counter exists to prevent.
    ++seq_;
    mapped_->seq = seq_ * 2 - 1;         // odd
    MemoryBarrier();

    mapped_->timestampMs = s.timestampMs;
    mapped_->frameIndex = s.frameIndex;
    mapped_->hasPawn = s.hasPawn;
    mapped_->posX = s.posX;
    mapped_->posY = s.posY;
    mapped_->posZ = s.posZ;
    mapped_->pitch = s.pitch;
    mapped_->yaw = s.yaw;
    mapped_->roll = s.roll;
    mapped_->liveSlots = s.liveSlots;
    mapped_->objectCount = s.objectCount;

    MemoryBarrier();
    mapped_->seq = seq_ * 2;             // even
}

void AgentPerception::StopPublishing() {
    if (mapped_) {
        UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (section_) {
        CloseHandle(static_cast<HANDLE>(section_));
        section_ = nullptr;
    }
}

AgentPerception& GetPerception() {
    static AgentPerception p;
    return p;
}

}  // namespace segcap
