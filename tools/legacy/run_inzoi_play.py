"""Drive inZOI from launch to armed gameplay capture.

Python port of tools/run_inzoi_play.ps1. See tools/harness.py for why.

    python tools/run_inzoi_play.py --inject
    python tools/run_inzoi_play.py --preflight     # no game, checks the flow

--preflight is the point of the port as much as the language is. Every
PowerShell failure this harness suffered surfaced only when its line executed,
four minutes into a run, because there is no compile step. Preflight runs the
whole control flow with the game stubbed out, so an ordering or name-resolution
mistake fails in two seconds instead of costing a loaded world.

STILL SUBPROCESSES, DELIBERATELY: act.ps1 (DPI-aware click/screenshot),
reset_markers.ps1, archive_capture.ps1. They work and they are not where the
failures came from.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import harness as H

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "bin"
DLL = BIN / "segcap.dll"
INJECTOR = BIN / "injector.exe"
VPAD = BIN / "vpad.exe"
LOG = BIN / "segcap.log"
ACT = ROOT / "tools" / "act.ps1"

GAME_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\inZOI\BlueClient\Binaries\Win64")
GAME_EXE = GAME_DIR / "inZOI-Win64-Shipping.exe"
GAME_IMAGE = "inZOI-Win64-Shipping.exe"
APP_ID = 2456740

PREFLIGHT = False


def say(msg: str) -> None:
    print(f"[play] {msg}", flush=True)


def ps(script: Path, *args: str) -> None:
    """Run a PowerShell helper. Never fatal on its own exit code -- these are
    housekeeping steps, and losing a run to cleanup is how the old script died."""
    if PREFLIGHT:
        say(f"(preflight) would run {script.name} {' '.join(args)}")
        return
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", str(script), *args],
        cwd=str(ROOT), capture_output=False,
    )


def click(fx: float, fy: float, wait: float, what: str, pid: int) -> None:
    H.ensure_live(pid, what, say)
    say(what)
    if PREFLIGHT:
        return
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ACT),
         "-ClickFx", str(fx), "-ClickFy", str(fy), "-Wait", str(wait)],
        cwd=str(ROOT),
    )


def main() -> int:
    global PREFLIGHT
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=180, help="capture hold length")
    ap.add_argument("--menu-wait", type=float, default=150, help="CEILING, not a sleep")
    ap.add_argument("--load-wait", type=float, default=240, help="CEILING, not a sleep")
    ap.add_argument("--inject", action="store_true")
    ap.add_argument("--idbuf", action="store_true")
    ap.add_argument("--d3d-debug", action="store_true")
    ap.add_argument("--no-mark", action="store_true")
    # OFF by default: motion makes UE stream, streaming churns the object graph,
    # and that churn destroys the marked slots the mask is built from. Static
    # capture has to work before dynamic capture means anything.
    ap.add_argument("--walk", action="store_true")
    ap.add_argument("--preflight", action="store_true",
                    help="run the control flow with no game; catches flow bugs in seconds")
    args = ap.parse_args()
    PREFLIGHT = args.preflight

    if not PREFLIGHT:
        for f in (DLL, INJECTOR, VPAD, GAME_EXE, ACT):
            if not f.exists():
                say(f"missing: {f}")
                return 1

    # --- markers ---------------------------------------------------------
    ps(ROOT / "tools" / "reset_markers.ps1", "-Bin", str(BIN))

    def marker(name: str, on: bool, note: str = "") -> None:
        p = BIN / name
        if PREFLIGHT:
            say(f"(preflight) marker {name} = {on}")
            return
        if on:
            p.write_text("1")
            if note:
                say(note)
        else:
            p.unlink(missing_ok=True)

    marker("segcap.mark", not args.no_mark, "mark marker SET -- this run writes bRenderCustomDepth")
    marker("segcap.d3ddebug", args.d3d_debug, "D3D12 VALIDATION LAYER ON -- diagnosis only, slow")
    marker("segcap.inject", args.inject, "INJECT ARMED -- copies recorded into the game's lists")
    marker("segcap.idbuf", args.idbuf or args.inject, "ID-BUFFER PROBE on")
    marker("segcap.requirearm", True)
    marker("segcap.arm", False)
    say("readback DISARMED until gameplay (budget reserved for the world)")

    # --- clean slate, archiving BEFORE deleting ---------------------------
    if not PREFLIGHT:
        H.kill_all([GAME_IMAGE, "vpad.exe"])
        time.sleep(3)
    # The log is the other half of the evidence and this order used to be
    # reversed, so every crashed run left captures with no explanation.
    ps(ROOT / "tools" / "archive_capture.ps1", "-Title", "inzoi", "-Bin", str(BIN))
    if not PREFLIGHT:
        LOG.unlink(missing_ok=True)

    # --- launch suspended + inject ---------------------------------------
    appid = GAME_DIR / "steam_appid.txt"
    if not PREFLIGHT and not appid.exists():
        appid.write_text(str(APP_ID))
        say("wrote steam_appid.txt (reversible; delete to undo)")

    if PREFLIGHT:
        say("(preflight) would launch and inject; skipping to flow checks")
        pid = 0
    else:
        say("launching suspended and injecting")
        subprocess.Popen(
            [str(INJECTOR), "--launch", str(GAME_EXE), "--args", "-dx12",
             "--workdir", str(GAME_DIR), "--dll", str(DLL)],
            cwd=str(ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        pid = 0
        for _ in range(60):
            got = H.find_pid(GAME_IMAGE)
            if got:
                pid = got
                break
            time.sleep(1)
        if not pid:
            say("game never appeared")
            return 1
        say(f"game pid {pid}")
        subprocess.Popen([str(VPAD), "--serve", "build/bin/vpad_cmd.txt"], cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if PREFLIGHT:
        say("(preflight) control flow reached gameplay; OK")
        return 0

    try:
        # Clicking needs only that the game is DRAWING. Gating it on
        # ProcessEvent/marking -- engine-introspection milestones -- is what made
        # every run pay for work it did not need yet.
        H.wait_for_log(LOG, ["distinct targets observed"], args.menu_wait,
                       "the game to start rendering", pid, say, floor=6)

        entered = False
        for attempt in range(1, 4):
            click(0.0883, 0.2169, 2, f"Continue (attempt {attempt})", pid)
            click(0.6926, 0.3631, 3, f"first save slot (attempt {attempt})", pid)
            entered = H.wait_for_log(LOG, ["world is changing"], 25,
                                     "the save to begin loading", pid, say)
            if entered:
                break
        if not entered:
            say("no load signal -- continuing anyway")

        H.wait_for_world_settled(LOG, args.load_wait, pid, say)
        click(0.0660, 0.9606, 5, "transport play (unpause)", pid)
        H.wait_for_world_settled(LOG, 60, pid, say, quiet=6)
        time.sleep(5)

        (BIN / "segcap.arm").write_text("1")
        say("ARMED -- every captured frame from here is gameplay")

        say(f"in gameplay; holding for {args.seconds}s")
        deadline = time.monotonic() + args.seconds
        step = 0
        while time.monotonic() < deadline:
            H.ensure_live(pid, "capture hold", say)
            if args.walk:
                ly = 30000 if step % 2 == 0 else -30000
                subprocess.run(
                    ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                     "-File", str(ACT), "-Ly", str(ly), "-Ms", "2500", "-Wait", "0.5"],
                    cwd=str(ROOT), capture_output=True)
            else:
                time.sleep(3)
            step += 1
    except (H.GameGone, H.GameFrozen) as exc:
        say(f"STOPPED: {exc}")

    masks = len(list(BIN.glob("segcap_mask_*.pgm")))
    cars = len(list(BIN.glob("segcap_mask_*.json")))
    print("\n=============== RESULT ===============")
    print(f"  masks / sidecars : {masks} / {cars}")
    for pat in ("FOUND OUR IDS", "drew ZERO barriers", "inject: ON", "inject: OFF",
                "inject: attempts", "CAPTURE ARMED", "customdepth: marked"):
        hits = [ln for ln in H.tail(LOG, 6000) if pat in ln]
        if hits:
            print("  " + hits[-1].strip()[:150])
    print("======================================")
    print(f"  log: {LOG}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
