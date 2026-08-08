"""
ab_diff.py -- did marking objects change what the player sees?

    python tools/ab_diff.py --off a1.png a2.png --on b1.png b2.png

The brief asks: "how you'd detect if you'd changed what the player sees". This is
that detection, and the design turns on one problem that makes a naive diff
worthless.

THE NOISE FLOOR PROBLEM
-----------------------
Stray uses temporal anti-aliasing. Consecutive frames of a completely static
scene are NOT identical -- TAA jitters the sample pattern every frame, so pixels
shift by small amounts constantly. A naive "are these two frames identical" test
therefore fails even when nothing has changed, and a naive "is the difference
small" test has no scale to judge against.

So we measure the game's own frame-to-frame variation first, from two frames
captured with marking OFF. That is the noise floor. Then we compare the
off-vs-on difference against it.

The question becomes falsifiable: does enabling CustomDepth marking change the
image by MORE than the game changes it by itself?

CustomDepth is an off-screen pass and should be invisible -- unless a
post-process samples it, which some games do for outlines. That is exactly the
case worth detecting, and it is why this is a measurement rather than an
assertion.
"""

import argparse
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


def stats(a, b, label):
    if a.shape != b.shape:
        raise SystemExit("%s: shape mismatch %s vs %s" % (label, a.shape, b.shape))
    d = np.abs(a - b)
    per_pixel = d.max(axis=2)          # worst channel per pixel
    differing = int((per_pixel > 0).sum())
    total = per_pixel.size
    return {
        "label": label,
        "max": int(per_pixel.max()),
        "mean": float(d.mean()),
        "differing_px": differing,
        "differing_pct": 100.0 * differing / total,
        "p99": int(np.percentile(per_pixel, 99)),
        "map": per_pixel,
    }


def report(s):
    print("%-28s max=%3d  mean=%6.3f  p99=%3d  differing=%8d (%6.3f%%)"
          % (s["label"], s["max"], s["mean"], s["p99"], s["differing_px"],
             s["differing_pct"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--off", nargs=2, required=True,
                    help="two frames captured with marking DISABLED")
    ap.add_argument("--on", nargs=2, required=True,
                    help="two frames captured with marking ENABLED")
    ap.add_argument("--heatmap", help="write a difference heatmap PNG here")
    args = ap.parse_args()

    off1, off2 = load(args.off[0]), load(args.off[1])
    on1, on2 = load(args.on[0]), load(args.on[1])

    print("noise floor -- the game's own frame-to-frame variation")
    print("-" * 78)
    floor_off = stats(off1, off2, "off vs off (baseline)")
    floor_on = stats(on1, on2, "on vs on")
    report(floor_off)
    report(floor_on)

    print()
    print("the actual question -- marking off vs marking on")
    print("-" * 78)
    cross = stats(off1, on1, "off vs on")
    report(cross)

    # Compare against the noise floor rather than against zero. Zero is the
    # wrong threshold for a game with temporal AA, and using it would report a
    # visual change on every comparison regardless of what we did.
    floor = max(floor_off["mean"], floor_on["mean"])
    floor_max = max(floor_off["max"], floor_on["max"])

    print()
    print("verdict")
    print("-" * 78)
    print("noise floor (mean) : %.3f" % floor)
    print("off-vs-on   (mean) : %.3f" % cross["mean"])

    if floor <= 0.0001:
        # A perfectly static capture -- then any difference at all is ours.
        verdict = cross["max"] == 0
        print("scene is pixel-static, so ANY difference is attributable to us")
    else:
        ratio = cross["mean"] / floor
        print("ratio              : %.2fx the noise floor" % ratio)
        # Within ~1.5x of the game's own variation is indistinguishable from it.
        verdict = ratio <= 1.5 and cross["max"] <= floor_max * 2

    print()
    if verdict:
        print("PASS: enabling CustomDepth marking does not change the rendered image")
        print("      beyond the game's own frame-to-frame variation.")
    else:
        print("FAIL: the image changed measurably. CustomDepth is likely being")
        print("      sampled by a post-process (outlines are the usual reason).")
        print("      This is a real finding, not a tolerance to widen.")

    if args.heatmap:
        m = cross["map"].astype(np.float32)
        m = (255.0 * m / max(m.max(), 1)).astype(np.uint8)
        # Red channel only: a greyscale heatmap is easy to mistake for the frame.
        rgb = np.dstack([m, np.zeros_like(m), np.zeros_like(m)])
        Image.fromarray(rgb, "RGB").save(args.heatmap)
        print("\nheatmap -> %s" % args.heatmap)

    return 0 if verdict else 1


sys.exit(main())
