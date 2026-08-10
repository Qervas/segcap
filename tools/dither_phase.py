"""Two follow-up questions the occupancy number alone cannot answer.

dither_probe.py established that 96.7% of inZOI regions sit on a 50% checkerboard
at period 2. That is too universal to be dithered LOD transitions, which only
affect meshes actually mid-transition. So:

  Q1. Is the UNION of all labelled pixels solid?
      If each slot covers half the lattice but the union covers all of it, the
      objects are interleaving WITH EACH OTHER -- the buffer is fully written and
      only the per-slot assignment alternates. That is a completely different
      problem from "half the pixels were discarded", and it would mean coverage
      is not actually lost, only shredded between neighbours.

  Q2. Is the checkerboard PHASE stable across frames?
      A phase that flips every frame is a temporal dither, and two consecutive
      frames would reconstruct full coverage exactly. A phase that never moves is
      spatial, and no amount of accumulation will fill it.

These two answers pick the fix between "accumulate two frames", "morphological
close", and "disable the dither inside the engine".
"""

import glob
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dither_probe import read_pgm  # noqa: E402


def frame_of(path):
    m = re.search(r"segcap_mask_(\d+)\.pgm$", path)
    return int(m.group(1)) if m else -1


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "build/bin"
    masks = sorted(glob.glob(os.path.join(directory, "segcap_mask_*.pgm")),
                   key=frame_of)[:8]
    if not masks:
        print("no masks in %s" % directory)
        return

    print("Q1. union coverage -- is the buffer fully written?")
    print("")
    for path in masks[:5]:
        w, h, raster = read_pgm(path)
        union = Counter()
        per_slot_px = Counter()
        for y in range(h):
            row = y * w
            for x in range(w):
                v = raster[row + x]
                if v:
                    union[(x % 2, y % 2)] += 1
                    per_slot_px[v] += 1
        tot = sum(union.values())
        if not tot:
            continue
        shares = sorted(union[(a, b)] / float(tot)
                        for a in range(2) for b in range(2))
        print("   %-24s labelled px %8d   phase shares %s"
              % (os.path.basename(path), tot,
                 " ".join("%.2f" % s for s in shares)))
    print("")
    print("   equal shares (~0.25 each) -> union is SOLID; slots interleave with")
    print("   each other and no coverage is missing, only misassigned.")
    print("   uneven shares             -> the buffer itself is half empty.")

    print("")
    print("Q2. phase stability per slot across frames")
    print("")
    # dominant phase of each slot, per frame
    slot_phase = defaultdict(dict)
    for path in masks:
        w, h, raster = read_pgm(path)
        acc = defaultdict(Counter)
        for y in range(h):
            row = y * w
            for x in range(w):
                v = raster[row + x]
                if v:
                    acc[v][(x % 2, y % 2)] += 1
        f = frame_of(path)
        for slot, phases in acc.items():
            if sum(phases.values()) < 500:
                continue
            slot_phase[slot][f] = max(phases, key=lambda k: phases[k])

    stable = flipping = 0
    shown = 0
    for slot, byframe in sorted(slot_phase.items()):
        if len(byframe) < 3:
            continue
        phases = [byframe[f] for f in sorted(byframe)]
        distinct = len(set(phases))
        if distinct == 1:
            stable += 1
        else:
            flipping += 1
        if shown < 8:
            print("   slot %-4d frames %d  phases %s%s"
                  % (slot, len(phases), " ".join("%d%d" % p for p in phases[:8]),
                     "   <- STABLE" if distinct == 1 else "   <- moves"))
            shown += 1
    print("")
    print("   stable phase : %d slots" % stable)
    print("   moving phase : %d slots" % flipping)
    print("")
    print("   all stable -> spatial dither; accumulation cannot help, must be")
    print("                 disabled in-engine or closed morphologically.")
    print("   moving     -> temporal dither; accumulating consecutive frames")
    print("                 reconstructs full coverage.")


if __name__ == "__main__":
    main()
