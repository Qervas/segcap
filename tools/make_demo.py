"""
make_demo.py -- assemble captured frames and masks into the demo video.

    python tools/make_demo.py --dir build/bin --out docs/evidence/demo.mp4

Produces a side-by-side video: the game's rendered frame on the left, the same
frame with its segmentation mask overlaid on the right. Both come from the same
Present call, so they are the same frame by construction -- there is no
synchronisation step here because there is nothing to synchronise.

Each frame is annotated with its frame index and the object count from that
frame's sidecar, so the video is self-describing rather than needing narration.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from overlay import palette, read_pgm  # noqa: E402


def find_ffmpeg():
    """Locate a WORKING ffmpeg.

    The one first on PATH here (a conda env build) exits with 0xC0000139,
    STATUS_ENTRYPOINT_NOT_FOUND -- a broken DLL dependency. It produces no
    output at all, not even -version, which looks like a codec problem until you
    check the exit code. So candidates are probed by actually running them
    rather than by existence.
    """
    candidates = []
    for base in (r"C:\Users\djmax\scoop\apps\ffmpeg",):
        if os.path.isdir(base):
            for ver in sorted(os.listdir(base), reverse=True):
                candidates.append(os.path.join(base, ver, "bin", "ffmpeg.exe"))
    candidates.append("ffmpeg")

    for c in candidates:
        try:
            r = subprocess.run([c, "-hide_banner", "-version"],
                               capture_output=True, text=True, timeout=15)
            if r.returncode == 0 and "ffmpeg version" in (r.stdout + r.stderr):
                return c
        except Exception:
            continue
    return None


def frame_index(path):
    m = re.search(r"_(\d+)\.", os.path.basename(path))
    return int(m.group(1)) if m else -1


def collect(directory):
    """Pair masks with colour frames by index. Only indices with BOTH are usable
    -- a mask without its frame cannot be overlaid, and a frame without its mask
    has nothing to show."""
    masks = {frame_index(p): p for p in glob.glob(os.path.join(directory, "segcap_mask_*.pgm"))}
    frames = {}
    for ext in ("ppm", "png"):
        for p in glob.glob(os.path.join(directory, "segcap_frame_*." + ext)):
            frames.setdefault(frame_index(p), p)

    paired = sorted(set(masks) & set(frames))
    print("masks: %d, frames: %d, paired: %d" % (len(masks), len(frames), len(paired)))
    if len(paired) < len(masks):
        missing = sorted(set(masks) - set(frames))
        print("  %d mask(s) have no matching frame: %s"
              % (len(missing), missing[:8]))
    return [(i, frames[i], masks[i]) for i in paired]


def sidecar_for(mask_path):
    p = mask_path.replace(".pgm", ".json")
    if not os.path.exists(p):
        return None
    try:
        with open(p) as f:
            return json.load(f)
    except Exception:
        return None


def compose(frame_path, mask_path, index, scale=0.5):
    base = Image.open(frame_path).convert("RGB")
    mask = read_pgm(mask_path)
    h, w = mask.shape
    if base.size != (w, h):
        base = base.resize((w, h))

    pal = palette()
    rgb = pal[mask]
    alpha = (np.where(mask > 0, 1.0, 0.0) * 0.55)[..., None]
    arr = np.asarray(base).astype(np.float32)
    blended = (arr * (1.0 - alpha) + rgb.astype(np.float32) * alpha).astype(np.uint8)

    left = base
    right = Image.fromarray(blended, "RGB")

    out = Image.new("RGB", (w * 2, h))
    out.paste(left, (0, 0))
    out.paste(right, (w, 0))

    sc = sidecar_for(mask_path)
    n_objects = len(sc["bindings"]) if sc else int((np.unique(mask) != 0).sum())
    labelled = 100.0 * float((mask > 0).sum()) / mask.size

    d = ImageDraw.Draw(out)
    d.text((20, 20), "frame %d  -  rendered" % index, fill=(255, 255, 255))
    d.text((w + 20, 20),
           "frame %d  -  %d objects, %.1f%% labelled" % (index, n_objects, labelled),
           fill=(255, 255, 255))

    if scale != 1.0:
        # Image.LANCZOS moved to Image.Resampling.LANCZOS in Pillow 10.
        resample = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
        out = out.resize((int(w * 2 * scale), int(h * scale)), resample)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/bin")
    ap.add_argument("--out", default="docs/evidence/demo.mp4")
    ap.add_argument("--fps", type=int, default=8)
    ap.add_argument("--scale", type=float, default=0.5)
    args = ap.parse_args()

    pairs = collect(args.dir)
    if not pairs:
        raise SystemExit("no paired mask+frame captures found in %s" % args.dir)

    staging = os.path.join(args.dir, "_demo_frames")
    os.makedirs(staging, exist_ok=True)
    for n, (idx, fpath, mpath) in enumerate(pairs):
        img = compose(fpath, mpath, idx, args.scale)
        img.save(os.path.join(staging, "%05d.png" % n))
        print("  composed frame %d (%d/%d)" % (idx, n + 1, len(pairs)))

    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        raise SystemExit("no working ffmpeg found")
    print("\nffmpeg: %s" % ffmpeg)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    cmd = [
        ffmpeg, "-y", "-framerate", str(args.fps),
        "-i", os.path.join(staging, "%05d.png"),
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
        args.out,
    ]
    print("\n" + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-2000:])
        raise SystemExit("ffmpeg failed")
    print("\nwrote %s (%d frames @ %d fps)" % (args.out, len(pairs), args.fps))


main()
