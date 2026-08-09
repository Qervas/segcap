// perception.h -- what the agent can know about the game, read from the engine.
//
// The point of this file, and why it is short:
//
// A game-playing agent normally has to infer the world from pixels. This one
// does not have to. The segmentation work already built everything needed to
// read the engine directly -- GUObjectArray traversal, FName resolution, and
// property lookup by name -- so the agent's sensor is the same machinery, asked
// different questions.
//
// That changes the difficulty of the problem. "Am I stuck?" is a hard question
// from a screenshot, especially against a game with temporal AA where 12% of
// pixels differ between two frames of a completely static scene. It is a
// trivial question given the player pawn's world transform.
//
// Everything here is READ-ONLY and runs on the game thread via the existing
// ProcessEvent hook. No new hooks, no new writes.
//
// Resolution chain, all by name, no hardcoded offsets:
//
//   PlayerController instance          (scan the object array)
//     -> Pawn / AcknowledgedPawn       (ObjectProperty)
//       -> RootComponent               (ObjectProperty on AActor)
//         -> RelativeLocation          (StructProperty FVector on USceneComponent)
//         -> RelativeRotation          (StructProperty FRotator)

#pragma once

#include <cstdint>

#include "../shared/agent_state.h"
#include "ue4.h"

namespace segcap {

class AgentPerception {
public:
    // Resolves the classes and properties once. Returns false and names the
    // missing piece rather than proceeding with a guessed offset.
    bool Resolve(ue4::Engine& engine);
    bool ready() const { return resolved_; }

    // Reads the current pawn transform. MUST run on the game thread: it chases
    // UObject pointers, and the game is free to destroy a pawn on a level
    // transition between any two of them.
    //
    // Returns false when there is no pawn -- at a menu, during a load, or in a
    // cutscene. That is a normal state and is reported as `hasPawn = 0` rather
    // than as a zeroed transform, because an agent cannot tell "no pawn" from
    // "pawn at the world origin" and would read a level load as a teleport.
    bool Sample(ue4::Engine& engine, AgentState& out);

    // Creates the shared section and publishes snapshots into it.
    bool StartPublishing();
    void Publish(const AgentState& s);
    void StopPublishing();

private:
    bool resolved_ = false;

    void* controllerClass_ = nullptr;
    ue4::PropertyInfo propPawn_;
    ue4::PropertyInfo propAckPawn_;
    ue4::PropertyInfo propRootComponent_;
    ue4::PropertyInfo propRelativeLocation_;
    ue4::PropertyInfo propRelativeRotation_;

    // Cached so the whole object array is not rescanned every tick. Validated
    // against its serial number before use: the engine recycles UObject slots,
    // and a stale pointer that happens to be readable is exactly how you end up
    // reading a transform off an unrelated object.
    void* cachedController_ = nullptr;
    int32_t cachedControllerSerial_ = 0;

    void* section_ = nullptr;      // HANDLE
    AgentState* mapped_ = nullptr;
    uint64_t seq_ = 0;
};

AgentPerception& GetPerception();

}  // namespace segcap
