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
        rc = Run(profile, args, preflight=args.preflight).go()
        if args.preflight:
            return rc
        masks = len(list((Path(__file__).resolve().parent.parent /
                          "build" / "bin").glob("segcap_mask_*.pgm")))
        if masks:
            print(f"[{profile.name}] captured {masks} masks on attempt {attempt}")
            return rc
        if attempt < attempts:
            # SAY WHICH FAILURE IT WAS, having looked.
            #
            # This printed "the game died before arming" unconditionally, which
            # is a cause it never checked -- and it is often wrong. A run that
            # reaches gameplay, arms, and then dies to the ResizeBuffers crash
            # (DEBUGGING.md 8.16) lands here too, and the two need completely
            # different follow-up: one is a menu-navigation problem, the other
            # is the open crash. The log is still on disk at this point and says
            # which happened.
            log = Path(__file__).resolve().parent.parent / "build" / "bin" / "segcap.log"
            armed = False
            try:
                with log.open("r", encoding="utf-8", errors="replace") as fh:
                    armed = any("CAPTURE ARMED" in ln for ln in fh)
            except OSError:
                pass
            why = ("it ARMED and then stopped presenting -- the game died during "
                   "capture, not before it" if armed else
                   "it never armed -- the game died before reaching gameplay")
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
