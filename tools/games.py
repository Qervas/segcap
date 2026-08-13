"""Per-game profiles: everything that differs between titles, and nothing else.

WHY THIS EXISTS. There were two run scripts, run_auto.ps1 (Stray) and
run_inzoi_play.ps1 (inZOI), which began as a copy and then DRIFTED. The costs
were real and repeated:

  * archive-before-delete-log was fixed in run_inzoi_play.ps1 and left broken in
    run_auto.ps1, so one of them destroyed the log a moment before the archiver
    came to collect it -- and which one you had used decided whether a crash was
    explainable afterwards.
  * the segcap_frame_* orphaning fix had to be applied in two places, and was
    not, which is how 114 Stray colour frames sat in build\\bin through an entire
    inZOI session waiting to be paired with the wrong game's masks.

A profile holds the parts that are genuinely per-title -- where the executable
lives, how you get from the main menu into the world, what the log says when
that worked -- and the runner holds the parts that are not. A fix to the shared
half is then a fix everywhere by construction rather than by memory.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from pathlib import Path


def steam_app_dir(appid: int) -> Path | None:
    """Where Steam actually installed an app, asked of Steam.

    The install path used to be a literal in each profile, which is wrong on any
    machine but this one -- Steam's root differs, and libraries routinely live on
    another drive entirely. Nothing about a title's identity is its path; its
    identity is the appid.

    Registry for Steam's own root, then libraryfolders.vdf for every library it
    knows about, then the app manifest in each for the real directory name --
    which is NOT reliably the store name (Stray installs to "Stray", inZOI to
    "inZOI", but neither is guaranteed and neither is the appid).

    Returns None rather than guessing, so the caller can fall back and say so.
    """
    roots: list[Path] = []
    try:
        import winreg
        for hive, key, val in (
            (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
        ):
            try:
                with winreg.OpenKey(hive, key) as k:
                    roots.append(Path(winreg.QueryValueEx(k, val)[0]))
            except OSError:
                continue
    except ImportError:                      # not Windows; harmless
        pass

    # Every library Steam knows about, including the root one.
    libraries: list[Path] = list(roots)
    for root in roots:
        vdf = root / "steamapps" / "libraryfolders.vdf"
        try:
            text = vdf.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        # "path"   "D:\\SteamLibrary"
        for m in re.finditer(r'"path"\s+"([^"]+)"', text):
            libraries.append(Path(m.group(1).replace("\\\\", "\\")))

    for lib in libraries:
        manifest = lib / "steamapps" / f"appmanifest_{appid}.acf"
        try:
            text = manifest.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        m = re.search(r'"installdir"\s+"([^"]+)"', text)
        if not m:
            continue
        path = lib / "steamapps" / "common" / m.group(1)
        if path.is_dir():
            return path
    return None


def game_exe(appid: int, relative: str, env_var: str, fallback: str) -> Path:
    """Resolve a game executable: explicit override, then Steam, then the literal.

    The fallback is the path this was developed against. It is kept so a machine
    where Steam discovery fails still runs, and it is deliberately LAST so it
    cannot mask a working answer.
    """
    override = os.environ.get(env_var)
    if override:
        return Path(override)
    found = steam_app_dir(appid)
    if found:
        candidate = found / relative
        if candidate.exists():
            return candidate
    return Path(fallback)


@dataclass(frozen=True)
class Step:
    """One click on the route into gameplay, in fractions of the window.

    Fractions rather than pixels because act.ps1 is DPI-aware and resolves them
    against the real window rect at click time, so a profile stays correct if the
    window is a different size than it was when the route was measured.
    """
    fx: float
    fy: float
    wait: float
    what: str


@dataclass(frozen=True)
class PadStep:
    """One virtual-gamepad press on the route into gameplay.

    SOME GAMES ACCEPT NOTHING ELSE. Stray ignores SendInput entirely -- its
    object array stayed at the menu's slot count after four scan-code taps -- so
    its menus can only be driven through ViGEm, which the game cannot distinguish
    from real hardware.

    This existed in the PowerShell runner as `vpad --menu --menu-presses 8` and
    was lost when the two runners were generalised into one Python runner:
    GameProfile could express a click and a keypress and had no way to say
    "press A". Stray's profile was given a click at a coordinate nobody had
    validated, so `capture.py stray` reached the menu and stopped there, and the
    README documented a command that had never run. See DEBUGGING.md 8.23.
    """
    btn: str                      # A, B, X, Y, START, DU/DD/DL/DR, LB, RB, ...
    times: int = 1                # presses; menus often need several
    ms: int = 400                 # how long to hold each one
    gap: float = 0.8              # seconds between presses
    what: str = ""


@dataclass(frozen=True)
class GameProfile:
    name: str                     # short slug; also the archive directory prefix
    image: str                    # process image name, for find/kill
    exe: Path
    workdir: Path
    launch_args: str = ""
    steam_appid: int | None = None

    # --- the route into gameplay -------------------------------------------
    # Split at the load boundary: everything before the world streams in, and
    # everything after. The runner waits for the world between them, so a title
    # that loads instantly and one that takes three minutes share the same code.
    # Drive movement with the virtual pad rather than keyboard/mouse. Required
    # for any title that ignores SendInput, which is how Stray behaves.
    pad_walk: bool = False

    # A route may mix mouse clicks and pad presses; some titles accept only one.
    to_load: tuple[Step | PadStep, ...] = ()
    after_load: tuple[Step | PadStep, ...] = ()

    # --- signals, not durations --------------------------------------------
    # A fixed sleep is a guess that nobody remeasures; these are things the DLL
    # actually logs. `render_signal` is the ONLY precondition for clicking: the
    # game must be drawing. Gating clicks on engine-introspection milestones is
    # what made every inZOI run pay for work it did not need yet.
    render_signal: str = "distinct targets observed"
    load_signal: str = "world is changing"

    # Ceilings for the above, never sleeps. The old fixed values live on here so
    # the worst case is unchanged while the common case gets fast.
    menu_ceiling: float = 150.0
    load_ceiling: float = 240.0
    # How long to wait for the loaded world to appear after clicking through the
    # menu, before assuming the click missed and clicking again.
    #
    # This was a hardcoded 25 in runner.py, in a file whose entire premise is
    # "signals, not durations", and it was never once satisfied: 0 of 58
    # archived runs. So every inZOI run ever made clicked Continue and the save
    # slot, timed out, and clicked BOTH AGAIN into a load that was already
    # running -- then logged "attempt 2 after 0s", which was the FIRST click's
    # load finally registering. The retry was self-correcting enough to hide a
    # ceiling that could not be met.
    #
    # The gate now watches the LEVEL NAME rather than object-count churn (see
    # runner.into_gameplay). Measured across 22 archived runs that reached a
    # world, render_signal -> OpeningLevel2 becoming RedCity_Map:
    #
    #     min 41.5s   median 69.9s   p90 73.4s   max 74.5s
    #
    # plus one observed outlier at 118.4s with the save no longer disk-cached.
    # 120s covers that; the common case exits at ~70s and pays nothing.
    load_begin_ceiling: float = 120.0

    # The world the title sits in BEFORE a save is loaded, by name.
    #
    # "Are we in gameplay?" was inferred twice and wrong both times. Latching the
    # first world to resolve as the menu assumes the name appears before the load
    # finishes; declaring that a title has no menu world assumes the opposite.
    # Stray does both depending on timing -- ProcessEvent installs somewhere
    # between t=40s and t=80s, the pad clears the title screen at t=10s, so the
    # first name observed is HK_Project_MainStart on a fast resolve and
    # Slums_ZONE on a slow one. Each observation produced a confident and
    # opposite conclusion.
    #
    # Naming it removes the race entirely: in gameplay means a world resolved and
    # it is not this one. No ordering assumption, no latch. Empty falls back to
    # the latch for a title whose menu world is not yet known.
    menu_world: str = ""

    # How many live UObjects mean "the engine is up", for THIS title.
    #
    # ProcessEvent is installed once the object array stops growing above this
    # floor, and everything downstream needs it: marking, world state, the level
    # name the load gate waits for. The floor existed as a hardcoded 200,000,
    # picked from inZOI's menu population of ~245,000.
    #
    # Stray's menu is ~175,000 (run_auto.ps1 measured 175k at the menu, 320k+ in
    # a level). So on Stray the floor could never be met at the menu: the gate
    # waited out its whole ceiling every run, ProcessEvent went in late or not at
    # all, and the level name the harness was waiting for could not exist. The
    # long "menu wait" was our own instrument, not the game -- Stray was loaded
    # and idle the entire time.
    #
    # It is a property of the title, so it belongs in the title's profile.
    object_plateau: int = 200_000

    # Some titles load paused and need an explicit unpause; some do not.
    settle_after_unpause: float = 60.0

    # Key that resumes the simulation, sent instead of clicking a transport
    # button. Empty means the title has no separate sim clock.
    resume_key: str = ""
    # Vertical position of the transport bar, as a window fraction. The play
    # button's X is found by sweeping, because it moves between builds and a
    # single guess already cost us every capture so far.
    transport_y: float = 0.0


_INZOI_EXE = game_exe(
    2456740, r"BlueClient\Binaries\Win64\inZOI-Win64-Shipping.exe", "SEGCAP_INZOI_EXE",
    r"C:\Program Files (x86)\Steam\steamapps\common\inZOI\BlueClient\Binaries\Win64\inZOI-Win64-Shipping.exe")

INZOI = GameProfile(
    name="inzoi",
    image="inZOI-Win64-Shipping.exe",
    exe=_INZOI_EXE,
    workdir=_INZOI_EXE.parent,
    launch_args="-dx12",
    steam_appid=2456740,
    # Observed on every inZOI run: the menu is OpeningLevel2 and a loaded save
    # is RedCity_Map. Naming it here means the load gate no longer depends on
    # the level oracle resolving before the load starts.
    menu_world="OpeningLevel2",
    to_load=(
        Step(0.0883, 0.2169, 2, "Continue"),
        Step(0.6926, 0.3631, 3, "first save slot"),
    ),
    # The sim loads PAUSED. Clicking play is what starts time; the clock in the
    # bottom-left advancing is how you know it worked.
    # No transport CLICK: '1' resumes at normal speed, and a key cannot land on
    # the pause button next door, which is what the click was doing.
    after_load=(),
    resume_key="",
    transport_y=0.9606,
)


# Hk_project, not Stray. The directory under common/ is "Stray" and the one
# under it is "Hk_project" -- an internal project name, which is exactly the
# sort of thing you cannot infer and have to look at. This profile carried
# `Stray\Binaries\Win64` from the day it was written, so the Python harness
# had never once launched Stray; the PowerShell runner it replaced had the
# right path all along (tools/legacy/run_auto.ps1:98).
_STRAY_EXE = game_exe(
    1332010, r"Hk_project\Binaries\Win64\Stray-Win64-Shipping.exe", "SEGCAP_STRAY_EXE",
    r"C:\Program Files (x86)\Steam\steamapps\common\Stray\Hk_project\Binaries\Win64\Stray-Win64-Shipping.exe")

STRAY = GameProfile(
    name="stray",
    image="Stray-Win64-Shipping.exe",
    exe=_STRAY_EXE,
    workdir=_STRAY_EXE.parent,
    launch_args="-dx12",
    steam_appid=1332010,
    # PAD, NOT MOUSE. Stray ignores SendInput -- four scan-code taps left the
    # object array at the menu's slot count -- so the menu is driven through
    # ViGEm, which it cannot tell from real hardware.
    #
    # Eight presses of A, which is what run_auto.ps1 used (`--menu-presses 8`).
    # More than the menu strictly needs, deliberately: A is idempotent once the
    # save is loading, and the depth of the menu varies with whether the title
    # screen has a "press any button" gate that frame. Under-pressing leaves it
    # in the menu; over-pressing costs nothing.
    #
    # The click that used to be here -- Step(0.5, 0.62, ...) -- was a coordinate
    # nobody validated, and it never worked. DEBUGGING.md 8.23.
    to_load=(PadStep(btn="A", times=8, ms=250, gap=0.9,
                     what="menu: A x8 on the virtual pad"),),
    # The start map, observed. Gameplay is Slums_ZONE.
    menu_world="HK_Project_MainStart",
    # ~175k at the menu, 320k+ in a level. 120k clears boot without needing a
    # population Stray never reaches before a save is loaded.
    object_plateau=120_000,
    # Stray discards SendInput, so the cat only moves on the pad.
    pad_walk=True,
    after_load=(),
    menu_ceiling=90.0,
    load_ceiling=120.0,
)


PROFILES: dict[str, GameProfile] = {p.name: p for p in (INZOI, STRAY)}
