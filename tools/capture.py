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
    ap.add_argument("--walk", action="store_true",
                    help="drive the character during the hold. OFF by default: motion "
                         "streams, streaming churns the object graph, and that churn "
                         "destroys the marked slots the mask is built from")
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

    return Run(profile, args, preflight=args.preflight).go()


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
