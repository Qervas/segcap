"""Is a mask real segmentation, or is it noise that merely passes the coherence checks?

verify_labels.py warns that its checks are necessary and not sufficient. This is
the failure mode it cannot see: a buffer of uniformly distributed bytes.

Noise passes identity consistency trivially (identity comes from the sidecar, not
the pixels). It passes temporal coherence trivially too -- the centroid of a
uniform random scatter sits at the middle of its area and does not move, so
nothing ever "jumps against the scene".

Three statistics separate real segmentation from noise, none of which noise can
fake:

  1. AREA DISTRIBUTION. Real scenes are dominated by a few large objects with a
     long tail of small ones; areas span orders of magnitude. Uniform bytes give
     every value near-identical area (total_px / 255).

  2. CENTROID SPREAD. Real objects sit in different places. Noise centroids all
     collapse toward the same point.

  3. COVERAGE. Real masks leave sky, UI and unmarked geometry at zero. A buffer
     where ~100% of pixels are labelled is not a segmentation of anything.

Run it against a known-good title as a control before trusting the verdict.
"""

import glob
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dither_probe import read_pgm  # noqa: E402


def analyse(path):
    w, h, raster = read_pgm(path)
    counts = Counter()
    sx = defaultdict(int)
    sy = defaultdict(int)
    for y in range(h):
        row = y * w
        for x in range(w):
            v = raster[row + x]
            if v:
                counts[v] += 1
                sx[v] += x
                sy[v] += y
    total = w * h
    labelled = sum(counts.values())
    if not counts:
        return None

    areas = sorted(counts.values(), reverse=True)
    cents = [(sx[v] / counts[v], sy[v] / counts[v]) for v in counts]
    mx = sum(c[0] for c in cents) / len(cents)
    my = sum(c[1] for c in cents) / len(cents)
    spread = (sum(((c[0] - mx) ** 2 + (c[1] - my) ** 2) for c in cents) / len(cents)) ** 0.5

    return {
        "file": os.path.basename(path),
        "w": w, "h": h,
        "coverage": labelled / float(total),
        "ids": len(counts),
        "area_max": areas[0],
        "area_med": areas[len(areas) // 2],
        "area_min": areas[-1],
        "area_ratio": areas[0] / float(max(1, areas[len(areas) // 2])),
        "centroid_spread": spread,
        "diag": (w * w + h * h) ** 0.5,
    }


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "build/bin"
    masks = sorted(glob.glob(os.path.join(directory, "segcap_mask_*.pgm")))[:4]
    if not masks:
        print("no masks in %s" % directory)
        return
    print("%-22s %8s %5s %8s %8s %7s %9s" %
          ("file", "coverage", "ids", "maxarea", "medarea", "max/med", "cent.spread"))
    verdicts = []
    for p in masks:
        r = analyse(p)
        if not r:
            continue
        print("%-22s %7.1f%% %5d %8d %8d %7.1fx %8.1f px" %
              (r["file"], 100 * r["coverage"], r["ids"], r["area_max"],
               r["area_med"], r["area_ratio"], r["centroid_spread"]))
        verdicts.append(r)

    if not verdicts:
        return
    cov = sum(v["coverage"] for v in verdicts) / len(verdicts)
    ratio = sum(v["area_ratio"] for v in verdicts) / len(verdicts)
    spread = sum(v["centroid_spread"] / v["diag"] for v in verdicts) / len(verdicts)

    print("")
    print("mean coverage            : %.1f%%" % (100 * cov))
    print("mean max/median area      : %.1fx" % ratio)
    print("mean centroid spread      : %.3f of image diagonal" % spread)
    print("")
    bad = []
    # NOT a coverage check. The first version flagged "coverage ~100%" as proof
    # of noise, and the known-good control failed it: every pixel legitimately
    # carries a stencil value, so full coverage is normal and says nothing. It
    # was only ever a plausible-sounding number, and it would have condemned a
    # working title. Run every new criterion against the control before trusting
    # it -- that is what caught this one.
    if ratio < 5:
        bad.append("all ids have near-identical area (%.1fx max/median)" % ratio)
    if spread < 0.12:
        bad.append("centroids collapse toward one point (%.3f of diagonal; a real "
                   "scene spreads objects over ~0.25+)" % spread)
    ids = sum(v["ids"] for v in verdicts) / float(len(verdicts))
    if ids > 200:
        bad.append("%.0f distinct ids -- essentially every possible byte value is "
                   "present, which real segmentation does not produce" % ids)

    if bad:
        print("VERDICT: NOT SEGMENTATION -- consistent with uniformly distributed bytes")
        for b in bad:
            print("   - %s" % b)
        print("")
        print("   Most likely the readback is sampling the wrong plane (depth read as")
        print("   stencil) or the wrong resource. Coherence checks cannot detect this.")
    else:
        print("VERDICT: consistent with real segmentation")
        print("   large objects dominate, ids sit in different places, background exists")


if __name__ == "__main__":
    main()
