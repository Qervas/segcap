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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

void MenuSequence(int presses) {
    // Stray: main menu -> SELECT SAVE (slot 1 pre-highlighted) -> SLOT 1
    // (CONTINUE pre-highlighted) -> in game. A confirms on every screen.
    //
    // More presses than screens, deliberately. The number of screens is not
    // actually fixed: intro logos, a "press any button" attract screen, and an
    // autosave notice all appear or not depending on how the game last exited.
    // A is contextual in gameplay (jump/interact) so surplus presses cost
    // nothing, whereas one press too few leaves the run stranded at a menu --
    // which is the failure this whole harness exists to stop repeating.
    Log("menu: A x%d with settle time between", presses);
    for (int i = 0; i < presses; ++i) {
        TapButton(XUSB_GAMEPAD_A);
        Sleep(2500);
    }
}

// Walks and looks around so captured frames differ from each other.
//
// The pattern deliberately mixes movement and camera rotation. For segmentation
// training data, what matters is that objects appear at varied positions,
// scales, and occlusions -- standing still and rotating produces far less
// variety than moving through the space.
void Patrol(int seconds) {
    Log("patrol: %d seconds of movement for scene variety", seconds);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;

    // Deterministic, not random: a reproducible traversal means two capture runs
    // over the same level are comparable, which matters when validating that a
    // change to the capture layer did not change the data.
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

    size_t i = 0;
    while (GetTickCount64() < deadline) {
        const auto& s = kSteps[i % (sizeof(kSteps) / sizeof(kSteps[0]))];
        ++i;

        XUSB_REPORT r;
        XUSB_REPORT_INIT(&r);
        r.sThumbLX = s.lx;
        r.sThumbLY = s.ly;
        r.sThumbRX = s.rx;
        Send(r);
        Log("  %s", s.what);

        // Re-send the held state at ~10Hz rather than sleeping on it.
        //
        // Two reasons. A real pad reports continuously, and some titles treat a
        // silent gap as the pad going away. More importantly for the dataset:
        // this gives the input log regular samples instead of one record every
        // couple of seconds, so a frame can be joined to an action without
        // extrapolating across a long hold.
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(s.ms);
        while (GetTickCount64() < until && GetTickCount64() < deadline) {
            Sleep(100);
            Send(r);
        }
    }
    Neutral();
    Log("patrol complete");
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
    std::string inputLogPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--test") doTest = true;
        else if (a == "--menu") doMenu = true;
        else if (a == "--input-log" && i + 1 < argc) inputLogPath = argv[++i];
        else if (a == "--menu-presses" && i + 1 < argc) menuPresses = std::atoi(argv[++i]);
        else if (a == "--patrol" && i + 1 < argc) patrolSeconds = std::atoi(argv[++i]);
        else if (a == "--help") { Usage(argv[0]); return 0; }
        else { Log("unknown argument: %s", a.c_str()); Usage(argv[0]); return 2; }
    }

    if (!doTest && !doMenu && patrolSeconds == 0) {
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

    if (doMenu) MenuSequence(menuPresses);
    if (patrolSeconds > 0) Patrol(patrolSeconds);

    Neutral();
    Disconnect();
    if (g_inputLog) {
        std::fclose(g_inputLog);
        g_inputLog = nullptr;
    }
    Log("pad released");
    return 0;
}
