// vpad.cpp -- a virtual Xbox 360 pad, for unattended dataset collection.
//
// Why this exists, and why it is not just a menu-clicker:
//
// This project is a segmentation-dataset capture system. A dataset pipeline that
// needs a human at a menu cannot collect at scale, and scale is the entire
// point. SendInput was tried first and Stray ignored it -- proven, not assumed:
// after four scan-code Enter taps the engine's object array stayed at the menu's
// 173,598 slots instead of growing to the ~320,000 an in-level session shows.
//
// ViGEm presents a controller through a kernel bus driver, so the game cannot
// distinguish it from real hardware. That solves the menu, and it solves a
// second problem that matters more for the actual product: a pad that MOVES
// generates varied scenes. A static camera would produce thousands of nearly
// identical frames, which is close to worthless as training data.
//
// Usage:
//   vpad.exe --test              create a pad, hold it, verify Windows sees it
//   vpad.exe --menu              A-button through Stray's three menu screens
//   vpad.exe --patrol <seconds>  walk and look around, for scene variety
//   vpad.exe --menu --patrol 120 both, in sequence

#include <windows.h>
#include <xinput.h>

#include <ViGEm/Client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../shared/agent_state.h"

#pragma comment(lib, "setupapi.lib")

namespace {

PVIGEM_CLIENT g_client = nullptr;
PVIGEM_TARGET g_pad = nullptr;
FILE* g_inputLog = nullptr;

// Milliseconds since the Unix epoch, from the system clock.
//
// Deliberately a WALL CLOCK rather than QueryPerformanceCounter or a
// process-relative timer. The input log is written by this process and the
// frame timestamps are written by a DLL inside the game; a per-process origin
// cannot be joined across that boundary, and QPC needs a shared epoch to be
// comparable. GetSystemTimeAsFileTime is the same clock in both processes and
// has 100ns resolution, which is far finer than a 16ms frame.
long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns ticks since 1601-01-01; 11644473600s to the Unix epoch.
    return static_cast<long long>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[vpad] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}

bool Connect() {
    g_client = vigem_alloc();
    if (!g_client) {
        Log("vigem_alloc failed (out of memory)");
        return false;
    }

    const VIGEM_ERROR err = vigem_connect(g_client);
    if (!VIGEM_SUCCESS(err)) {
        Log("vigem_connect failed: 0x%X", err);
        if (err == VIGEM_ERROR_BUS_NOT_FOUND) {
            Log("  the ViGEmBus driver is not installed -- run the installer first");
        }
        return false;
    }

    g_pad = vigem_target_x360_alloc();
    const VIGEM_ERROR add = vigem_target_add(g_client, g_pad);
    if (!VIGEM_SUCCESS(add)) {
        Log("vigem_target_add failed: 0x%X", add);
        return false;
    }

    Log("virtual X360 pad connected (index %d)", vigem_target_get_index(g_pad));
    return true;
}

void Disconnect() {
    if (g_pad) {
        vigem_target_remove(g_client, g_pad);
        vigem_target_free(g_pad);
        g_pad = nullptr;
    }
    if (g_client) {
        vigem_disconnect(g_client);
        vigem_free(g_client);
        g_client = nullptr;
    }
}

// Every input state we hand to the game, timestamped.
//
// This exists because the capture is meant to feed world models, which learn
// P(next frame | current frame, action). A video without the actions that
// produced it is only half the training pair.
//
// The unusual advantage here is that we are the one SYNTHESISING the input, so
// the action is known exactly rather than inferred. There is no hooking of the
// game's input handling, no guessing at deadzones, no sampling race: this is
// literally the report the driver delivered, written at the moment it was sent.
//
// Written as JSONL -- one self-contained record per line, appended and flushed
// immediately -- so a run that is killed mid-session still leaves a valid,
// readable log up to the last delivered input.
void LogInput(const XUSB_REPORT& r) {
    if (!g_inputLog) return;
    std::fprintf(g_inputLog,
                 "{\"t\":%lld,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,"
                 "\"lt\":%u,\"rt\":%u,\"buttons\":%u}\n",
                 NowMs(), r.sThumbLX, r.sThumbLY, r.sThumbRX, r.sThumbRY,
                 static_cast<unsigned>(r.bLeftTrigger),
                 static_cast<unsigned>(r.bRightTrigger),
                 static_cast<unsigned>(r.wButtons));
    std::fflush(g_inputLog);
}

void Send(const XUSB_REPORT& report) {
    if (g_client && g_pad) vigem_target_x360_update(g_client, g_pad, report);
    LogInput(report);
}

void Neutral() {
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);
    Send(r);
}

// A button press has to be held long enough for the game to sample it. UE polls
// input once per frame, so anything under ~2 frames can be missed entirely --
// which is a plausible reason synthetic input "does nothing" even when it is
// being delivered.
void TapButton(USHORT button, int holdMs = 120) {
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);
    r.wButtons = button;
    Send(r);
    Sleep(holdMs);
    Neutral();
    Sleep(120);
}

void MenuSequence(int presses, int gapMs) {
    // Stray: main menu -> SELECT SAVE (slot 1 pre-highlighted) -> SLOT 1
    // (CONTINUE pre-highlighted) -> in game. A confirms on every screen.
    //
    // More presses than screens, deliberately. The number of screens is not
    // actually fixed: intro logos, a "press any button" attract screen, and an
    // autosave notice all appear or not depending on how the game last exited.
    // A is contextual in gameplay (jump/interact) so surplus presses cost
    // nothing, whereas one press too few leaves the run stranded at a menu --
    // which is the failure this whole harness exists to stop repeating.
    // The gap is configurable because it is title-specific, not a constant.
    // Stray's menus animate in under two seconds. inZOI loads a saved world
    // between confirmations and can take a minute, so a 2.5s gap would fire
    // every remaining press into a loading screen and arrive in gameplay having
    // already pressed A four more times than intended.
    Log("menu: A x%d, %dms between", presses, gapMs);
    for (int i = 0; i < presses; ++i) {
        TapButton(XUSB_GAMEPAD_A);
        Sleep(gapMs);
    }
}

// ---------------------------------------------------------------------------
// Closed-loop patrol.
//
// The open-loop patrol below generates camera variety and nothing else. It does
// not know where it is, whether it moved, or that it has been walking into the
// same wall for four minutes. That was enough to prove the capture pipeline and
// it is not enough to collect a dataset: an unattended run that wedges in a
// corner after ninety seconds produces thousands of near-identical frames,
// which is worse than producing none, because it looks like data.
//
// What makes closing the loop cheap here is that the capture project already
// built the sensor. segcap.dll reads the player pawn's world transform out of
// the engine by reflection and publishes it in shared memory, so "am I stuck?"
// is a subtraction rather than a vision problem. Asking it from pixels would
// mean fighting Stray's temporal AA, which changes 12% of pixels between two
// frames of a completely static scene.
//
// Degrades cleanly: with no shared state -- segcap not injected, or no pawn
// because we are at a menu -- this falls back to the open-loop pattern rather
// than freezing.
// ---------------------------------------------------------------------------

struct StateReader {
    HANDLE section = nullptr;
    const segcap::AgentState* view = nullptr;

    bool IsOpen() const { return view != nullptr; }

    bool Open() {
        if (view) return true;
        section = OpenFileMappingA(FILE_MAP_READ, FALSE, segcap::kAgentStateName);
        if (!section) return false;
        view = static_cast<const segcap::AgentState*>(
            MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(segcap::AgentState)));
        if (!view) {
            CloseHandle(section);
            section = nullptr;
            return false;
        }
        return true;
    }

    // Seqlock read: retry while the counter is odd (a write is in progress) or
    // changed across the copy. Without this the agent could observe half of one
    // position and half of the next, which as a stuck-detector input is worse
    // than no reading at all -- it looks like motion.
    bool Read(segcap::AgentState& out) const {
        if (!view) return false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint64_t before = view->seq;
            if (before & 1ULL) continue;
            MemoryBarrier();
            out = *view;
            MemoryBarrier();
            if (view->seq == before) {
                return out.magic == segcap::kAgentStateMagic;
            }
        }
        return false;
    }

    void Close() {
        if (view) { UnmapViewOfFile(view); view = nullptr; }
        if (section) { CloseHandle(section); section = nullptr; }
    }
};

// Escalating recovery. Cheap moves first, because they usually work.
//
// 0  back up          -- caught on a lip or a doorframe
// 1  back + turn 90   -- facing a wall
// 2  jump forward     -- a step or ledge the walk cannot climb (Stray's cat
//                        jumps contextually, so A is the right button)
// 3  turn ~180        -- the direction is a dead end; leave
void Recover(int escalation) {
    XUSB_REPORT r;
    switch (escalation) {
        case 0:
            XUSB_REPORT_INIT(&r); r.sThumbLY = -22000;
            Send(r); Sleep(700);
            break;
        case 1:
            XUSB_REPORT_INIT(&r); r.sThumbLY = -18000; r.sThumbRX = 20000;
            Send(r); Sleep(900);
            break;
        case 2:
            XUSB_REPORT_INIT(&r); r.sThumbLY = 20000;
            Send(r); Sleep(200);
            r.wButtons = XUSB_GAMEPAD_A;
            Send(r); Sleep(250);
            r.wButtons = 0;
            Send(r); Sleep(500);
            break;
        default:
            XUSB_REPORT_INIT(&r); r.sThumbRX = 26000;
            Send(r); Sleep(1200);
            break;
    }
    Neutral();
    Sleep(150);
}

// Patrol that reacts to whether it is actually moving.
//
// Policy, deliberately simple: walk the deterministic pattern, but measure
// displacement between ticks. If the pawn has not moved while movement was
// commanded, escalate through recovery behaviours -- back up, turn away, jump,
// then turn further -- and resume. Escalation matters because the cheap fix
// works most of the time and the expensive one is needed rarely; always doing
// the expensive one would waste most of the session.
//
// The reader is opened LAZILY and retried, not once at startup. segcap cannot
// publish until ProcessEvent is verified and the engine layer has resolved,
// which measured at t+49.9s on Stray -- while vpad starts at t+7s. Opening once
// meant the shared section did not exist yet, so the agent fell back to
// open-loop for the entire run and said it was doing so exactly once, 40
// seconds before the state it wanted became available.
// inZOI's control scheme, read off its own on-screen legend rather than assumed:
//
//     RB + L   Run
//     LB + R   Move Camera        <- camera needs LB HELD
//     X        UI Focus
//     Y        Radial Menu
//     B        Cancel Action
//
// Two things this corrects. The Stray-derived patrol turns the camera with a
// bare right stick, which in inZOI does nothing at all -- the modifier is not
// optional. And inZOI auto-resumes into the last save, so it needs no menu
// navigation whatsoever; the A presses written for Stray's menus were being
// delivered into live gameplay, where A is a context action.
//
// Found by screenshotting the game and reading the legend. Every other blind
// input loop in this project cost multiple runs before an instrument was
// pointed at it; this one cost two.
void PatrolInZoi(int seconds, int cancelEverySecs) {
    Log("patrol (inZOI profile): %d seconds", seconds);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    ULONGLONG nextCancel =
        cancelEverySecs > 0 ? GetTickCount64() + static_cast<ULONGLONG>(cancelEverySecs) * 1000 : 0;

    // No A anywhere. In gameplay it is a context action and can open dialogs,
    // start conversations, or confirm something we cannot see.
    static const struct { SHORT lx, ly, rx; bool run; int ms; const char* what; } kSteps[] = {
        {     0,  26000,     0, false, 2600, "walk forward" },
        {     0,  26000, 12000, false, 2200, "walk + pan right" },
        { 20000,  16000,     0, false, 2000, "walk forward-right" },
        {     0,  26000,-12000, false, 2200, "walk + pan left" },
        {-20000,  16000,     0, false, 2000, "walk forward-left" },
        {     0,      0, 20000, false, 1800, "pan camera right" },
        {     0,  28000,     0, true,  2600, "run forward" },
        {     0, -20000,     0, false, 1600, "back up" },
        {     0,      0,-20000, false, 1800, "pan camera left" },
    };
    const size_t n = sizeof(kSteps) / sizeof(kSteps[0]);

    size_t i = 0;
    while (GetTickCount64() < deadline) {
        if (nextCancel && GetTickCount64() > nextCancel) {
            nextCancel = GetTickCount64() + static_cast<ULONGLONG>(cancelEverySecs) * 1000;
            Log("  cancel action: B");
            TapButton(XUSB_GAMEPAD_B, 120);
            Sleep(400);
        }

        const auto& s = kSteps[i % n];
        ++i;

        XUSB_REPORT r;
        XUSB_REPORT_INIT(&r);
        r.sThumbLX = s.lx;
        r.sThumbLY = s.ly;
        r.sThumbRX = s.rx;
        // The modifiers are held for the whole step, not tapped: they gate the
        // stick, so releasing them mid-step silently stops the camera moving.
        if (s.rx != 0) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
        if (s.run)     r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
        Send(r);
        Log("  %s", s.what);

        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(s.ms);
        while (GetTickCount64() < until && GetTickCount64() < deadline) {
            Sleep(100);
            Send(r);
        }
    }
    Neutral();
    Log("patrol complete (inZOI profile)");
}

void Patrol(int seconds, StateReader& reader, int uiEscapeSecs) {
    Log("patrol: %d seconds%s", seconds,
        uiEscapeSecs > 0 ? " (with periodic UI escape)" : "");
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    ULONGLONG nextOpenAttempt = 0;
    ULONGLONG nextUiEscape =
        uiEscapeSecs > 0 ? GetTickCount64() + static_cast<ULONGLONG>(uiEscapeSecs) * 1000 : 0;
    bool announced = false;

    // Deterministic, not random: a reproducible traversal means two capture runs
    // over the same level are comparable, which matters when validating that a
    // change to the capture layer did not change the data. The pattern mixes
    // movement with camera rotation because objects need to appear at varied
    // positions, scales and occlusions -- rotating on the spot produces far
    // less variety than moving through the space.
    static const struct { SHORT lx, ly, rx; int ms; const char* what; } kSteps[] = {
        {     0,  24000,     0, 2500, "forward" },
        {     0,  24000,  9000, 2000, "forward + look right" },
        { 18000,  12000,     0, 2000, "forward-right" },
        {     0,  24000, -9000, 2000, "forward + look left" },
        {-18000,  12000,     0, 2000, "forward-left" },
        {     0,      0, 16000, 1500, "turn in place" },
        {     0,  24000,     0, 2500, "forward" },
        {     0, -18000,     0, 1500, "back up" },
        {     0,      0,-16000, 1500, "turn back" },
    };
    const size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

    // 40 uu over a tick is a slow walk; below that with movement commanded means
    // something is in the way. Stray's cat moves ~300 uu/s, so this is generous
    // rather than trigger-happy -- a false "stuck" costs a pointless recovery,
    // and being too eager to declare it would shred the traversal.
    constexpr float kMovedThresholdUU = 40.0f;
    constexpr int kStuckTicksToAct = 6;        // ~600ms of no progress

    size_t stepIdx = 0;
    int stuckTicks = 0;
    int recoveries = 0;
    int escalation = 0;
    bool haveLast = false;
    float lastX = 0, lastY = 0, lastZ = 0;
    ULONGLONG lastSampleMs = 0;

    while (GetTickCount64() < deadline) {
        // Periodic UI escape. In a game whose gameplay is interleaved with
        // menus -- inZOI opens panels constantly -- movement input is swallowed
        // by whatever dialog is focused, and no amount of stick deflection will
        // move anything. B backs out of it. Done on a timer rather than on
        // detection because there is no perception on this title yet: the
        // property reflection that would say "the pawn did not move" does not
        // resolve on UE5.
        if (nextUiEscape && GetTickCount64() > nextUiEscape) {
            nextUiEscape = GetTickCount64() + static_cast<ULONGLONG>(uiEscapeSecs) * 1000;
            Log("  UI escape: B");
            TapButton(XUSB_GAMEPAD_B, 120);
            Sleep(500);
        }

        const auto& s = kSteps[stepIdx % kStepCount];
        ++stepIdx;

        XUSB_REPORT r;
        XUSB_REPORT_INIT(&r);
        r.sThumbLX = s.lx;
        r.sThumbLY = s.ly;
        r.sThumbRX = s.rx;
        Send(r);

        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(s.ms);
        while (GetTickCount64() < until && GetTickCount64() < deadline) {
            Sleep(100);
            Send(r);          // hold the state, and keep the input log sampled

            // Keep trying to attach until segcap publishes.
            if (!reader.IsOpen() && GetTickCount64() > nextOpenAttempt) {
                nextOpenAttempt = GetTickCount64() + 2000;
                if (reader.Open()) Log("  engine state attached -- closed loop active");
            }
            if (!reader.IsOpen()) {
                if (!announced) {
                    Log("  no engine state yet -- open loop until segcap publishes");
                    announced = true;
                }
                continue;
            }

            segcap::AgentState st;
            if (!reader.Read(st) || !st.hasPawn) {
                // No pawn: menu, loading, or a cutscene. Not stuck -- just not
                // in control. Reset so the first frames after regaining control
                // are not immediately judged.
                haveLast = false;
                stuckTicks = 0;
                continue;
            }
            if (st.timestampMs == lastSampleMs) continue;   // no new publish yet
            lastSampleMs = st.timestampMs;

            if (haveLast) {
                const float dx = st.posX - lastX, dy = st.posY - lastY, dz = st.posZ - lastZ;
                const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
                const bool commandedMove = (s.lx != 0 || s.ly != 0);
                if (commandedMove && moved < kMovedThresholdUU) {
                    ++stuckTicks;
                } else {
                    stuckTicks = 0;
                    escalation = 0;
                }
            }
            lastX = st.posX; lastY = st.posY; lastZ = st.posZ;
            haveLast = true;

            if (stuckTicks >= kStuckTicksToAct) {
                ++recoveries;
                Log("  STUCK at (%.0f, %.0f, %.0f) -- recovery %d, escalation %d",
                    lastX, lastY, lastZ, recoveries, escalation);
                Recover(escalation);
                escalation = (escalation + 1) % 4;
                stuckTicks = 0;
                haveLast = false;
                break;                     // re-pick a step after recovering
            }
        }
    }
    Neutral();
    Log("patrol complete (%d recoveries)", recoveries);
}

// Command-server mode: hold the pad open and apply whatever an external prober
// asks for, one input at a time.
//
// This exists because hardcoding a control scheme per title is a dead end. It
// was wrong for inZOI twice -- once assuming Stray's menu flow on a game that
// auto-resumes into a save, once assuming a bare right stick turns a camera
// that requires LB held. Both were guesses, and the correct answer was
// obtainable in a minute by pressing one button and looking at the screen.
//
// So: the pad becomes an instrument something else can drive, and control
// discovery becomes a measurement (tools/probe_controls.py) instead of a
// constant in a table. That generalises to any title; a hardcoded scheme
// generalises to none.
//
// File protocol rather than a socket, for the same reason the rest of this
// project uses marker files: no port to collide, no lifetime to manage, and it
// can be driven by hand from a shell when something needs poking.
//
//   command file:  seq=<n> lx=<n> ly=<n> rx=<n> ry=<n> lt=<n> rt=<n> btn=<CSV> ms=<n>
//   ack file:      the seq most recently applied
//   seq=-1         exit
USHORT ButtonMask(const std::string& csv) {
    static const struct { const char* name; USHORT bit; } kMap[] = {
        {"A", XUSB_GAMEPAD_A}, {"B", XUSB_GAMEPAD_B},
        {"X", XUSB_GAMEPAD_X}, {"Y", XUSB_GAMEPAD_Y},
        {"LB", XUSB_GAMEPAD_LEFT_SHOULDER}, {"RB", XUSB_GAMEPAD_RIGHT_SHOULDER},
        {"LS", XUSB_GAMEPAD_LEFT_THUMB},    {"RS", XUSB_GAMEPAD_RIGHT_THUMB},
        {"START", XUSB_GAMEPAD_START},      {"BACK", XUSB_GAMEPAD_BACK},
        {"DU", XUSB_GAMEPAD_DPAD_UP},       {"DD", XUSB_GAMEPAD_DPAD_DOWN},
        {"DL", XUSB_GAMEPAD_DPAD_LEFT},     {"DR", XUSB_GAMEPAD_DPAD_RIGHT},
    };
    USHORT mask = 0;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const std::string tok =
            csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        for (const auto& m : kMap) {
            if (tok == m.name) { mask |= m.bit; break; }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return mask;
}

void Serve(const std::string& cmdPath) {
    Log("serve: reading commands from %s", cmdPath.c_str());
    const std::string ackPath = cmdPath + ".ack";
    long long lastSeq = 0;
    Neutral();

    for (;;) {
        std::FILE* f = nullptr;
        if (fopen_s(&f, cmdPath.c_str(), "r") == 0 && f) {
            char line[512] = {};
            if (std::fgets(line, sizeof(line), f)) {
                long long seq = 0; int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, ms = 400;
                char btn[128] = "";
                // Tolerant parse: a partially written file just fails to match
                // and is retried on the next poll rather than applying garbage.
                const char* p = line;
                while (*p) {
                    if      (sscanf_s(p, "seq=%lld", &seq) == 1 && strncmp(p, "seq=", 4) == 0) {}
                    else if (strncmp(p, "lx=", 3) == 0)  sscanf_s(p, "lx=%d", &lx);
                    else if (strncmp(p, "ly=", 3) == 0)  sscanf_s(p, "ly=%d", &ly);
                    else if (strncmp(p, "rx=", 3) == 0)  sscanf_s(p, "rx=%d", &rx);
                    else if (strncmp(p, "ry=", 3) == 0)  sscanf_s(p, "ry=%d", &ry);
                    else if (strncmp(p, "lt=", 3) == 0)  sscanf_s(p, "lt=%d", &lt);
                    else if (strncmp(p, "rt=", 3) == 0)  sscanf_s(p, "rt=%d", &rt);
                    else if (strncmp(p, "ms=", 3) == 0)  sscanf_s(p, "ms=%d", &ms);
                    else if (strncmp(p, "btn=", 4) == 0) sscanf_s(p, "btn=%127s", btn, (unsigned)_countof(btn));
                    while (*p && *p != ' ') ++p;
                    while (*p == ' ') ++p;
                }
                std::fclose(f);
                f = nullptr;

                if (seq == -1) { Log("serve: exit requested"); break; }
                if (seq > lastSeq) {
                    XUSB_REPORT r;
                    XUSB_REPORT_INIT(&r);
                    r.sThumbLX = static_cast<SHORT>(lx);
                    r.sThumbLY = static_cast<SHORT>(ly);
                    r.sThumbRX = static_cast<SHORT>(rx);
                    r.sThumbRY = static_cast<SHORT>(ry);
                    r.bLeftTrigger = static_cast<BYTE>(lt);
                    r.bRightTrigger = static_cast<BYTE>(rt);
                    r.wButtons = ButtonMask(btn);

                    Log("serve: #%lld lx=%d ly=%d rx=%d ry=%d btn=%s ms=%d",
                        seq, lx, ly, rx, ry, btn[0] ? btn : "-", ms);

                    // Hold by re-sending: a single report can be missed if the
                    // game polls between frames, and some titles treat silence
                    // as the pad going idle.
                    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(ms);
                    while (GetTickCount64() < until) { Send(r); Sleep(16); }
                    Neutral();

                    lastSeq = seq;
                    std::FILE* a = nullptr;
                    if (fopen_s(&a, ackPath.c_str(), "w") == 0 && a) {
                        std::fprintf(a, "%lld\n", lastSeq);
                        std::fclose(a);
                    }
                }
            } else if (f) {
                std::fclose(f);
            }
        }
        Sleep(40);
    }
    Neutral();
}

void Usage(const char* exe) {
    std::printf(
        "usage: %s [--test] [--menu] [--patrol SECONDS]\n"
        "\n"
        "  --test            create the pad and hold it briefly, to verify the\n"
        "                    ViGEmBus driver is working\n"
        "  --menu            press A through Stray's menu screens into gameplay\n"
        "  --patrol SECONDS  move and look around, generating scene variety for\n"
        "                    dataset capture\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    bool doTest = false, doMenu = false;
    int patrolSeconds = 0;
    int menuPresses = 8;
    int menuGapMs = 2500;
    int preDelaySecs = 0;
    int uiEscapeSecs = 0;
    std::string inputLogPath;
    std::string profile = "stray";
    std::string servePath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--test") doTest = true;
        else if (a == "--menu") doMenu = true;
        else if (a == "--input-log" && i + 1 < argc) inputLogPath = argv[++i];
        else if (a == "--menu-presses" && i + 1 < argc) menuPresses = std::atoi(argv[++i]);
        else if (a == "--menu-gap" && i + 1 < argc) menuGapMs = std::atoi(argv[++i]);
        else if (a == "--pre-delay" && i + 1 < argc) preDelaySecs = std::atoi(argv[++i]);
        else if (a == "--ui-escape" && i + 1 < argc) uiEscapeSecs = std::atoi(argv[++i]);
        else if (a == "--profile" && i + 1 < argc) profile = argv[++i];
        else if (a == "--serve" && i + 1 < argc) servePath = argv[++i];
        else if (a == "--patrol" && i + 1 < argc) patrolSeconds = std::atoi(argv[++i]);
        else if (a == "--help") { Usage(argv[0]); return 0; }
        else { Log("unknown argument: %s", a.c_str()); Usage(argv[0]); return 2; }
    }

    if (!doTest && !doMenu && patrolSeconds == 0 && servePath.empty()) {
        Usage(argv[0]);
        return 2;
    }

    if (!Connect()) {
        Disconnect();
        return 1;
    }

    if (!inputLogPath.empty()) {
        if (fopen_s(&g_inputLog, inputLogPath.c_str(), "w") != 0 || !g_inputLog) {
            Log("could not open input log %s -- continuing without it",
                inputLogPath.c_str());
            g_inputLog = nullptr;
        } else {
            Log("input log -> %s", inputLogPath.c_str());
        }
    }

    if (doTest) {
        Log("holding pad for 5s -- check Windows game controller settings");
        Neutral();
        Sleep(2000);
        Log("pressing A once as a visible signal");
        TapButton(XUSB_GAMEPAD_A, 300);
        Sleep(2000);
        Log("test complete");
    }

    if (!servePath.empty()) {
        Serve(servePath);
        Neutral();
        Disconnect();
        if (g_inputLog) { std::fclose(g_inputLog); g_inputLog = nullptr; }
        Log("pad released");
        return 0;
    }

    if (preDelaySecs > 0) {
        Log("waiting %ds before touching anything (title still loading)", preDelaySecs);
        Neutral();
        Sleep(preDelaySecs * 1000);
    }

    if (doMenu) MenuSequence(menuPresses, menuGapMs);

    if (patrolSeconds > 0) {
        if (profile == "inzoi") {
            // No StateReader: property reflection does not resolve on UE5, so
            // there is no pawn transform to close the loop on. Open loop, and
            // say so rather than pretending otherwise.
            Log("profile inzoi: open loop (no UE5 perception yet)");
            PatrolInZoi(patrolSeconds, uiEscapeSecs);
        } else {
            StateReader reader;
            reader.Open();             // may fail; Patrol retries
            Patrol(patrolSeconds, reader, uiEscapeSecs);
            reader.Close();
        }
    }

    Neutral();
    Disconnect();
    if (g_inputLog) {
        std::fclose(g_inputLog);
        g_inputLog = nullptr;
    }
    Log("pad released");
    return 0;
}
