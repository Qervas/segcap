// agent_state.h -- the perception block shared between the injected DLL and the
// agent that drives the pad.
//
// WHY A SHARED BLOCK AND NOT ONE PROCESS
//
// Perception lives inside the game (it needs the engine) and actuation lives
// outside it (ViGEm talks to a kernel bus driver). Those could be merged by
// linking the ViGEm client into segcap.dll, and that was rejected for the same
// reason the compressor was: the injected DLL shares an address space with a
// shipped game, and everything added to it is risk taken on the game's behalf.
// A 128-byte shared section costs the host nothing.
//
// The latency is irrelevant at the rate this matters. The policy runs at 10Hz;
// a shared-memory read is measured in nanoseconds.
//
// WHY THE SEQUENCE COUNTER
//
// The writer is the game thread and the reader is another process, with no lock
// between them. A torn read here would put the agent at a position that never
// existed -- half of last frame's coordinates and half of this frame's -- which
// as a stuck-detector input is worse than no data at all, because it looks like
// motion. The classic seqlock handles it: the writer bumps `seq` to odd before
// writing and to even after, and the reader retries while `seq` is odd or
// changed across the read. No blocking, and the reader can never observe a
// half-written state.

#pragma once

#include <cstdint>

namespace segcap {

inline constexpr char kAgentStateName[] = "Local\\segcap_agent_state_v1";
inline constexpr uint32_t kAgentStateMagic = 0x53474341;   // 'SGCA'
inline constexpr uint32_t kAgentStateVersion = 1;

struct AgentState {
    uint32_t magic;
    uint32_t version;

    // Seqlock. Even and unchanged across a read means the snapshot is coherent.
    volatile uint64_t seq;

    uint64_t timestampMs;      // same wall clock as the input log
    uint64_t frameIndex;

    // Whether the fields below mean anything this tick. A pawn does not exist
    // at the main menu, during loading, or in a cutscene that dispossesses the
    // player -- and "no pawn" must be distinguishable from "pawn at the origin",
    // or the agent reads a level transition as having teleported to 0,0,0.
    uint32_t hasPawn;

    float posX, posY, posZ;    // world location, Unreal units (1 uu = 1 cm)
    float pitch, yaw, roll;    // degrees

    // How much of the world the capture layer is currently labelling. Useful to
    // the policy as a proxy for "is anything actually being rendered": during a
    // load screen or a fade this collapses to zero.
    uint32_t liveSlots;
    uint32_t objectCount;      // GUObjectArray size; menu ~175k, in-level ~330k+

    uint32_t reserved[8];
};

static_assert(sizeof(AgentState) < 256, "keep the shared block small");

}  // namespace segcap
