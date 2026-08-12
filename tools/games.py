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

from dataclasses import dataclass, field
from pathlib import Path


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
    to_load: tuple[Step, ...] = ()
    after_load: tuple[Step, ...] = ()

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

    # Some titles load paused and need an explicit unpause; some do not.
    settle_after_unpause: float = 60.0

    # Key that resumes the simulation, sent instead of clicking a transport
    # button. Empty means the title has no separate sim clock.
    resume_key: str = ""
    # Vertical position of the transport bar, as a window fraction. The play
    # button's X is found by sweeping, because it moves between builds and a
    # single guess already cost us every capture so far.
    transport_y: float = 0.0


INZOI = GameProfile(
    name="inzoi",
    image="inZOI-Win64-Shipping.exe",
    exe=Path(r"C:\Program Files (x86)\Steam\steamapps\common\inZOI\BlueClient\Binaries\Win64\inZOI-Win64-Shipping.exe"),
    workdir=Path(r"C:\Program Files (x86)\Steam\steamapps\common\inZOI\BlueClient\Binaries\Win64"),
    launch_args="-dx12",
    steam_appid=2456740,
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


STRAY = GameProfile(
    name="stray",
    image="Stray-Win64-Shipping.exe",
    exe=Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stray\Stray\Binaries\Win64\Stray-Win64-Shipping.exe"),
    workdir=Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stray\Stray\Binaries\Win64"),
    launch_args="-dx12",
    # Stray drops into gameplay from a single continue; it does not load paused.
    to_load=(Step(0.5, 0.62, 4, "continue"),),
    after_load=(),
    menu_ceiling=90.0,
    load_ceiling=120.0,
)


PROFILES: dict[str, GameProfile] = {p.name: p for p in (INZOI, STRAY)}
