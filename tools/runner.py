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
from games import GameProfile, PadStep

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
        # What actually happened, for the caller to report instead of guess.
        # "armed" / "refused" / "never-got-there" -- three different failures
        # that were all being described with one hardcoded sentence.
        self.outcome = "never-got-there"
        # Did the game die under us? The control run's entire result.
        self.died = False
        # The world we were in when the capture armed, empty if never identified.
        # A control cannot claim "died in gameplay" without this.
        self.armed_level = ""
        # Process name act.ps1 targets: the profile's image without .exe. Passed
        # explicitly on every invocation, because act.ps1's old default pointed
        # at one specific game and silently misfired for the other.
        image = profile.image
        self.proc = image[:-4] if image.lower().endswith(".exe") else image

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
        """Perform one route step, whichever kind it is.

        Dispatches on the step type rather than assuming a mouse, because a
        profile may legitimately need either and Stray needs the pad: it ignores
        SendInput outright, so a click is not a fallback for a button.
        """
        H.ensure_live(self.pid, step.what or getattr(step, "btn", "step"), self.say)
        if isinstance(step, PadStep):
            self.pad(step)
            return
        self.say(step.what)
        if self.preflight:
            return
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", str(ACT), "-Process", self.proc, "-ClickFx", str(step.fx),
                        "-ClickFy", str(step.fy), "-Wait", str(step.wait)], cwd=str(ROOT))

    def pad(self, step: PadStep) -> None:
        """Press a pad button N times through the vpad command channel.

        vpad --serve is already running from launch(); this is the client half
        that was never written. Each press waits for vpad's ack, so a press
        cannot be queued over one that has not been read yet -- without that the
        pad silently skips them.
        """
        label = step.what or f"pad {step.btn} x{step.times}"
        self.say(label)
        if self.preflight:
            return
        # Is the pad server even alive? It is spawned in launch() and nothing
        # checked afterwards, so a vpad that failed to start left the route
        # pressing buttons into nothing while every step reported fine.
        if not H.find_pid("vpad.exe"):
            self.say("!! vpad is NOT running -- no pad input will reach the game. "
                     "Check that the ViGEmBus driver is installed (see README "
                     "Prerequisites); vpad exits immediately without it.")
            return
        for i in range(step.times):
            if not H.pad_send(BIN / "vpad_cmd.txt", btn=step.btn, ms=step.ms):
                self.say(f"!! pad press {i + 1}/{step.times} ({step.btn}) was never "
                         f"acknowledged -- is vpad running, and is the ViGEmBus "
                         f"driver installed?")
            time.sleep(step.gap)

    # --- phases -----------------------------------------------------------
    def prepare(self) -> None:
        self.ps(ROOT / "tools" / "reset_markers.ps1", "-Bin", str(BIN))
        o = self.o
        # CONTROL RUN. census suppresses every GPU path in the DLL (readback,
        # injected copies, colour) and this clears marking too, so the process
        # differs from an untouched game only by our hooks and our logging.
        #
        # It exists because the crash archive cannot answer whether the
        # ResizeBuffers deaths are ours: all 62 reports on disk date from
        # 08-09, the day this project started on inZOI, so there is no
        # pre-injection baseline to compare against. Inference from that data
        # is not available at any price; one control run settles it.
        census = getattr(o, "census", False)
        if census:
            self.say("CONTROL RUN -- no GPU work (segcap.noreadback suppresses readback, "
                     "injected copies and colour) and no marking, on the NORMAL path so "
                     "the engine layer still reports world state. If the game dies in "
                     "gameplay anyway, the crash is not something we are doing to it.")
        # NOT census mode. Census looked like the obvious control and cannot be
        # one: it returns before the periodic ProbeWorldState ever runs, so the
        # level is permanently `?` and the run cannot tell gameplay from the
        # menu -- which is the single thing a control has to establish. Opting
        # into ProcessEvent there did not help either; discovery needs the object
        # graph to have plateaued, and census reaches that point too late to be
        # useful and, when hoisted earlier, too early to work.
        #
        # noreadback is the right switch: same gate as census on every GPU path
        # (hooks.cpp checks `!censusOnly_ && !noReadback_` for readback, inject
        # and colour alike), but the run is otherwise ordinary, so ProcessEvent,
        # ProbeWorldState and the level oracle all behave as in a capture run.
        self.marker("segcap.census", False)
        self.marker("segcap.noreadback", census)
        self.marker("segcap.petriage", False)
        self.marker("segcap.mark", not o.no_mark and not census,
                    "mark marker SET -- this run writes bRenderCustomDepth")
        self.marker("segcap.d3ddebug", o.d3d_debug,
                    "D3D12 VALIDATION LAYER ON -- diagnosis only, slow")
        self.marker("segcap.inject", o.inject and not census,
                    "INJECT ARMED -- copies recorded into the game's own lists")
        self.marker("segcap.idbuf", (o.idbuf or o.inject) and not census,
                    "ID-BUFFER PROBE on")
        self.marker("segcap.groundtruth", getattr(o, "groundtruth", False) and not census,
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
            radius = int(getattr(self.o, "radius", 0) or 0)
            if radius:
                (BIN / "segcap.radius").write_text(str(radius))
                self.say(f"distance gate ON -- only marking within {radius} units of the "
                         f"character (drift past {int(radius * 1.25)} hands the slot back)")
            else:
                (BIN / "segcap.radius").unlink(missing_ok=True)
        self.say(f"capture budget {self.o.captures} frames, stride {self.o.stride}")
        self.marker("segcap.requirearm", True)
        self.marker("segcap.arm", False)
        self.say("readback DISARMED until gameplay (budget reserved for the world)")

        if not self.preflight:
            H.kill_all([self.p.image, "vpad.exe"])
            time.sleep(3)
            # CLEAR THE PAD CHANNEL. vpad applies a command only when its seq
            # exceeds the last it applied, and it reports progress by writing
            # that seq to <cmd>.ack -- both files survive the run that wrote
            # them. A leftover ack of 768 makes every press in the next run
            # return "acknowledged" the instant it is sent, because the waiter
            # only asks whether ack >= seq.
            #
            # So the press never blocks, nothing verifies it landed, and a run
            # whose pad was not even running reported eight successful presses.
            # Same shape as the stale screenshots in frame_changes: old state on
            # disk read as this run's answer.
            for leftover in ("vpad_cmd.txt", "vpad_cmd.txt.ack", "vpad_cmd.txt.tmp"):
                (BIN / leftover).unlink(missing_ok=True)
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
        # KEEP VPAD'S OUTPUT. It was sent to DEVNULL, so when the pad server
        # failed there was nothing anywhere saying why -- and it logs exactly
        # what you need: whether the virtual pad connected, and every command it
        # applied ("serve: #7 ... btn=A"). Discarding that is what made a run
        # that pressed nothing look identical to one that pressed eight times.
        vpad_log = (BIN / "vpad.log").open("w")
        subprocess.Popen([str(VPAD), "--serve", "build/bin/vpad_cmd.txt"], cwd=str(ROOT),
                         stdout=vpad_log, stderr=subprocess.STDOUT)
        # Give it a moment, then confirm rather than assume. Without the driver
        # it exits at once, and every later press would go nowhere silently.
        time.sleep(1.5)
        if H.find_pid("vpad.exe"):
            self.say("vpad: virtual pad server up")
        else:
            self.say("!! vpad FAILED TO START -- pad input will not reach the game. "
                     f"See {BIN / 'vpad.log'}; the usual cause is a missing ViGEmBus "
                     f"driver (README Prerequisites).")
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
        # DO NOT BLOCK ON THE WORLD NAME. It cannot arrive until we click.
        #
        # An earlier version waited up to 120s here for a level name, and that
        # was a deadlock of my own making:
        #
        #   the harness waits for the world name
        #     -> the name needs ProbeWorldState
        #       -> which needs ProcessEvent
        #         -> which needs UE's object graph above 200,000
        #           -> which only happens once the save starts loading
        #             -> which needs the click the harness is refusing to make
        #
        # Observed exactly that: `object graph did NOT settle within 90s -- only
        # 53375 slots`, then `ProcessEvent not found`, then `level=?` forever.
        # Every unexplained '?' after that change was this, not a slow engine.
        #
        # So: take the name if it happens to be there, and carry on regardless.
        # wait_for_level_change latches its own baseline when we have none.
        self.menu_level = H.current_level(LOG)
        self.say(f"menu world is '{self.menu_level or 'not yet known'}'"
                 + ("" if self.menu_level else " -- clicking anyway; the world only "
                                               "populates once a load starts, so waiting "
                                               "here would prevent the thing it waits for"))

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
            if not p.expects_level_change:
                # Nothing to leave, so the signal is simply that a world resolved
                # at all. WAIT for it rather than sampling once: the name cannot
                # appear until ProcessEvent installs, which waits on the object
                # graph settling, so immediately after the pad presses it is
                # always empty and a single check always says no.
                self.say("no separate menu world on this title -- waiting for any world "
                         f"to resolve (ceiling {p.load_begin_ceiling:.0f}s)")
                deadline = time.monotonic() + p.load_begin_ceiling
                level = ""
                while time.monotonic() < deadline:
                    H.ensure_live(self.pid, "world to resolve", self.say)
                    level = H.current_level(LOG)
                    if level:
                        break
                    time.sleep(2)
                if level:
                    self.say(f"in world '{level}' after "
                             f"{p.load_begin_ceiling - (deadline - time.monotonic()):.0f}s")
                    entered = True
                    break
                self.say("no world resolved within the ceiling -- retrying the route")
            elif self.menu_level:
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
        if self.p.expects_level_change:
            H.wait_for_level_change(LOG, self.menu_level, 240, self.pid, self.say)
        else:
            self.say(f"in world '{H.current_level(LOG) or 'unnamed'}' -- no menu world to "
                     f"leave on this title")

        if p.resume_key or p.transport_y:
            running = False
            y = p.transport_y or 0.9606
            for x in (0.086, 0.100, 0.114, 0.128, 0.072, 0.142, 0.058):
                if self.preflight:
                    running = True
                    break
                # Bail the moment the game is gone rather than sweeping a corpse.
                # Observed: seven consecutive clicks each throwing
                # "inZOI-Win64-Shipping is not running", seven "screen static:
                # still paused" verdicts about a process that had already
                # exited, and then a 220-second hold against it.
                H.ensure_live(self.pid, "transport sweep", self.say)
                self.say(f"resume: clicking transport at ({x:.3f}, {y:.4f})")
                subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                "-File", str(ACT), "-Process", self.proc,
                                "-ClickFx", str(x), "-ClickFy", str(y),
                                "-Wait", "1"], cwd=str(ROOT))
                if H.frame_changes(ROOT, ACT, self.proc):
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
        # IS THE GAME EVEN ALIVE? Ask before arming, not 220 seconds later.
        #
        # Three attempts in one run armed on a dead process. The transport sweep
        # had already thrown "inZOI-Win64-Shipping is not running" seven times in
        # a row, the pause probe had correctly reported `screen static`, and the
        # harness announced ARMED anyway and settled in to hold for 220s against
        # a pid that no longer existed. ensure_live raises GameGone, which go()
        # already catches and turns into a failed attempt -- so the retry starts
        # immediately instead of after the full hold.
        H.ensure_live(self.pid, "arming", self.say)

        level = H.current_level(LOG)
        if level and self.menu_level and level == self.menu_level:
            self.say(f"REFUSING to arm: still in '{level}', the world we started in. "
                     f"Capturing here would fill the budget with menu frames and "
                     f"report success. Failing this attempt so the run can retry.")
            self.outcome = "refused"
            return False

        self.outcome = "armed"
        self.armed_level = level
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
                                "-File", str(ACT), "-Process", self.proc,
                                "-Ly", str(ly), "-Ms", "2500",
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
        # PREFLIGHT CHECKS PATHS TOO. It used to skip this block entirely, which
        # is backwards: the whole point of --preflight is to catch a harness or
        # profile mistake in one second instead of four minutes into a run.
        #
        # It hid a real one. games.py had Stray at `Stray\Binaries\Win64`, but
        # the executable lives under `Hk_project\Binaries\Win64` -- so the Python
        # harness had never launched Stray, and `stray --preflight` reported
        # "control flow OK" every time because it never looked.
        #
        # The game executable is profile data and a missing one is a bug, so it
        # fails. Build outputs are just "run build.ps1 first", so in preflight
        # they are reported and tolerated -- a fresh clone should still be able
        # to validate its control flow.
        missing_build = [f for f in (DLL, INJECTOR, VPAD, ACT) if not f.exists()]
        if not self.p.exe.exists():
            self.say(f"MISSING GAME EXECUTABLE: {self.p.exe}")
            self.say("  set the override env var, or fix the profile in tools/games.py")
            return 1
        if missing_build:
            for f in missing_build:
                self.say(f"missing build output: {f}")
            if not self.preflight:
                return 1
            self.say("(preflight) continuing anyway -- run .\\build.ps1 before a real run")
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
            self.died = True
        self.report()
        return 0
