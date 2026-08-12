"""Animate object-ID masks on their own, with no colour frames to overlay onto.

make_demo.py pairs each mask with the colour frame from the same Present, which
is the right deliverable -- but it needs both. inZOI currently captures masks
without frames (the colour path enqueues every Present into a 3-slot ring while
only ~61 of 23,000 masks are kept, so exact-index pairing almost never hits).

That is worth showing anyway: the ids, their stability across frames, and the
fact that object boundaries track the scene. Same palette as overlay.py, so a
given id is the same colour here as in the paired demo.

    python tools/mask_movie.py --dir build/bin --out docs/evidence/inzoi_masks.gif
"""

from __future__ import annotations

import argparse
import glob
import os
import re

import numpy as np
from PIL import Image

from overlay import palette, read_pgm


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/bin")
    ap.add_argument("--out", default="docs/evidence/inzoi_masks.gif")
    ap.add_argument("--fps", type=int, default=8)
    # FULL RESOLUTION by default. 0.5 turned 1280x800 masks into 640x400 for no
    # reason other than keeping a GIF small -- and the GIF itself then quantised
    # 255 object ids into a 256-colour palette, merging ids that the mask had
    # kept distinct. Both losses were invisible in the code and obvious on screen.
    ap.add_argument("--scale", type=float, default=1.0)
    # Track a subject the way make_demo.py tracks Stray's cat: everything keeps
    # its id colour, but the subject stays saturated while the rest of the scene
    # is dimmed, so you can follow one object across the session by eye. On inZOI
    # the Zois are skeletal meshes, so that class IS the character filter.
    ap.add_argument("--highlight-class", default="",
                    help="sidecar className substring to keep bright (e.g. SkeletalMesh)")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dir, "segcap_mask_*.pgm")),
                   key=lambda p: int(re.search(r"(\d+)", os.path.basename(p)).group(1)))
    if not paths:
        print(f"no masks in {args.dir}")
        return 1

    pal = palette()
    frames = []
    tracked: set[int] = set()
    for p in paths:
        mask = read_pgm(p)

        # Which ids are the subject THIS frame. Read per frame, not once: slots
        # are leased and re-leased as objects come and go, so an id that names a
        # character now may name a wall later. The sidecar is the authority for
        # the frame it was published with -- that pairing is the whole point of
        # writing one per mask.
        keep: set[int] = set()
        if args.highlight_class:
            sc = os.path.splitext(p)[0] + ".json"
            if os.path.exists(sc):
                import json
                with open(sc) as fh:
                    for b in json.load(fh).get("bindings", []):
                        if args.highlight_class.lower() in str(b.get("className", "")).lower():
                            keep.add(int(b["slot"]))
                tracked |= keep
        # Nearest-neighbour throughout: these are ids, not intensities. Averaging
        # id 60 and id 158 gives 109, which is a different object or none.
        arr = pal[mask].astype(np.float32)
        if keep:
            # Dim everything that is not the subject. Applied to the COLOUR, never
            # to the ids -- the mask itself is untouched, so no id is ever blended
            # into a different one.
            sel = np.isin(mask, list(keep))[..., None]
            arr = np.where(sel, arr, arr * 0.30 + 26.0)
        rgb = Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGB")
        if args.scale != 1.0:
            w = int(rgb.width * args.scale)
            h = int(rgb.height * args.scale)
            rgb = rgb.resize((w, h), Image.NEAREST)
        # Keep RGB for video. Converting to a 256-colour palette here is what
        # merged neighbouring ids in the GIF path.
        frames.append(rgb if args.out.lower().endswith(".mp4")
                      else rgb.convert("P", palette=Image.ADAPTIVE, colors=256))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    if args.out.lower().endswith(".mp4"):
        # Straight to video: no GIF in the middle, no palette, no second encode.
        # -crf 16 because these are flat colour regions with hard edges, exactly
        # what a lossy codec smears first.
        import subprocess, tempfile
        from make_demo import find_ffmpeg
        with tempfile.TemporaryDirectory() as tmp:
            for i, im in enumerate(frames):
                im.save(os.path.join(tmp, "f%05d.png" % i))
            subprocess.run([find_ffmpeg(), "-y", "-loglevel", "error",
                            "-framerate", str(args.fps),
                            "-i", os.path.join(tmp, "f%05d.png"),
                            "-c:v", "libx264", "-preset", "slow", "-crf", "16",
                            "-pix_fmt", "yuv420p", args.out], check=False)
    else:
        frames[0].save(args.out, save_all=True, append_images=frames[1:],
                       duration=int(1000 / args.fps), loop=0, optimize=False)
    ids = set()
    for p in paths:
        ids |= set(np.unique(read_pgm(p)).tolist())
    ids.discard(0)
    print(f"wrote {args.out}: {len(frames)} frames @ {args.fps}fps "
          f"= {len(frames) / args.fps:.1f}s, {len(ids)} distinct object ids across the session")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
