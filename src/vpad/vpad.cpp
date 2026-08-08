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

void Send(const XUSB_REPORT& report) {
    if (g_client && g_pad) vigem_target_x360_update(g_client, g_pad, report);
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

void MenuSequence() {
    // Stray: main menu -> SELECT SAVE (slot 1 pre-highlighted) -> SLOT 1
    // (CONTINUE pre-highlighted) -> in game. A confirms on every screen.
    // A fourth press is harmless once in gameplay.
    Log("menu: A x4 with settle time between");
    for (int i = 0; i < 4; ++i) {
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

        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(s.ms);
        while (GetTickCount64() < until && GetTickCount64() < deadline) Sleep(50);
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

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--test") doTest = true;
        else if (a == "--menu") doMenu = true;
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

    if (doTest) {
        Log("holding pad for 5s -- check Windows game controller settings");
        Neutral();
        Sleep(2000);
        Log("pressing A once as a visible signal");
        TapButton(XUSB_GAMEPAD_A, 300);
        Sleep(2000);
        Log("test complete");
    }

    if (doMenu) MenuSequence();
    if (patrolSeconds > 0) Patrol(patrolSeconds);

    Neutral();
    Disconnect();
    Log("pad released");
    return 0;
}
