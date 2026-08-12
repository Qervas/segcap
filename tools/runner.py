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
        self.menu_level = ""

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
        self.marker("segcap.groundtruth", getattr(o, "groundtruth", False),
                    "GROUND-TRUTH INTERVENTION on -- one slot gets unmarked mid-run")
        # OWN THESE, do not inherit them.
        #
        # segcap.captures and segcap.stride were left on disk by the old
        # PowerShell harness and silently overrode the DLL's defaults, so every
        # run produced exactly 61 masks no matter what maxCaptures_ was raised to.
        # A marker nobody writes is a marker nobody can see; the runner now sets
        # both explicitly every run, so the value in effect is the value asked for.
        if not self.preflight:
            (BIN / "segcap.captures").write_text(str(self.o.captures))
            (BIN / "segcap.stride").write_text(str(self.o.stride))
        self.say(f"capture budget {self.o.captures} frames, stride {self.o.stride}")
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
        # Remember which world the MENU is, so the load can be detected as a
        # change rather than guessed at from a timer.
        # WAIT FOR THE NAME TO EXIST BEFORE RECORDING IT.
        #
        # This read current_level() immediately after render_signal and took
        # whatever was there -- which is nothing. render_signal fires at ~t=22
        # (the first census line), and the DLL's first `state: ... level=` line
        # does not appear until ~t=38, once the engine layer has resolved a live
        # UWorld. So menu_level was reliably '?', and a load gate keyed on "the
        # level changed FROM the menu" silently degraded to its fallback every
        # single run.
        #
        # Checked once, at startup, at the wrong moment -- the trap this file
        # already warns about three times over.
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            self.menu_level = H.current_level(LOG)
            if self.menu_level:
                break
            time.sleep(2)
        self.say(f"menu world is '{self.menu_level or '?'}'"
                 + ("" if self.menu_level else " -- no level line yet; the load gate will "
                                               "fall back to the object-churn signal"))

        entered = False
        for attempt in range(1, 4):
            for step in p.to_load:
                self.click(step)
            if not p.load_signal:
                entered = True
                break
            # GATE ON THE LEVEL NAME, NOT ON OBJECT CHURN.
            #
            # This waited on `world is changing`, which is what the DLL logs
            # whenever the object count jumps. That fires for the MENU world
            # settling as readily as for a save coming up, so it cannot answer
            # "did my click start a load?" -- and with a 25s ceiling it was
            # never once satisfied in 58 archived runs, so every inZOI run ever
            # made clicked Continue and the save slot a second time into a load
            # that was already running.
            #
            # The level NAME is unambiguous: OpeningLevel2 -> RedCity_Map means
            # the save is up. Measured across 22 archived runs that reached a
            # world, time from render_signal to that change:
            #
            #     min 41.5s   median 69.9s   p90 73.4s   max 74.5s
            #
            # plus one observed outlier at 118.4s in a session where the game
            # had been relaunched a dozen times and the save was no longer
            # cached. The ceiling covers that; the common case exits at ~70s.
            #
            # wait_for_level_change also requires the name to hold steady, which
            # rejects the intermediate worlds UE passes through on the way.
            #
            # Only when we actually know the menu's world name. A title whose
            # DLL never publishes `state: ... level=` leaves menu_level empty,
            # and gating on a change from "" would wait the full ceiling for a
            # signal that cannot arrive -- turning a fix for one game into
            # exactly this bug for the other.
            self.say(f"entering the world (attempt {attempt})")
            if self.menu_level:
                entered = bool(H.wait_for_level_change(
                    LOG, self.menu_level, p.load_begin_ceiling, self.pid, self.say))
            else:
                entered = H.wait_for_log(LOG, [p.load_signal], p.load_begin_ceiling,
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
        # TRY, THEN CHECK. Not "send it and hope".
        #
        # The keystroke was dispatched correctly every time -- focus acquired,
        # "key: sent '1' (vk 0x31)" in the log -- and the simulation stayed
        # paused for 176 seconds. Dispatching an input and observing its effect
        # are different claims, and only the second one is worth anything.
        # Candidates are tried in order and each is judged by whether game time
        # actually starts flowing.
        # THE SPEED KEY IS A TOGGLE, so press it an ODD number of times.
        #
        # inZOI maps 1-4 to game speeds and pressing the number you are ALREADY
        # on pauses -- so '1' flips between normal speed and paused. The previous
        # version sent '1' via SendInput, checked, then sent '1' again through a
        # different API as a "fallback". Two presses. It toggled the game back to
        # paused every single time, and the fallback loop written to fix the
        # problem was the problem.
        #
        # Press once, ask the engine, and only press again if it is still paused
        # -- which converges precisely because it toggles. Never two presses
        # without a check between them.
        # RESUME, THEN LOOK AT THE SCREEN.
        #
        # Every indirect oracle failed here. UE's IsGamePaused reads false even
        # in the opening menu, because inZOI runs its own world clock rather
        # than using UE's pause flag. Object-count delta was garbage-collection
        # noise. So the check is the one a person would make: take a screenshot,
        # wait, take another, and see whether anything moved.
        #
        # The key is tried first because it needs no coordinate. If it does not
        # take, sweep the transport bar -- inZOI's speed controls sit bottom-left
        # and our original single guess landed on PAUSE, one button left of play.
        # CLICK THE TRANSPORT. No keyboard.
        #
        # The number keys do not resume this game in practice, and the engine
        # cannot tell us whether they did: UE's IsGamePaused describes the
        # ENGINE pause a popup menu sets, and it reads false even sitting in the
        # opening menu. inZOI runs its own world clock, independent of it, and a
        # loaded save comes up with that clock stopped -- a behaviour its own
        # forums document. There is no flag to read, only a button to press.
        #
        # So: sweep the transport bar and judge each press by whether the SCREEN
        # starts changing on its own. The original single guess at x=0.066 landed
        # on PAUSE, one button left of play, which is why every "resumed" capture
        # was of a frozen world.
        # BE IN THE WORLD BEFORE TOUCHING THE TRANSPORT.
        #
        # A screenshot caught the harness clicking the transport bar while a
        # LOADING SCREEN was still up -- tip text, spinner, no HUD at all. Every
        # resume click landed on nothing, which is why the sim was never running.
        H.wait_for_level_change(LOG, self.menu_level, 240, self.pid, self.say)

        if p.resume_key or p.transport_y:
            running = False
            y = p.transport_y or 0.9606
            for x in (0.086, 0.100, 0.114, 0.128, 0.072, 0.142, 0.058):
                if self.preflight:
                    running = True
                    break
                self.say(f"resume: clicking transport at ({x:.3f}, {y:.4f})")
                subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                "-File", str(ACT), "-ClickFx", str(x), "-ClickFy", str(y),
                                "-Wait", "1"], cwd=str(ROOT))
                if H.frame_changes(ROOT, ACT):
                    self.say(f"sim IS running -- screen changed with no input, "
                             f"play is at x={x:.3f}")
                    running = True
                    break
                self.say("screen static: still paused")
            if not running:
                self.say("!! could not resume -- capturing a PAUSED world. Masks stay "
                         "correct but nothing in the scene will move.")
        for step in p.after_load:
            self.click(step)
        if p.after_load:
            H.wait_for_world_settled(LOG, p.settle_after_unpause, self.pid, self.say, quiet=6)
        time.sleep(5)

    def capture(self) -> bool:
        # DO NOT ARM IF WE KNOW WE ARE NOT IN THE WORLD.
        #
        # Every wait in into_gameplay is a CEILING that proceeds on expiry, which
        # is right -- a missed signal should not deadlock a run. But nothing then
        # consulted the answer, so a run whose every check had FAILED still armed
        # and captured happily:
        #
        #   no stable level change within 120s -- proceeding
        #   world-settled signal not seen within 240s -- proceeding
        #   no stable level change within 240s -- proceeding
        #   ARMED -- every captured frame from here is gameplay
        #
        # That run spent 200s photographing OpeningLevel2. Once the detectors
        # were fixed to report honestly (see 8.18), the remaining bug was that
        # the honest answer was ignored one line later.
        #
        # Failing the attempt here is strictly better than capturing the menu:
        # capture.py relaunches, and a fast retry beats 200s of wrong data that
        # reports success. Only refuses when the level is KNOWN and still the
        # menu's -- an unreadable level falls through, because "I could not tell"
        # is not "it is wrong", and the fallback path for titles with no level
        # line has to keep working.
        level = H.current_level(LOG)
        if level and self.menu_level and level == self.menu_level:
            self.say(f"REFUSING to arm: still in '{level}', the world we started in. "
                     f"Capturing here would fill the budget with menu frames and "
                     f"report success. Failing this attempt so the run can retry.")
            return False

        (BIN / "segcap.arm").write_text("1")
        self.say(f"ARMED in '{level or 'unknown world'}' -- every captured frame "
                 f"from here is gameplay")
        self.say(f"holding for {self.o.seconds}s")
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
        return True

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
