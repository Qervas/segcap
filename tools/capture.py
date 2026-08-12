"""One entry point for every title.

    python tools/capture.py inzoi --inject
    python tools/capture.py stray  --seconds 120
    python tools/capture.py inzoi --preflight        # no game; validates the flow

--preflight is why the harness moved off PowerShell. Four runs died to PowerShell
mistakes -- a relative path resolved against the process CWD, a kill on an
already-exited pid, a function called above its definition -- and none were hard
bugs. What made them expensive is that there is no compile step, so each surfaced
only when its line ran, four minutes in, costing a loaded game world each time.
Preflight walks the whole route with the game stubbed out and fails in a second.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from games import PROFILES          # noqa: E402
from runner import Run              # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", choices=sorted(PROFILES))
    ap.add_argument("--seconds", type=int, default=180, help="capture hold length")
    ap.add_argument("--inject", action="store_true",
                    help="record copies into the game's own command lists")
    ap.add_argument("--idbuf", action="store_true", help="id-buffer probe (implied by --inject)")
    ap.add_argument("--d3d-debug", action="store_true", help="D3D12 validation layer; slow")
    ap.add_argument("--no-mark", action="store_true", help="read-only; no CustomDepth writes")
    ap.add_argument("--control", "--census", dest="census", action="store_true",
                    help="CONTROL RUN. No GPU work of any kind (no readback, no injected "
                         "copies, no colour) and no marking, but otherwise an ordinary "
                         "run, so it still navigates and reports world state. Answers the "
                         "one question the crash archive cannot: does inZOI die the same "
                         "way in gameplay when we touch nothing? Overrides "
                         "--inject/--idbuf/--groundtruth/--mark")
    ap.add_argument("--groundtruth", action="store_true",
                    help="mid-run, unmark exactly one slot and check that precisely its "
                         "pixels vanish. The only check here that cannot be satisfied by "
                         "coherent-but-wrong data -- every other one reads the buffer, this "
                         "one changes something and requires the mask to follow")
    ap.add_argument("--walk", action="store_true",
                    help="drive the character during the hold. OFF by default: motion "
                         "streams, streaming churns the object graph, and that churn "
                         "destroys the marked slots the mask is built from")
    ap.add_argument("--captures", type=int, default=400,
                    help="mask budget for the run (was pinned at 60 by a stale marker file)")
    ap.add_argument("--stride", type=int, default=8,
                    help="keep every Nth frame; 8 is ~4/sec at 30fps")
    ap.add_argument("--retries", type=int, default=3,
                    help="relaunch and try again if a run captures nothing. inZOI crashes "
                         "during world streaming often enough that a single attempt is a "
                         "coin flip; the capture itself is reliable once it arms")
    ap.add_argument("--preflight", action="store_true",
                    help="validate the control flow without launching anything")
    ap.add_argument("--menu-ceiling", type=float, default=None,
                    help="override the render-signal ceiling (a CEILING, not a sleep)")
    ap.add_argument("--load-ceiling", type=float, default=None,
                    help="override the world-load ceiling (a CEILING, not a sleep)")
    args = ap.parse_args()

    profile = PROFILES[args.game]
    if args.menu_ceiling is not None:
        profile = replace_ceiling(profile, menu=args.menu_ceiling)
    if args.load_ceiling is not None:
        profile = replace_ceiling(profile, load=args.load_ceiling)

    # RETRY, because the failure is the game, not the capture.
    #
    # inZOI dies during world streaming on most attempts -- E_ABORT out of
    # ResizeBuffers, or E_INVALIDARG from its own Close() -- at 45s, 66s, 70s,
    # 79s, 114s across today's runs, all before the readback ever arms. Once a
    # run does reach gameplay it captures fine: the successful one produced 401
    # masks over a full 300s hold. Retrying converts a coin flip into a capture,
    # and a harness that gives up after one crash is not automation.
    attempts = max(1, args.retries)
    for attempt in range(1, attempts + 1):
        run = Run(profile, args, preflight=args.preflight)
        rc = run.go()
        if args.preflight:
            return rc
        # A CONTROL RUN'S RESULT IS SURVIVAL, NOT MASKS.
        #
        # Census produces zero masks by construction -- that is the point. Judging
        # it by mask count would relaunch three more times and report "no capture
        # after 4 attempts" about an experiment that had already answered its
        # question.
        if getattr(args, "census", False):
            # A CONTROL IS ONLY A CONTROL IF IT REACHED THE SAME STATE.
            #
            # The first version of this printed "the game DIED anyway -- so this
            # crash is not something we are doing to it" about a run that died
            # in the MENU, at "the world to begin loading (attempt 2)", having
            # never reached gameplay. The crash under investigation happens in
            # the open city with is_loading FALSE; a death in the menu is a
            # different event and says nothing about it.
            #
            # Which is the same unchecked-claim bug this file already fixed once
            # for the retry message, committed an hour earlier, reintroduced
            # immediately in new code. The lesson evidently has to be applied,
            # not just known.
            # "Armed" is not the same as "armed in the world". A run whose level
            # oracle never resolved arms anyway -- the refusal only fires when
            # the level is KNOWN and still the menu's -- so requiring outcome
            # alone would let an unverified run render a verdict, which is the
            # exact failure this gate exists to prevent.
            if run.outcome == "armed" and not run.armed_level:
                print(f"[{profile.name}] CONTROL INCONCLUSIVE: armed, but the world was "
                      f"never identified, so there is no evidence it reached gameplay "
                      f"rather than sitting in the menu.")
                return 1
            if run.outcome != "armed":
                # RETRY, don't give up. Roughly half of all attempts die before
                # gameplay, so returning here would report INCONCLUSIVE most of
                # the time and burn the launch for nothing. The verdict needs ONE
                # attempt that reaches the city; getting there is what retries
                # are for, exactly as they are for a capture run.
                print(f"[{profile.name}] control attempt {attempt} never reached gameplay "
                      f"(outcome: {run.outcome}); retrying")
                if attempt < attempts:
                    continue
                print(f"[{profile.name}] CONTROL INCONCLUSIVE after {attempts} attempts: "
                      f"never reached gameplay, so nothing can be compared against a "
                      f"capture run that did. Nothing is proven either way.")
                return 1
            verdict = ("the game DIED IN GAMEPLAY anyway, with us doing no GPU work and "
                       "no marking -- so this crash is not something we are doing to it"
                       if run.died else
                       "the game SURVIVED the full hold in gameplay with us doing nothing "
                       "-- which points back at our GPU work or our marking")
            print(f"[{profile.name}] CONTROL RESULT: {verdict}")
            print(f"[{profile.name}] one run is one sample. Repeat on a fresh boot before "
                  f"treating this as established, and check "
                  f"%LOCALAPPDATA%\\BlueClient\\Saved\\Crashes for a matching report.")
            return 0

        masks = len(list((Path(__file__).resolve().parent.parent /
                          "build" / "bin").glob("segcap_mask_*.pgm")))
        if masks:
            print(f"[{profile.name}] captured {masks} masks on attempt {attempt}")
            return rc
        if attempt < attempts:
            # SAY WHICH FAILURE IT WAS, having looked.
            #
            # This printed "the game died before arming" unconditionally, which
            # is a cause it never checked. THREE different things land here and
            # they need opposite follow-up:
            #
            #   armed          -- reached gameplay, then died. The open
            #                     ResizeBuffers crash (DEBUGGING.md 8.16).
            #   refused        -- the world never loaded, so capture() declined
            #                     to photograph the menu. Navigation/timing.
            #   never-got-there -- died on the way in.
            #
            # The first version of this fix distinguished only the first from
            # "the rest" and then asserted the rest were the game dying -- which
            # printed "the game died before reaching gameplay" for a run whose
            # game was alive and had simply not loaded. Same unchecked claim,
            # one branch over. The runner knows; it now says.
            why = {
                "armed": "it ARMED and then stopped presenting -- the game died "
                         "during capture, not before it",
                "refused": "it reached gameplay but the world never loaded, so it "
                           "refused to arm rather than capture the menu",
                "never-got-there": "it never reached a loaded world",
            }[run.outcome]
            print(f"[{profile.name}] attempt {attempt} captured nothing ({why}); retrying")
    print(f"[{profile.name}] no capture after {attempts} attempts")
    return 1


def replace_ceiling(profile, menu: float | None = None, load: float | None = None):
    import dataclasses
    changes = {}
    if menu is not None:
        changes["menu_ceiling"] = menu
    if load is not None:
        changes["load_ceiling"] = load
    return dataclasses.replace(profile, **changes)


if __name__ == "__main__":
    sys.exit(main())
