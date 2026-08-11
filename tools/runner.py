"""The half of a capture run that is the same for every game.

Launch suspended and inject, get to gameplay, arm the readback, hold, report.
Everything title-specific lives in games.GameProfile; everything Win32 lives in
harness. This module is the orchestration and nothing else, so a fix here is a
fix for every title rather than for whichever script you happened to edit.
"""

from __future__ import annotations

import subprocess
import time
from pathlib import Path

import harness as H
from games import GameProfile

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "bin"
DLL = BIN / "segcap.dll"
INJECTOR = BIN / "injector.exe"
VPAD = BIN / "vpad.exe"
LOG = BIN / "segcap.log"
ACT = ROOT / "tools" / "act.ps1"


class Run:
    def __init__(self, profile: GameProfile, opts, preflight: bool = False) -> None:
        self.p = profile
        self.o = opts
        self.preflight = preflight
        self.pid = 0

    # --- plumbing ---------------------------------------------------------
    def say(self, msg: str) -> None:
        print(f"[{self.p.name}] {msg}", flush=True)

    def ps(self, script: Path, *args: str) -> None:
        """Run a PowerShell helper, never fatally.

        These are housekeeping steps. The PowerShell runner made a failed
        cleanup fatal and lost a whole run to `Stop-Process` on a pid that had
        already exited -- dying during cleanup, before launching anything.
        """
        if self.preflight:
            self.say(f"(preflight) would run {script.name} {' '.join(args)}")
            return
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", str(script), *args], cwd=str(ROOT))

    def marker(self, name: str, on: bool, note: str = "") -> None:
        path = BIN / name
        if self.preflight:
            self.say(f"(preflight) marker {name} = {on}")
            return
        if on:
            path.write_text("1")
            if note:
                self.say(note)
        else:
            path.unlink(missing_ok=True)

    def click(self, step) -> None:
        H.ensure_live(self.pid, step.what, self.say)
        self.say(step.what)
        if self.preflight:
            return
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", str(ACT), "-ClickFx", str(step.fx),
                        "-ClickFy", str(step.fy), "-Wait", str(step.wait)], cwd=str(ROOT))

    # --- phases -----------------------------------------------------------
    def prepare(self) -> None:
        self.ps(ROOT / "tools" / "reset_markers.ps1", "-Bin", str(BIN))
        o = self.o
        self.marker("segcap.mark", not o.no_mark,
                    "mark marker SET -- this run writes bRenderCustomDepth")
        self.marker("segcap.d3ddebug", o.d3d_debug,
                    "D3D12 VALIDATION LAYER ON -- diagnosis only, slow")
        self.marker("segcap.inject", o.inject,
                    "INJECT ARMED -- copies recorded into the game's own lists")
        self.marker("segcap.idbuf", o.idbuf or o.inject, "ID-BUFFER PROBE on")
        self.marker("segcap.requirearm", True)
        self.marker("segcap.arm", False)
        self.say("readback DISARMED until gameplay (budget reserved for the world)")

        if not self.preflight:
            H.kill_all([self.p.image, "vpad.exe"])
            time.sleep(3)
        # ARCHIVE BEFORE DELETING THE LOG. Reversed, this destroys the one
        # artefact that explains a crash, a moment before the archiver collects
        # it. That bug lived in one of the two old runners and not the other.
        self.ps(ROOT / "tools" / "archive_capture.ps1", "-Title", self.p.name, "-Bin", str(BIN))
        if not self.preflight:
            LOG.unlink(missing_ok=True)

    def launch(self) -> bool:
        p = self.p
        if p.steam_appid is not None and not self.preflight:
            appid = p.workdir / "steam_appid.txt"
            if not appid.exists():
                appid.write_text(str(p.steam_appid))
                self.say("wrote steam_appid.txt (reversible; delete to undo)")
        if self.preflight:
            self.say("(preflight) would launch and inject")
            return True

        self.say("launching suspended and injecting")
        cmd = [str(INJECTOR), "--launch", str(p.exe)]
        if p.launch_args:
            cmd += ["--args", p.launch_args]
        cmd += ["--workdir", str(p.workdir), "--dll", str(DLL)]
        subprocess.Popen(cmd, cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            got = H.find_pid(p.image)
            if got:
                self.pid = got
                break
            time.sleep(1)
        if not self.pid:
            self.say("game never appeared")
            return False
        self.say(f"game pid {self.pid}")
        subprocess.Popen([str(VPAD), "--serve", "build/bin/vpad_cmd.txt"], cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True

    def into_gameplay(self) -> None:
        p = self.p
        # The ONLY precondition for clicking is that the game is drawing.
        H.wait_for_log(LOG, [p.render_signal], p.menu_ceiling,
                       "the game to start rendering", self.pid, self.say, floor=6)

        # Probe rather than assume. A click on a still-loading screen does
        # nothing, so being early costs one wasted click while being late costs
        # minutes. The loop is self-correcting: a click that lands on the wrong
        # screen leaves the next iteration starting from the menu again.
        entered = False
        for attempt in range(1, 4):
            for step in p.to_load:
                self.click(step)
            if not p.load_signal:
                entered = True
                break
            entered = H.wait_for_log(LOG, [p.load_signal], 25,
                                     f"the world to begin loading (attempt {attempt})",
                                     self.pid, self.say)
            if entered:
                break
        if not entered:
            self.say("no load signal -- continuing anyway")

        H.wait_for_world_settled(LOG, p.load_ceiling, self.pid, self.say)

        # RESUME THE SIM WITH A KEY, NOT A CLICK, AND DO IT FIRST.
        #
        # The transport bar's play button sits between pause and the
        # fast-forward speeds, and our click coordinate was landing left of it --
        # so every "unpaused" capture was of a frozen simulation, and the motion
        # in those masks was camera movement alone. A key needs no coordinate and
        # cannot hit the neighbouring control. inZOI maps the number keys to
        # speeds, with 1 being normal.
        if p.resume_key:
            self.say(f"resuming the sim with '{p.resume_key}'")
            if not self.preflight:
                subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                "-File", str(ACT), "-Key", p.resume_key, "-Wait", "1"],
                               cwd=str(ROOT))
        for step in p.after_load:
            self.click(step)
        if p.after_load:
            H.wait_for_world_settled(LOG, p.settle_after_unpause, self.pid, self.say, quiet=6)
        time.sleep(5)

    def capture(self) -> None:
        (BIN / "segcap.arm").write_text("1")
        self.say("ARMED -- every captured frame from here is gameplay")
        self.say(f"in gameplay; holding for {self.o.seconds}s")
        deadline = time.monotonic() + self.o.seconds
        step = 0
        stalled = 0
        while time.monotonic() < deadline:
            H.ensure_live(self.pid, "capture hold", self.say)
            # The game can be alive, responding and burning CPU while no longer
            # PRESENTING -- and capture only happens on the Present path, so that
            # state is indistinguishable from success at the end of a run. Two
            # runs were lost to it today. The DLL logs from Present, so a log that
            # has stopped growing means we are recording nothing.
            if not H.log_is_advancing(LOG):
                stalled += 1
                if stalled >= 3:
                    self.say("STOPPED: the DLL's log has stopped growing -- the game is "
                             "no longer presenting, so nothing is being captured")
                    break
            else:
                stalled = 0
            if self.o.walk:
                # OFF by default. Motion makes UE stream, streaming churns the
                # object graph, and that churn destroys the marked slots the mask
                # is built from -- we were destabilising the state we were trying
                # to read. Static capture must work before dynamic means anything.
                ly = 30000 if step % 2 == 0 else -30000
                subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                "-File", str(ACT), "-Ly", str(ly), "-Ms", "2500",
                                "-Wait", "0.5"], cwd=str(ROOT), capture_output=True)
            else:
                time.sleep(3)
            step += 1

    def report(self) -> None:
        masks = len(list(BIN.glob("segcap_mask_*.pgm")))
        cars = len(list(BIN.glob("segcap_mask_*.json")))
        print("\n=============== RESULT ===============")
        print(f"  masks / sidecars : {masks} / {cars}")
        lines = H.tail(LOG, 8000)
        for pat in ("FOUND OUR IDS", "mask source switched", "drew ZERO barriers",
                    "inject: ON", "inject: OFF", "inject: attempts",
                    "CAPTURE ARMED", "customdepth: marked"):
            hits = [ln for ln in lines if pat in ln]
            if hits:
                print("  " + hits[-1].strip()[:150])
        print("======================================")
        print(f"  log: {LOG}")

    # --- entry ------------------------------------------------------------
    def go(self) -> int:
        if not self.preflight:
            for f in (DLL, INJECTOR, VPAD, self.p.exe, ACT):
                if not f.exists():
                    self.say(f"missing: {f}")
                    return 1
        self.prepare()
        if not self.launch():
            return 1
        if self.preflight:
            self.say("(preflight) control flow OK")
            return 0
        try:
            self.into_gameplay()
            self.capture()
        except (H.GameGone, H.GameFrozen) as exc:
            self.say(f"STOPPED: {exc}")
        self.report()
        return 0
