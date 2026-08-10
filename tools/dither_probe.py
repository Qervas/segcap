"""Characterise the stipple in a mask: regular dither lattice, or something else?

verify_labels.py reports that 63.7% of inZOI regions are "dithered", but it
decides that from fragmentation alone -- a region broken into as many pieces as
it has pixels. Fragmentation is consistent with several different causes and
does not distinguish between them:

  * a regular screen-space dither (UE's dithered LOD transition / dithered
    opacity masks discard pixels on a fixed 2x2 or 4x4 lattice)
  * temporal jitter, where each frame writes a different pseudo-random subset
  * genuine per-pixel interleaving of two objects that really do overlap
  * a readback that is sampling the wrong plane or a wrong row pitch

They imply completely different fixes, so the pattern has to be measured rather
than named. This script asks one question per region: do its pixels sit on a
fixed lattice?

Method: for period p in {2, 4, 8}, bin every pixel of a region by
(x mod p, y mod p). A solid region spreads evenly over all p*p residues. A
regular dither concentrates in a strict subset -- half the residues for a 50%
2x2 dither, a quarter for 25%, and so on. The concentration ratio (occupied
residues / total residues) is the discriminator, and it is invariant to the
region's shape, size and position, which fragmentation counts are not.

Usage:
    python tools/dither_probe.py build/bin            # whole session
    python tools/dither_probe.py build/bin --frame 12042
"""

import argparse
import glob
import json
import os
import re
from collections import Counter, defaultdict


def read_pgm(path):
    """Minimal binary P5 reader. Returns (width, height, bytes)."""
    with open(path, "rb") as fh:
        data = fh.read()
    # Header is ASCII tokens, possibly with # comments, then a single whitespace
    # byte before the raster. Parsing it by hand avoids a numpy/PIL dependency,
    # which matters because this has to run on the capture machine.
    tokens, pos = [], 0
    while len(tokens) < 4:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    magic, width, height, maxval = tokens
    if magic != b"P5":
        raise ValueError("%s: not binary PGM (%r)" % (path, magic))
    pos += 1  # exactly one whitespace byte separates header from raster
    w, h = int(width), int(height)
    raster = data[pos : pos + w * h]
    if len(raster) != w * h:
        raise ValueError("%s: short raster %d != %d" % (path, len(raster), w * h))
    return w, h, raster


def analyse_region(pixels, periods=(2, 4, 8)):
    """pixels: list of (x, y). Returns {period: occupied_fraction_of_residues}."""
    out = {}
    for p in periods:
        residues = Counter()
        for x, y in pixels:
            residues[(x % p, y % p)] += 1
        # Count residues carrying a non-trivial share. A residue holding a
        # handful of pixels out of thousands is noise (edges, occluders), not
        # evidence that the lattice is occupied.
        floor = max(1, len(pixels) // (p * p * 8))
        occupied = sum(1 for c in residues.values() if c > floor)
        out[p] = occupied / float(p * p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--frame", type=int, default=None)
    ap.add_argument("--min-px", type=int, default=300,
                    help="ignore regions smaller than this (too few pixels to "
                         "distinguish a lattice from chance)")
    ap.add_argument("--max-frames", type=int, default=12)
    args = ap.parse_args()

    masks = sorted(glob.glob(os.path.join(args.directory, "segcap_mask_*.pgm")))
    if args.frame is not None:
        masks = [m for m in masks
                 if re.search(r"segcap_mask_(\d+)\.pgm$", m)
                 and int(re.search(r"segcap_mask_(\d+)\.pgm$", m).group(1)) == args.frame]
    masks = masks[: args.max_frames]
    if not masks:
        print("no masks found in %s" % args.directory)
        return

    verdicts = Counter()
    per_period = defaultdict(list)
    examples = []

    for path in masks:
        w, h, raster = read_pgm(path)
        sidecar = path[:-4] + ".json"
        names = {}
        if os.path.exists(sidecar):
            with open(sidecar, "r", encoding="utf-8") as fh:
                doc = json.load(fh)
            for b in doc.get("bindings", []):
                names[b["slot"]] = b.get("objectName", "")

        by_slot = defaultdict(list)
        for y in range(h):
            row = y * w
            for x in range(w):
                v = raster[row + x]
                if v:
                    by_slot[v].append((x, y))

        for slot, pixels in by_slot.items():
            if len(pixels) < args.min_px:
                continue
            frac = analyse_region(pixels)
            for p, f in frac.items():
                per_period[p].append(f)
            # A region is "lattice-locked" when it occupies clearly fewer than
            # all residues at some period -- i.e. entire phases of the grid are
            # empty, which shape alone cannot produce.
            best_p = min(frac, key=lambda p: frac[p])
            if frac[best_p] <= 0.60:
                verdicts["lattice"] += 1
                if len(examples) < 10:
                    examples.append((frac[best_p], best_p, os.path.basename(path),
                                     slot, names.get(slot, ""), len(pixels)))
            else:
                verdicts["solid"] += 1

    total = sum(verdicts.values())
    print("frames analysed : %d" % len(masks))
    print("regions >= %dpx : %d" % (args.min_px, total))
    if not total:
        return
    print("")
    print("residue occupancy by period (1.00 = every phase of the grid is used)")
    for p in sorted(per_period):
        vals = sorted(per_period[p])
        med = vals[len(vals) // 2]
        print("   period %d : median %.2f   min %.2f   max %.2f"
              % (p, med, vals[0], vals[-1]))
    print("")
    print("lattice-locked regions : %d (%.1f%%)"
          % (verdicts["lattice"], 100.0 * verdicts["lattice"] / total))
    print("solid regions          : %d (%.1f%%)"
          % (verdicts["solid"], 100.0 * verdicts["solid"] / total))

    if examples:
        print("")
        print("most lattice-locked:")
        for frac, p, fname, slot, name, npx in sorted(examples)[:10]:
            print("   %.2f  period %d  %-24s slot %-4d %-32s %6d px"
                  % (frac, p, fname, slot, name[:32], npx))

    print("")
    print("reading the numbers")
    print("  occupancy ~1.00 everywhere      -> solid coverage; stipple is NOT a dither")
    print("  occupancy ~0.50 at period 2     -> classic 50%% checkerboard dither")
    print("  occupancy ~0.25-0.50 at 4 or 8  -> UE dithered LOD transition / dithered opacity")
    print("  low at 2 AND 4 AND 8, varying   -> pseudo-random per-pixel discard, not a fixed grid")


if __name__ == "__main__":
    main()
