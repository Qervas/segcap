"""
overlay.py -- turn a stencil mask into something a human can check.

    python tools/overlay.py mask.pgm                    colourised mask only
    python tools/overlay.py mask.pgm --frame frame.png  blended over the frame
    python tools/overlay.py mask.pgm --sidecar sc.json  label regions by object

The brief is explicit that "a number in a log is not evidence" and asks for masks
overlaid on captured video to prove they line up. This is that check.

Design notes:

- Colours come from golden-ratio hue stepping, not a random palette. Adjacent
  ids get maximally distinct hues, which matters because neighbouring objects in
  a scene often get adjacent slot numbers, and two similar greens touching each
  other look like one object.

- Id 0 (unmarked) is rendered fully transparent rather than black. Painting it
  black would hide misalignment: a mask offset by a few pixels still looks
  plausible against a dark background, and edge alignment is exactly what this
  tool exists to check.

- Region centroids and pixel counts are printed, so "does this line up" can be
  answered numerically as well as visually.
"""

import argparse
import colorsys
import json
import os
import sys

import numpy as np
from PIL import Image

from mask_align import AlignmentError, align_mask_to_frame


def read_pgm(path):
    """Binary PGM (P5). Parsed by hand -- Pillow's PGM support is fine but the
    header is fully known here and this avoids a surprise on a malformed file."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P5"):
        raise SystemExit("%s is not a binary PGM" % path)

    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1

    w, h, _ = fields
    px = np.frombuffer(data[pos:pos + w * h], dtype=np.uint8)
    if px.size != w * h:
        raise SystemExit("truncated PGM: expected %d bytes, got %d" % (w * h, px.size))
    return px.reshape((h, w))


def palette():
    """256 visually distinct RGB colours; index 0 is unused (transparent).

    Golden-ratio hue stepping rather than random or sequential: sequential hues
    make adjacent ids nearly identical, and adjacent ids are common between
    neighbouring objects. Random palettes produce occasional near-collisions.
    """
    pal = np.zeros((256, 3), dtype=np.uint8)
    golden = 0.61803398875
    h = 0.0
    for i in range(1, 256):
        h = (h + golden) % 1.0
        # Alternate value/saturation so even a hue collision differs in tone.
        v = 0.95 if i % 2 else 0.72
        s = 0.85 if i % 3 else 0.60
        r, g, b = colorsys.hsv_to_rgb(h, s, v)
        pal[i] = (int(r * 255), int(g * 255), int(b * 255))
    return pal


def analyse(mask):
    """Per-id pixel counts and centroids -- the numeric half of the evidence."""
    ids, counts = np.unique(mask, return_counts=True)
    rows = []
    for i, c in zip(ids, counts):
        if i == 0:
            continue
        ys, xs = np.nonzero(mask == i)
        rows.append({
            "id": int(i),
            "pixels": int(c),
            "percent": 100.0 * c / mask.size,
            "centroid": (float(xs.mean()), float(ys.mean())),
            "bbox": (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())),
        })
    rows.sort(key=lambda r: -r["pixels"])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mask")
    ap.add_argument("--frame", help="captured colour frame to overlay onto")
    ap.add_argument("--sidecar", help="JSON slot->object table for this frame")
    ap.add_argument("--alpha", type=float, default=0.55)
    ap.add_argument("--out", help="output PNG (default: <mask>.overlay.png)")
    args = ap.parse_args()

    mask = read_pgm(args.mask)
    h, w = mask.shape
    print("mask   : %s  %dx%d" % (args.mask, w, h))

    rows = analyse(mask)
    labelled = sum(r["pixels"] for r in rows)
    print("objects: %d distinct ids, %.2f%% of pixels labelled"
          % (len(rows), 100.0 * labelled / mask.size))

    names = {}
    if args.sidecar and os.path.exists(args.sidecar):
        with open(args.sidecar) as f:
            sc = json.load(f)
        for b in sc.get("bindings", []):
            names[int(b["slot"])] = "%s (%s) id=%s" % (
                b.get("objectName", "?"), b.get("className", "?"), b.get("stableId", "?"))
        print("sidecar: %s, %d bindings" % (args.sidecar, len(names)))
    elif args.sidecar:
        print("sidecar: %s NOT FOUND -- ids will be unlabelled" % args.sidecar)

    print()
    print("%-6s %10s %8s %-20s %s" % ("id", "pixels", "percent", "centroid", "object"))
    print("-" * 78)
    for r in rows[:20]:
        print("%-6d %10d %7.3f%% (%7.1f,%7.1f)  %s"
              % (r["id"], r["pixels"], r["percent"],
                 r["centroid"][0], r["centroid"][1], names.get(r["id"], "")))

    # ---- render ----------------------------------------------------------
    # The mask is upscaled to the frame, never the reverse -- see mask_align.
    # This has to happen BEFORE the palette lookup: pal[mask] turns ids into
    # colours, and once they are colours a resize would blend across object
    # boundaries with nothing to complain about it.
    if args.frame:
        base = Image.open(args.frame).convert("RGB")
        try:
            mask = align_mask_to_frame(mask, base.size)
        except AlignmentError as exc:
            print("\nERROR: %s" % exc)
            return 1
        h, w = mask.shape

    pal = palette()
    rgb = pal[mask]                              # (h, w, 3)
    alpha = np.where(mask > 0, 255, 0).astype(np.uint8)

    if args.frame:
        arr = np.asarray(base).astype(np.float32)
        a = (alpha[..., None] / 255.0) * args.alpha
        # Unmarked pixels stay fully transparent, so a misaligned mask shows as
        # an obvious offset rather than being hidden by a dark fill.
        out = (arr * (1.0 - a) + rgb.astype(np.float32) * a).astype(np.uint8)
        img = Image.fromarray(out, "RGB")
    else:
        img = Image.fromarray(np.dstack([rgb, alpha]), "RGBA")

    out_path = args.out or (args.mask + ".overlay.png")
    img.save(out_path)
    print("\nwrote %s" % out_path)


# Guarded so other tools can import palette() and read_pgm() without this
# running its argument parser -- make_demo.py imports both, and an unguarded
# main() made it fail with "unrecognized arguments" from a completely different
# script's parser.
if __name__ == "__main__":
    main()
