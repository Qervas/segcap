"""Win32 process/window primitives for driving a game under capture.

WHY THIS EXISTS IN PYTHON. The orchestrator was PowerShell, and in a single
session four runs died to PowerShell semantics rather than to anything about the
game:

  * `Bitmap.Save` with a relative path resolves against the PROCESS's
    CurrentDirectory, which `Set-Location` does not update -- a run died 150s in
    reporting that a directory which plainly existed did not.
  * `Stop-Process` on a pid that exited between enumeration and the kill, fatal
    under `$ErrorActionPreference = "Stop"` -- died during CLEANUP, before
    launching anything.
  * A function called above the `function` keyword that defines it -- scripts
    execute top-to-bottom, so it died 20s in with "not recognized as a cmdlet".
  * `[ref]$errs` on a variable that did not exist yet.

None of those are hard bugs. What made them expensive is that PowerShell has no
compile step, so each surfaced only when its line executed -- and here a line
four minutes in costs four minutes plus a loaded game world. Python does not fix
that by itself, but it makes the control flow importable, type-checkable with
mypy, and testable without launching a game, which does.

WHAT IS DELIBERATELY NOT PORTED. `act.ps1` (DPI-aware clicking and screenshots)
and the marker/archive scripts stay as subprocesses. They work, they are not
where the failures came from, and rewriting working code three days before a
demo buys nothing but fresh bugs.

ONE REAL LOSS, AND THE WORKAROUND. PowerShell hands you `.Threads` with
`WaitReason`, and that is what proved a "hang" was 146 threads at suspend count 1
rather than a deadlock. psutil does not expose it and the ctypes equivalent
(NtQuerySystemInformation) is a lot of struct marshalling. It turns out not to
matter: the CPU delta was the actual discriminator -- a suspended process burns
ZERO cycles while a streaming one burns plenty -- and `ResumeThread`'s return
value gives the previous suspend count directly, so attempting the recovery IS
the test. See `resume_all_threads`.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import re
import subprocess
import time
from pathlib import Path
from typing import Callable, Iterable, Sequence

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)

TH32CS_SNAPTHREAD = 0x00000004
THREAD_SUSPEND_RESUME = 0x0002
INVALID_HANDLE_VALUE = wt.HANDLE(-1).value


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ThreadID", wt.DWORD),
        ("th32OwnerProcessID", wt.DWORD),
        ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wt.DWORD),
    ]


class FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", wt.DWORD), ("dwHighDateTime", wt.DWORD)]


def _ft(f: FILETIME) -> int:
    return (f.dwHighDateTime << 32) | f.dwLowDateTime


def find_pid(image: str) -> int | None:
    """First pid whose image name matches, via tasklist (no psutil dependency)."""
    out = subprocess.run(
        ["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH", "/FO", "CSV"],
        capture_output=True, text=True,
    ).stdout
    m = re.search(r'^"' + re.escape(image) + r'","(\d+)"', out, re.M)
    return int(m.group(1)) if m else None


def kill(pid: int) -> None:
    """Terminate, tolerating a process that already exited.

    The PowerShell version made exactly this case fatal and lost a run to it.
    A process we wanted gone being already gone is success, not an error.
    """
    subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                   capture_output=True, text=True)


def kill_all(images: Iterable[str]) -> None:
    for image in images:
        subprocess.run(["taskkill", "/F", "/IM", image],
                       capture_output=True, text=True)


def cpu_seconds(pid: int) -> float | None:
    """Kernel+user CPU consumed. None if the process is gone.

    This is the discriminator that matters: a SUSPENDED process consumes zero,
    a process streaming a 35 GB save consumes a lot while pumping no messages.
    Both look identical to "is it responding".
    """
    h = kernel32.OpenProcess(0x0400 | 0x1000, False, pid)  # QUERY_INFORMATION|LIMITED
    if not h:
        return None
    try:
        creation, exit_, kern, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if not kernel32.GetProcessTimes(h, ctypes.byref(creation), ctypes.byref(exit_),
                                        ctypes.byref(kern), ctypes.byref(user)):
            return None
        return (_ft(kern) + _ft(user)) / 1e7
    finally:
        kernel32.CloseHandle(h)


def main_window(pid: int) -> int | None:
    """Top-level visible window owned by pid."""
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd: int, _lp: int) -> bool:
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def is_hung(pid: int) -> bool:
    """True when the window's message loop has stopped pumping.

    This is what PowerShell calls `.Responding`. On its own it means very little
    -- see cpu_seconds -- but combined with a flat CPU it is decisive.
    """
    hwnd = main_window(pid)
    if hwnd is None:
        return False
    return bool(user32.IsHungAppWindow(hwnd))


def resume_all_threads(pid: int) -> tuple[int, int]:
    """Resume every thread of pid. Returns (resumed, max_previous_suspend_count).

    Doubles as the DIAGNOSIS. ResumeThread returns the thread's PREVIOUS suspend
    count, so a return of 1 across every thread is proof of exactly one
    unmatched whole-process suspend -- which is what was actually happening when
    inZOI "hung": 146 of 147 threads suspended, zero CPU, and resuming them by
    hand brought the game back and it ran on for another 400 seconds.
    """
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snap == INVALID_HANDLE_VALUE:
        return (0, 0)
    resumed = 0
    worst = 0
    try:
        entry = THREADENTRY32()
        entry.dwSize = ctypes.sizeof(THREADENTRY32)
        ok = kernel32.Thread32First(snap, ctypes.byref(entry))
        while ok:
            if entry.th32OwnerProcessID == pid:
                h = kernel32.OpenThread(THREAD_SUSPEND_RESUME, False, entry.th32ThreadID)
                if h:
                    prev = kernel32.ResumeThread(h)
                    if prev != 0xFFFFFFFF and prev > 0:
                        resumed += 1
                        worst = max(worst, prev)
                    kernel32.CloseHandle(h)
            ok = kernel32.Thread32Next(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return (resumed, worst)


class GameGone(RuntimeError):
    """The process exited."""


class GameFrozen(RuntimeError):
    """The process is alive, pumping nothing, and consuming no CPU."""


def ensure_live(pid: int, what: str, log: Callable[[str], None]) -> None:
    """Raise if the game is gone or genuinely dead; recover a suspend in place.

    Three states share one symptom (`not responding`) and must not share one
    verdict:

      exited              -> GameGone
      busy (CPU moving)   -> fine, return. A 35 GB save stops pumping messages
                             for long stretches while working hard. The first
                             version of this check threw here and killed a run
                             during world load, the one phase where a multi-second
                             stall is the expected behaviour.
      suspended (no CPU)  -> resume it and carry on, loudly. Unexplained, fully
                             recoverable, and a run that dies on it wastes twenty
                             minutes for something that costs milliseconds to undo.
    """
    if find_pid_is_dead(pid):
        raise GameGone(f"game exited before: {what}")
    if not is_hung(pid):
        return

    before = cpu_seconds(pid)
    time.sleep(3)
    if find_pid_is_dead(pid):
        raise GameGone(f"game exited before: {what}")
    if not is_hung(pid):
        return
    after = cpu_seconds(pid)
    if before is not None and after is not None and (after - before) > 0.5:
        return  # busy, not dead

    resumed, prev = resume_all_threads(pid)
    if resumed:
        log(f"!! SUSPENDED before {what}: resumed {resumed} thread(s), "
            f"previous suspend count {prev}. UNEXPLAINED FAULT.")
        time.sleep(5)
        if not is_hung(pid):
            return

    # Disk I/O also shows as flat CPU, so watch considerably longer before
    # condemning it. Any sign of life wins.
    for _ in range(6):
        c0 = cpu_seconds(pid)
        time.sleep(5)
        if find_pid_is_dead(pid):
            raise GameGone(f"game exited before: {what}")
        if not is_hung(pid):
            return
        c1 = cpu_seconds(pid)
        if c0 is not None and c1 is not None and (c1 - c0) > 0.5:
            log(f"(not responding but working -- CPU advancing during: {what})")
            return
    raise GameFrozen(
        f"game FROZEN before {what}: pid {pid} alive, no messages and no CPU for 30s")


def find_pid_is_dead(pid: int) -> bool:
    return cpu_seconds(pid) is None


def log_is_advancing(path: Path, seconds: float = 12.0) -> bool:
    """Is the DLL still logging? i.e. is the game still RENDERING?

    Liveness had two checks -- the window pumping messages, and CPU being
    consumed -- and a game can fail at capture while passing both. Observed: the
    DLL's log stopped at t=77.5s, the harness armed, drove a non-rendering game
    for 200 more seconds, and exited reporting success. CPU was moving the whole
    time, so `ensure_live` waved it through; it had been made tolerant precisely
    because an earlier version killed a run during a legitimate loading stall.

    The DLL logs from the Present path, so its log growing is direct evidence
    that frames are still being presented -- which is the only liveness that
    matters once we are trying to capture. Cheap: one stat() call.

    SIZE, not mtime. Windows does not reliably flush a file's modification time
    while a handle is open and being appended to -- the directory entry can sit
    stale for a long time while the file grows. Checking mtime therefore reported
    a perfectly healthy, actively-logging game as dead, and killed two capture
    runs before they ever armed. Size comes from the same stat() and does update.
    """
    if not path.exists():
        return False
    first = path.stat().st_size
    time.sleep(min(seconds, 3.0))
    return path.stat().st_size > first


def tail(path: Path, lines: int = 1200) -> list[str]:
    """Last N lines, tolerant of a file being written to concurrently."""
    if not path.exists():
        return []
    try:
        with path.open("rb") as f:
            f.seek(0, 2)
            size = f.tell()
            block = min(size, 256 * 1024)
            f.seek(size - block)
            data = f.read(block)
        return data.decode("utf-8", "replace").splitlines()[-lines:]
    except OSError:
        return []


def wait_for_log(path: Path, patterns: Sequence[str], timeout: float, what: str,
                 pid: int, log: Callable[[str], None], floor: float = 0.0) -> bool:
    """Wait until every pattern has appeared. False on timeout (never fatal).

    Waiting on a SIGNAL rather than a duration is the whole point: the fixed
    `MenuWait 150` / `LoadWait 180` sleeps cost 5.5 minutes of every iteration
    and were sized once against a worst case nobody remeasured. The old values
    survive only as ceilings, so the worst case is unchanged.
    """
    t0 = time.monotonic()
    log(f"waiting for {what} (ceiling {timeout:.0f}s)")
    while time.monotonic() - t0 < timeout:
        ensure_live(pid, what, log)
        lines = tail(path)
        if all(any(p in ln for ln in lines) for p in patterns):
            waited = time.monotonic() - t0
            if waited < floor:
                time.sleep(floor - waited)
                waited = floor
            log(f"{what} after {waited:.0f}s (ceiling was {timeout:.0f}s)")
            return True
        time.sleep(2)
    log(f"{what} NOT observed within {timeout:.0f}s -- proceeding on the ceiling")
    return False


def wait_for_world_settled(path: Path, timeout: float, pid: int,
                           log: Callable[[str], None], quiet: float = 8.0) -> bool:
    """Wait until segcap stops reporting object-count churn.

    UE streams a save in over many seconds and segcap logs `world is changing
    (A -> B objects)` on each jump. When that goes quiet, streaming is done.
    This is the signal the flat 180s sleep was standing in for.
    """
    t0 = time.monotonic()
    log(f"waiting for the world to settle (quiet {quiet:.0f}s, ceiling {timeout:.0f}s)")
    last_change = time.monotonic()
    seen = ""
    saw_any = False
    while time.monotonic() - t0 < timeout:
        ensure_live(pid, "world settle", log)
        changes = [ln for ln in tail(path, 600) if "world is changing" in ln]
        if changes and changes[-1] != seen:
            seen = changes[-1]
            last_change = time.monotonic()
            saw_any = True
        if saw_any and (time.monotonic() - last_change) >= quiet:
            log(f"world settled after {time.monotonic() - t0:.0f}s")
            return True
        time.sleep(2)
    log(f"world-settled signal not seen within {timeout:.0f}s -- proceeding")
    return False


def sim_is_running(path: Path, log: Callable[[str], None], samples: int = 4) -> bool:
    """Is game TIME flowing, as opposed to merely being in the world?

    The DLL publishes `state: WORLD objects=N delta=+D ...` each census. In a
    running world objects are constantly created and destroyed, so delta moves.
    In a paused one it is +0 forever -- measured at exactly +0 for 176 seconds
    straight while the harness cheerfully reported "live gameplay".

    This is the check that was missing. Every "the sim is running" claim until
    now came from having SENT an input, never from observing an effect.
    """
    seen = 0
    for _ in range(samples):
        lines = [ln for ln in tail(path, 400) if "state: " in ln and "delta=" in ln]
        for ln in lines[-3:]:
            try:
                d = int(ln.split("delta=")[1].split()[0])
            except (IndexError, ValueError):
                continue
            # A THRESHOLD, not "not zero". This accepted +3 out of 564,553
            # objects and reported a fully paused game as live gameplay -- three
            # objects is garbage collection, not time passing. And prefer the
            # engine's own answer when it is present: `sim=RUNNING` comes from
            # UGameplayStatics::IsGamePaused and settles the question outright.
            if "sim=RUNNING" in ln:
                log("sim IS running (engine says so: IsGamePaused = false)")
                return True
            if "sim=PAUSED" in ln:
                continue
            if abs(d) >= 25:
                log(f"sim IS running (object delta {d:+d})")
                return True
        seen += 1
        time.sleep(3)
    log("sim appears PAUSED (object delta +0 across every sample)")
    return False


def frame_changes(root: Path, act: Path, wait: float = 4.0) -> bool:
    """Does the SCREEN change while we hold still? The only honest pause oracle.

    inZOI does not use UE's pause flag -- IsGamePaused reads false even sitting
    in the opening menu -- so the engine's answer describes UE, not the Zoi's
    world clock. Object-count delta was noise. What is left is the thing a human
    uses: look at it, wait, look again.

    Two screenshots with no input between them. If world time is flowing, Zois
    walk, the clock ticks and the pixels differ. If it is paused with a static
    camera, the frames are effectively identical. Compared as bytes rather than
    images so this needs no PIL -- PNG encoding of identical pixels is identical.

    DELETE BOTH FIRST, AND REQUIRE BOTH BACK. act.ps1 throws before writing
    anything if the game is gone, so the files keep whatever a PREVIOUS call left
    in them -- and a previous call's two frames differ, because the screen really
    was changing then. A dead game therefore compared two stale images, found
    them different, and this returned True.

    Observed exactly that: the transport click failed with "inZOI-Win64-Shipping
    is not running", the very next line was "sim IS running -- screen changed
    with no input", and the harness armed a capture on a process that did not
    exist. The DLL's counters for that run agree -- inject attempts=0, 0 live
    slots, 0 identities.

    A missing file means the screenshot failed. That is not the same as "nothing
    moved", but it is certainly not evidence that something did.
    """
    a = root / "build" / "bin" / "pause_probe_a.png"
    b = root / "build" / "bin" / "pause_probe_b.png"
    for out in (a, b):
        out.unlink(missing_ok=True)
    for out in (a, b):
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", str(act), "-Wait", "0.2", "-Out", str(out)],
                       cwd=str(root), capture_output=True)
        if out is a:
            time.sleep(wait)
    if not (a.exists() and b.exists()):
        return False
    try:
        return a.read_bytes() != b.read_bytes()
    except OSError:
        return False


def current_level(path: Path) -> str:
    """The loaded UWorld's name, straight from the engine's own state line.

    Empty when no world is resolved YET, which is not the same as no state line.
    The DLL prints `state: MENU level=?` before the engine layer has found a live
    UWorld, and this used to return that `?` verbatim -- a sentinel that reads as
    "unknown" to a human and as a perfectly good value to the caller, because a
    one-character string is truthy.

    The damage was silent and in two places. `menu world is '?'` was
    indistinguishable from "we could not read it", so a poll that waited for the
    name to exist exited immediately holding `?`. And the load gate, which waits
    to leave the menu's world, was then satisfied by the MENU's own name
    appearing -- passing for the wrong reason at the wrong moment.

    Scans the whole tail rather than the last 400 lines. tail() reads a fixed
    256 KB block either way, so the smaller window bought nothing and lost the
    answer whenever a census burst -- one line per render target -- pushed the
    state line further back than 400. Observed as `ARMED in 'unknown world'`
    seconds after the level had been confirmed twice.
    """
    for ln in reversed(tail(path, 100_000)):
        if "state: " in ln and "level=" in ln:
            name = ln.split("level=")[1].split()[0]
            if name and name != "?":
                return name
    return ""


def wait_for_level_change(path: Path, was: str, timeout: float, pid: int,
                          log: Callable[[str], None], stable: float = 6.0) -> str:
    """Wait until a DIFFERENT world is loaded and its name holds steady.

    Marked-component count could not tell menu from gameplay -- the menu world
    marks ~106 components too, so a >=100 threshold passed three seconds in and
    the harness went on clicking a loading screen. The level NAME can: the menu
    is OpeningLevel2, a loaded save is a different world entirely, and the name
    comes from UWorld rather than from anything inferred.

    Requiring it to hold steady rejects the intermediate worlds UE passes
    through while streaming.

    LATCHES ITS OWN BASELINE when `was` is empty. The world name does not exist
    until UE's object graph is populated enough for ProcessEvent to install, and
    that only happens once a load is under way -- so at click time the caller
    frequently has no name to pass. Blocking until it had one deadlocked the
    harness against itself. Instead, the first name that appears IS the world we
    started in (we are still in the menu when it resolves), and the wait then
    runs normally against it.
    """
    t0 = time.monotonic()
    log(f"waiting to leave '{was or 'the world we start in'}' for a loaded world "
        f"(ceiling {timeout:.0f}s)")
    seen = ""
    since = time.monotonic()
    while time.monotonic() - t0 < timeout:
        ensure_live(pid, "level change", log)
        now = current_level(path)
        if not was and now:
            was = now
            log(f"baseline world is '{was}' (first name to resolve); waiting to leave it")
            time.sleep(2)
            continue
        if now and now != was:
            if now != seen:
                seen = now
                since = time.monotonic()
                log(f"world is now '{now}' -- waiting {stable:.0f}s for it to settle")
            elif time.monotonic() - since >= stable:
                log(f"loaded world '{now}' after {time.monotonic() - t0:.0f}s")
                return now
        time.sleep(2)
    log(f"no stable level change within {timeout:.0f}s -- proceeding")
    return current_level(path)


def wait_for_gameplay(path: Path, timeout: float, pid: int,
                      log: Callable[[str], None], min_marked: int = 100) -> bool:
    """Wait until we are REALLY in the world, not still on a loading screen.

    "World settled" -- object-count churn going quiet -- fires while the loading
    screen is still up, and a screenshot proved it: the harness was clicking the
    transport bar over a loading tip and a spinner, so every click went nowhere
    and the simulation was never resumed. The spinner also animates, which is why
    a full-frame "did the screen change" oracle reported the sim as running.

    The DLL already knows the difference and logs it: on a loading screen it can
    mark 8-16 components, in gameplay it marks 255. A high marked count means
    real renderable actors exist, which only happens once the world is up.
    """
    t0 = time.monotonic()
    log(f"waiting for real gameplay (marked >= {min_marked}, ceiling {timeout:.0f}s)")
    while time.monotonic() - t0 < timeout:
        ensure_live(pid, "gameplay", log)
        for ln in reversed(tail(path, 600)):
            if "state: " not in ln or "marked=" not in ln:
                continue
            try:
                marked = int(ln.split("marked=")[1].split()[0])
            except (IndexError, ValueError):
                continue
            if marked >= min_marked:
                log(f"in gameplay after {time.monotonic() - t0:.0f}s (marked={marked})")
                return True
            break
        time.sleep(2)
    log(f"gameplay signal not seen within {timeout:.0f}s -- proceeding anyway")
    return False
