"""
make_demo.py -- assemble captured frames and masks into the demo video.

    python tools/make_demo.py --dir build/bin --out demo.mp4 --fps 10

Side by side: the game's rendered frame on the left, the same frame with its
segmentation mask overlaid and LABELLED on the right. Both come from the same
Present call, so they are the same frame by construction -- there is no
synchronisation step here because there is nothing to synchronise.

Why the labels exist
--------------------
A coloured overlay proves the mask lines up with the image. It does NOT show
that the ids mean anything, and "the ids mean something" is the actual claim:
the stencil channel is 8 bits, so a slot number is a lease that only resolves to
an object through that frame's sidecar. Drawing the resolved names on the frame
is the difference between showing a pretty overlay and showing that the sidecar
round-trip works on every frame.

The tracked-object callout does the same job for identity across TIME. It
follows one stableId through the whole video, printing the slot it currently
holds. When that slot number changes while the name and id do not, the 8-bit
recycling problem and its solution are both visible in one line of text.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from overlay import palette, read_pgm  # noqa: E402
from mask_align import align_mask_to_frame  # noqa: E402


def find_ffmpeg():
    """Locate a WORKING ffmpeg.

    The one first on PATH here (a conda env build) exits with 0xC0000139,
    STATUS_ENTRYPOINT_NOT_FOUND -- a broken DLL dependency. It produces no
    output at all, not even -version, which looks like a codec problem until you
    check the exit code. So candidates are probed by actually running them
    rather than by existence.

    Search order comes from the environment rather than one machine's home
    directory: SEGCAP_FFMPEG if set, then scoop's versioned layout under the
    current user, then whatever is on PATH.
    """
    candidates = []
    if os.environ.get("SEGCAP_FFMPEG"):
        candidates.append(os.environ["SEGCAP_FFMPEG"])
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    for base in (os.path.join(home, "scoop", "apps", "ffmpeg"),):
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


def load_font(size):
    """A real TrueType font, not PIL's bitmap default.

    The default font is ~11px and does not scale, so on a 1280x720 frame it is
    unreadable and on a downscaled composite it disappears entirely. Labels that
    cannot be read are worse than no labels: they add clutter and imply the tool
    is showing something it is not.
    """
    # WINDIR rather than a literal C:\Windows -- Windows is not always on C:,
    # and the fallback keeps this working off-platform.
    font_dirs = (os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"),
                 "/usr/share/fonts/truetype/dejavu")
    for name in ("segoeuib.ttf", "seguisb.ttf", "arialbd.ttf", "segoeui.ttf", "arial.ttf"):
        for d in font_dirs:
            p = os.path.join(d, name)
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, size)
                except Exception:
                    pass
    return ImageFont.load_default()


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
        print("  %d mask(s) have no matching frame: %s" % (len(missing), missing[:8]))
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


# Tall enough for the stick dials PLUS their captions. At 76 the captions were
# clipped off the bottom edge, which looked like a rendering bug rather than a
# layout constant being two dozen pixels short.
INPUT_PANEL_H = 100

# XUSB button bits, in the order they are drawn.
BUTTONS = [
    (0x1000, "A"), (0x2000, "B"), (0x4000, "X"), (0x8000, "Y"),
    (0x0100, "LB"), (0x0200, "RB"),
    (0x0001, "DU"), (0x0002, "DD"), (0x0004, "DL"), (0x0008, "DR"),
    (0x0010, "ST"), (0x0020, "BK"),
]


def load_input_log(directory):
    """The actions that produced these frames, as written by vpad.exe.

    Returned sorted by timestamp so a frame can be matched with a binary search.
    """
    path = os.path.join(directory, "segcap_input.jsonl")
    if not os.path.exists(path):
        return []
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except Exception:
                continue          # a run killed mid-write leaves one partial line
    out.sort(key=lambda r: r["t"])
    return out


def action_at(samples, t_ms):
    """The input in effect at a frame's Present time.

    A step function, held from the last sample at or before the frame -- which
    is what a gamepad actually is. Interpolating between samples would invent
    stick positions the game never saw, and for a world model the action label
    has to be the action that was really delivered.

    Returns None if the frame falls outside the log entirely (before the pad
    started, or after it was released), so those frames can be shown as having
    no known action rather than silently attributed the nearest one.
    """
    if not samples or not t_ms:
        return None
    lo, hi = 0, len(samples)
    while lo < hi:
        mid = (lo + hi) // 2
        if samples[mid]["t"] <= t_ms:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    s = samples[lo - 1]
    # More than 2s stale means the pad was not reporting; do not pretend.
    if t_ms - s["t"] > 2000:
        return None
    return s


def draw_stick(d, cx, cy, r, x, y, label, font):
    """One analogue stick as a dial with a dot at its current position."""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(120, 122, 130), width=1)
    d.line([cx - r, cy, cx + r, cy], fill=(60, 62, 70))
    d.line([cx, cy - r, cx, cy + r], fill=(60, 62, 70))
    # Y is inverted: the pad reports up as positive, the screen grows downward.
    px = cx + (x / 32768.0) * r
    py = cy - (y / 32768.0) * r
    live = abs(x) > 3000 or abs(y) > 3000
    col = (120, 230, 140) if live else (150, 152, 160)
    if live:
        d.line([cx, cy, px, py], fill=col, width=2)
    d.ellipse([px - 4, py - 4, px + 4, py + 4], fill=col)
    d.text((cx - r, cy + r + 3), label, font=font, fill=(150, 152, 160))


def draw_input_panel(img, act, font, small):
    """The controller state, drawn under the frames.

    Shown because this capture is meant to feed world models, which learn
    P(next frame | frame, action). The mask half of the video shows what is in
    the scene; this shows what was being asked of it. Together they are the
    training pair.
    """
    d = ImageDraw.Draw(img)
    w, h = img.size
    top = h - INPUT_PANEL_H
    d.rectangle([0, top, w, h], fill=(24, 25, 29))
    d.line([0, top, w, top], fill=(60, 62, 70))

    if act is None:
        d.text((18, top + 36), "input: no controller sample for this frame",
               font=font, fill=(170, 130, 130))
        return

    r = 28
    draw_stick(d, 58, top + 40, r, act["lx"], act["ly"], "L stick", small)
    draw_stick(d, 164, top + 40, r, act["rx"], act["ry"], "R stick", small)

    # Triggers, one bar each, stacked.
    for row, (name, val) in enumerate((("LT", act.get("lt", 0)),
                                       ("RT", act.get("rt", 0)))):
        ty = top + 26 + row * 26
        d.text((236, ty - 3), name, font=small, fill=(150, 152, 160))
        d.rectangle([270, ty, 344, ty + 15], outline=(90, 92, 100))
        if val:
            d.rectangle([271, ty + 1, 271 + int(72 * val / 255.0), ty + 14],
                        fill=(120, 230, 140))

    # Buttons.
    bx = 376
    bits = int(act.get("buttons", 0))
    for bit, name in BUTTONS:
        on = bool(bits & bit)
        d.rounded_rectangle([bx, top + 32, bx + 34, top + 60], radius=6,
                            fill=(120, 230, 140) if on else (44, 46, 52),
                            outline=(90, 92, 100))
        tw = d.textlength(name, font=small)
        d.text((bx + 17 - tw / 2, top + 39), name, font=small,
               fill=(20, 22, 26) if on else (150, 152, 160))
        bx += 40

    d.text((bx + 24, top + 30), "action recorded with every frame",
           font=font, fill=(200, 202, 210))
    d.text((bx + 24, top + 52),
           "exact, not inferred - this pipeline synthesises the input",
           font=small, fill=(125, 127, 137))


def pretty(name, cls):
    """Object names in a shipping build are mostly boilerplate.

    'StaticMeshComponent0' and 'NODE_AddStaticMeshComponent-36' are what Unreal
    actually calls things, and a frame covered in those reads as noise. Where
    the name carries no information the class is shown instead, which at least
    distinguishes a skeletal mesh (a character) from static geometry.
    """
    n = re.sub(r"_GEN_VARIABLE$", "", name or "")
    n = re.sub(r"^NODE_Add", "", n)
    if re.fullmatch(r"(StaticMeshComponent|SkeletalMeshComponent|SceneComponent)\d*", n):
        n = cls.replace("Component", "")
    n = re.sub(r"Component-?\d*$", "", n)
    return n[:26] if n else (cls or "?")


def choose_tracked(pairs):
    """Pick one object to follow for the whole video.

    Prefers the cat, then any skeletal mesh (characters move, so the callout
    stays interesting), then whatever is present in the most frames. Returns
    (stableId, label) or None.
    """
    seen = {}
    for _, _, mpath in pairs:
        sc = sidecar_for(mpath)
        if not sc:
            continue
        for b in sc.get("bindings", []):
            sid = b.get("stableId")
            e = seen.setdefault(sid, {"n": 0, "name": b.get("objectName", ""),
                                      "cls": b.get("className", "")})
            e["n"] += 1

    if not seen:
        return None
    cats = [k for k, v in seen.items() if v["name"] == "CharacterMesh0"]
    if cats:
        best = max(cats, key=lambda k: seen[k]["n"])
        return best, "the cat"
    skel = [k for k, v in seen.items() if v["cls"] == "SkeletalMeshComponent"]
    pool = skel if skel else list(seen)
    best = max(pool, key=lambda k: seen[k]["n"])
    return best, pretty(seen[best]["name"], seen[best]["cls"])


def outlined(draw, xy, text, font, fill, halo=(0, 0, 0)):
    """Text with a dark halo. A segmentation overlay is a field of saturated
    colour, so plain text is illegible against roughly half of it whatever
    colour it is."""
    x, y = xy
    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, 1), (-1, 1), (1, -1)):
        draw.text((x + dx, y + dy), text, font=font, fill=halo)
    draw.text((x, y), text, font=font, fill=fill)


def label_regions(draw, mask, sc, pal, font, max_labels, min_frac):
    """Name the biggest regions, at their centroids, without overlapping.

    Only regions above min_frac of the frame are labelled. A 720p frame holds
    ~80 objects and labelling all of them produces an unreadable mess -- the
    point is to demonstrate that ids resolve to real objects, which a dozen
    legible labels do better than eighty illegible ones.
    """
    names = {}
    for b in (sc or {}).get("bindings", []):
        names[int(b["slot"])] = (pretty(b.get("objectName", ""), b.get("className", "")),
                                 int(b.get("stableId", 0)),
                                 bool(b.get("released", False)))

    ids, counts = np.unique(mask, return_counts=True)
    order = sorted(((int(i), int(c)) for i, c in zip(ids, counts) if i != 0),
                   key=lambda t: -t[1])

    placed = []
    h, w = mask.shape
    drawn = 0
    for sid, count in order:
        if drawn >= max_labels or count < min_frac * mask.size:
            break
        nm = names.get(sid)
        if not nm or nm[2]:          # skip trailing (released) labels
            continue
        ys, xs = np.nonzero(mask == sid)
        cx, cy = float(xs.mean()), float(ys.mean())

        text = "%s  #%d" % (nm[0], nm[1])
        tw = draw.textlength(text, font=font)
        th = font.size + 2
        x = min(max(cx - tw / 2, 4), w - tw - 4)
        y = min(max(cy - th / 2, 4), h - th - 4)

        # Greedy de-overlap: nudge down until it clears what is already placed.
        for _ in range(24):
            if not any(abs(x - px) < (tw + pw) / 2 and abs(y - py) < th + 2
                       for px, py, pw in placed):
                break
            y += th + 3
            if y > h - th - 4:
                break
        else:
            continue
        placed.append((x, y, tw))

        col = tuple(int(v) for v in pal[sid])
        draw.rectangle([x - 4, y - 2, x + tw + 3, y + th], fill=(0, 0, 0, 255))
        draw.rectangle([x - 4, y - 2, x - 2, y + th], fill=col)
        outlined(draw, (x, y - 1), text, font, col)
        drawn += 1
    return drawn


def compose(frame_path, mask_path, index, scale, font, hud_font, tracked, opts,
            inputs=None):
    base = Image.open(frame_path).convert("RGB")
    mask = read_pgm(mask_path)
    # Upscale the MASK to the frame, exactly, by integer replication -- never
    # shrink the frame to the mask, which is what this did and which quietly
    # threw away three quarters of the video on any title that renders its
    # scene below the present resolution (inZOI: 1280x800 scene, 2560x1600
    # present). See mask_align for why nearest-neighbour is mandatory, not a
    # preference. Must run before pal[mask] turns ids into colours.
    mask = align_mask_to_frame(mask, base.size)
    h, w = mask.shape

    pal = palette()
    rgb = pal[mask]
    alpha = (np.where(mask > 0, 1.0, 0.0) * opts.alpha)[..., None]
    arr = np.asarray(base).astype(np.float32)
    blended = (arr * (1.0 - alpha) + rgb.astype(np.float32) * alpha).astype(np.uint8)

    left = base
    right = Image.fromarray(blended, "RGB")

    sc = sidecar_for(mask_path)
    rd = ImageDraw.Draw(right)
    label_regions(rd, mask, sc, pal, font, opts.max_labels, opts.min_frac)

    bar = 34
    panel = INPUT_PANEL_H if inputs is not None else 0
    out = Image.new("RGB", (w * 2, h + bar + panel), (16, 16, 18))
    out.paste(left, (0, bar))
    out.paste(right, (w, bar))

    d = ImageDraw.Draw(out)
    n_objects = int((np.unique(mask) != 0).sum())
    labelled = 100.0 * float((mask > 0).sum()) / mask.size

    d.text((14, 9), "Stray  -  rendered frame %d" % index, font=hud_font, fill=(235, 235, 240))
    d.text((w + 14, 9),
           "segmentation  -  %d objects,  %.1f%% of pixels labelled" % (n_objects, labelled),
           font=hud_font, fill=(235, 235, 240))

    # The identity callout: same stableId every frame, and the slot it currently
    # holds. A changing slot with an unchanged id is the 8-bit lease working.
    if tracked and sc:
        sid, label = tracked
        cur = None
        for b in sc.get("bindings", []):
            if b.get("stableId") == sid and not b.get("released"):
                cur = b
                break
        if cur:
            txt = "tracking %s   stableId #%d   currently slot %d" % (label, sid, cur["slot"])
            tw = d.textlength(txt, font=hud_font)
            d.text((w * 2 - tw - 14, 9), txt, font=hud_font, fill=(255, 214, 102))

    if inputs is not None:
        draw_input_panel(out, action_at(inputs, (sc or {}).get("timestampMs", 0)),
                         hud_font, font)

    # Always land on EVEN dimensions. libx264 with yuv420p subsamples chroma
    # 2x2 and rejects an odd width or height outright -- a 0.62 scale of 2560
    # gives 1587, and ffmpeg fails with a message that says nothing about
    # parity. Rounding here rather than choosing scales that happen to work
    # means any --scale value is safe.
    tw = int(out.width * scale) & ~1
    th = int(out.height * scale) & ~1
    if (tw, th) != out.size:
        # Image.LANCZOS moved to Image.Resampling.LANCZOS in Pillow 10.
        resample = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
        out = out.resize((tw, th), resample)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/bin")
    ap.add_argument("--out", default="docs/evidence/demo.mp4")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--scale", type=float, default=0.6)
    ap.add_argument("--alpha", type=float, default=0.55)
    ap.add_argument("--max-labels", type=int, default=12)
    ap.add_argument("--min-frac", type=float, default=0.004,
                    help="skip regions smaller than this fraction of the frame")
    ap.add_argument("--font-size", type=int, default=17)
    ap.add_argument("--no-input", action="store_true",
                    help="omit the controller panel even if an input log exists")
    ap.add_argument("--hold", type=int, default=1,
                    help="repeat each composed frame N times; slows playback "
                         "without re-encoding at a fractional fps")
    args = ap.parse_args()

    pairs = collect(args.dir)
    if not pairs:
        raise SystemExit("no paired mask+frame captures found in %s" % args.dir)

    tracked = choose_tracked(pairs)
    if tracked:
        print("tracking stableId #%d (%s) across the session" % (tracked[0], tracked[1]))

    inputs = None if args.no_input else load_input_log(args.dir)
    if inputs:
        stamps = []
        for _, _, m in pairs:
            sc = sidecar_for(m)
            if sc:
                stamps.append(sc.get("timestampMs", 0))
        matched = sum(1 for t in stamps if action_at(inputs, t))
        print("input log: %d samples, %.1fs span; %d/%d frames matched to an action"
              % (len(inputs), (inputs[-1]["t"] - inputs[0]["t"]) / 1000.0,
                 matched, len(stamps)))
    elif inputs is not None:
        print("input log: none found (segcap_input.jsonl) -- no action panel")
        inputs = None

    font = load_font(args.font_size)
    hud_font = load_font(19)

    staging = os.path.join(args.dir, "_demo_frames")
    os.makedirs(staging, exist_ok=True)
    for f in glob.glob(os.path.join(staging, "*.png")):
        os.remove(f)

    n = 0
    for idx, fpath, mpath in pairs:
        img = compose(fpath, mpath, idx, args.scale, font, hud_font, tracked, args,
                      inputs)
        for _ in range(max(1, args.hold)):
            img.save(os.path.join(staging, "%05d.png" % n))
            n += 1
        if (n // max(1, args.hold)) % 20 == 0:
            print("  composed %d/%d" % (n // max(1, args.hold), len(pairs)))

    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        raise SystemExit("no working ffmpeg found")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    cmd = [
        ffmpeg, "-y", "-framerate", str(args.fps),
        "-i", os.path.join(staging, "%05d.png"),
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
        args.out,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        # Print the tail of stderr, not a bare "failed". ffmpeg's actual
        # complaint is usually specific and actionable, and swallowing it turns
        # a two-second fix into a guessing game.
        print(r.stderr[-2500:])
        raise SystemExit("ffmpeg failed")
    print("\nwrote %s (%d frames @ %d fps = %.1fs)"
          % (args.out, n, args.fps, n / float(args.fps)))


if __name__ == "__main__":
    main()
