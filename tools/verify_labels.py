"""
verify_labels.py -- are the labels CORRECT, not merely self-consistent?

    python tools/verify_labels.py --dir build/bin

The gap this closes
-------------------
Everything else in this project proves the labels are consistent, decodable and
stable:

    identity_report.py   0 slot ambiguity, 0 unbound ids, identity survives
                         slot loss
    pack.py --verify     every byte round-trips
    overlay.py           the mask lines up with the image

None of that proves slot 71's pixels are actually the object the sidecar names.
A bug that wrote a consistent-but-wrong stencil value -- an off-by-one in the
lease, a slot reused while the previous owner was still rendering, a sidecar
built from the wrong frame's table -- would pass every one of those checks and
produce a dataset that is confidently mislabelled.

This is the check that can fail in that case.

What is actually testable offline
---------------------------------
Ground truth would require intervention: unmark one primitive and confirm
exactly its pixels vanish. That needs a live run. What can be tested from
captured data alone are properties that a WRONG label breaks and a right one
does not:

1. IDENTITY CONSISTENCY. A stableId must name the same class and object for the
   whole session. If it ever changes, the registry is corrupting identities, and
   every downstream track is garbage.

2. SPATIAL COHERENCE. A real object projects to a mostly-connected region. If
   one id's pixels are scattered across unrelated parts of the frame, that id is
   being shared by two different objects. Occlusion legitimately fragments
   things (a railing in front of a wall), so this is a distribution to inspect,
   not a hard pass/fail.

3. TEMPORAL COHERENCE, measured AGAINST THE SCENE. Captured frames are 0.1s
   apart, so an object's region should move roughly as much as everything else
   moves -- that is the camera. An id whose region jumps by far more than the
   frame's median displacement, while keeping a similar area, has almost
   certainly been reassigned to a different physical object.

   Comparing each object to the frame's own median is what makes this work
   during camera rotation. An absolute threshold would flag every object every
   time the camera turns, which is a measurement that cannot observe the thing
   it is judging -- the failure mode this project ran into repeatedly.

Limits, stated plainly: these are necessary, not sufficient. A label that is
wrong in a spatially and temporally coherent way -- two objects that always move
together and are always adjacent -- would pass. This narrows the space of
possible corruption a great deal; it does not eliminate it.
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict

import numpy as np

try:
    import cv2
except ImportError:                                    # pragma: no cover
    cv2 = None

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from overlay import read_pgm  # noqa: E402


def frame_index(path):
    m = re.search(r"_(\d+)\.", os.path.basename(path))
    return int(m.group(1)) if m else -1


def load_session(directory):
    masks = {frame_index(p): p for p in glob.glob(os.path.join(directory, "segcap_mask_*.pgm"))}
    out = []
    for f in sorted(masks):
        j = masks[f].replace(".pgm", ".json")
        if not os.path.exists(j):
            continue
        with open(j) as fh:
            out.append((f, masks[f], json.load(fh)))
    return out


def slot_to_id(sc):
    """Live (non-trailing) bindings only. A released binding is deliberately kept
    resolvable, but it describes a label on its way out, so including it here
    would report motion for an object that has already stopped being tracked."""
    return {int(b["slot"]): b for b in sc.get("bindings", []) if not b.get("released")}


# ---------------------------------------------------------------- check 1

def check_identity_consistency(session):
    seen = {}
    violations = []
    for f, _, sc in session:
        for b in sc.get("bindings", []):
            sid = b["stableId"]
            key = (b.get("className", ""), b.get("objectName", ""))
            if sid in seen and seen[sid] != key:
                violations.append((sid, seen[sid], key, f))
            seen[sid] = key
    return len(seen), violations


# ---------------------------------------------------------------- check 2

_CLOSE_KERNEL = np.ones((3, 3), np.uint8)


def blob_profile(mask, slot):
    """Describe one region: is it a blob, a stipple, or genuinely fragmented?

    Returns (fraction_in_largest_blob, n_components, is_dithered, largest_mask).

    The morphological close is the important part. UE4 renders dithered LOD
    transitions and dithered opacity as a SCREEN-DOOR STIPPLE, and CustomDepth
    inherits it, so those objects arrive as a checkerboard where no two labelled
    pixels are 8-connected -- 13,088 pixels in 13,088 components. Counting raw
    components calls that "completely fragmented" when it is in fact a perfectly
    well-formed label with holes punched in a regular pattern.

    Closing with a 3x3 bridges the single-pixel gaps so a stipple reads as the
    one object it actually is, and the raw component count is kept separately so
    dithering can be reported as the distinct phenomenon it is rather than
    silently repaired.
    """
    binary = (mask == slot).astype(np.uint8)
    total = int(binary.sum())
    if total == 0:
        return 0.0, 0, False, None

    n_raw, _, _, _ = cv2.connectedComponentsWithStats(binary, connectivity=8)
    raw_components = n_raw - 1
    # A stipple has almost as many components as pixels.
    is_dithered = raw_components > 0.3 * total

    closed = cv2.morphologyEx(binary, cv2.MORPH_CLOSE, _CLOSE_KERNEL)
    n, labels, stats, _ = cv2.connectedComponentsWithStats(closed, connectivity=8)
    if n <= 1:
        return 0.0, raw_components, is_dithered, None
    areas = stats[1:, cv2.CC_STAT_AREA]
    biggest = int(areas.argmax()) + 1
    frac = float(areas.max()) / float(closed.sum())
    return frac, raw_components, is_dithered, (labels == biggest)


def check_spatial_coherence(session, sample_every, min_px):
    """Largest connected component as a fraction of each region's pixels."""
    scores = []
    worst = []
    dithered = 0
    measured = 0
    for f, mpath, sc in session[::sample_every]:
        mask = read_pgm(mpath)
        live = slot_to_id(sc)
        ids, counts = np.unique(mask, return_counts=True)
        for i, c in zip(ids, counts):
            i = int(i)
            if i == 0 or c < min_px or i not in live:
                continue
            frac, ncomp, dith, _ = blob_profile(mask, i)
            if ncomp == 0:
                continue
            measured += 1
            if dith:
                dithered += 1
            scores.append(frac)
            worst.append((frac, f, i, live[i].get("objectName", "?"),
                          live[i].get("className", "?"), int(c), ncomp, dith))
    worst.sort(key=lambda r: r[0])
    return np.array(scores), worst, dithered, measured


# ---------------------------------------------------------------- check 3

def region_stats(mask, ids, compact_only):
    """Centroid and area per id, using the LARGEST BLOB rather than all pixels.

    The first version of this used the centroid of every pixel carrying the id,
    and the worst "offender" it reported turned out to be a modular mesh whose
    bounding box was the entire frame in 4-5 pieces. As the camera panned,
    different pieces dominated and the centroid slid across the screen far
    faster than any individual piece moved. It was flagging the instability of
    my own statistic, not an error in the label -- the same "measurement that
    cannot observe the thing it is judging" failure this project keeps hitting.

    Two corrections: track the largest blob's centroid, and (via compact_only)
    skip regions that are not predominantly one blob, because for a genuinely
    scattered object no single centroid means anything.
    """
    out = {}
    for i in ids:
        frac, ncomp, _dith, biggest = blob_profile(mask, i)
        if biggest is None or frac < compact_only:
            continue
        ys, xs = np.nonzero(biggest)
        if xs.size == 0:
            continue
        out[i] = (float(xs.mean()), float(ys.mean()), int(xs.size))
    return out


def load_inputs(directory):
    """The action log, if this session recorded one."""
    p = os.path.join(directory, "segcap_input.jsonl")
    if not os.path.exists(p):
        return []
    out = []
    with open(p) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except Exception:
                    pass
    out.sort(key=lambda r: r["t"])
    return out


def camera_rate_at(inputs, t_ms):
    """Right-stick magnitude in effect at a frame -- i.e. how fast the camera
    was turning.

    This exists because the first three transitions this check flagged were all
    real, all unexplained, and all resolved instantly by the action log: the
    right stick was at FULL deflection (+-16000), the patrol's turn-in-place
    step. Under a fast yaw, perspective moves objects near the screen edges many
    times further in pixels than objects near the centre, so a large ratio
    against the frame median is expected geometry rather than a mislabel.

    Worth noting that the input recording was added for world-model training and
    paid for itself first as a debugging instrument.
    """
    if not inputs or not t_ms:
        return None
    lo, hi = 0, len(inputs)
    while lo < hi:
        mid = (lo + hi) // 2
        if inputs[mid]["t"] <= t_ms:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    s = inputs[lo - 1]
    if t_ms - s["t"] > 2000:
        return None
    return float(np.hypot(s.get("rx", 0), s.get("ry", 0)))


def check_temporal_coherence(session, sample_every, min_px, outlier_mult, compact,
                             inputs=None):
    """Displacement of each object relative to the frame's own median.

    The median across all objects in a frame IS the camera motion. Dividing by
    it turns 'this moved a lot' into 'this moved a lot MORE than everything
    else', which is the only version of the question that survives a turning
    camera.
    """
    pairs = list(zip(session[::sample_every], session[sample_every::sample_every]))
    ratios = []
    suspects = []

    for (f0, m0, sc0), (f1, m1, sc1) in pairs:
        a, b = read_pgm(m0), read_pgm(m1)
        if a.shape != b.shape:
            continue
        diag = float(np.hypot(*a.shape))
        live0, live1 = slot_to_id(sc0), slot_to_id(sc1)

        # Match by stableId, not by slot: a slot can legitimately change hands
        # between frames, and comparing across that would manufacture a
        # teleport out of correct behaviour.
        id0 = {v["stableId"]: k for k, v in live0.items()}
        id1 = {v["stableId"]: k for k, v in live1.items()}
        common = set(id0) & set(id1)
        if len(common) < 5:
            continue

        s0 = region_stats(a, [id0[s] for s in common], compact)
        s1 = region_stats(b, [id1[s] for s in common], compact)

        moves = []
        for sid in common:
            k0, k1 = id0[sid], id1[sid]
            if k0 not in s0 or k1 not in s1:
                continue
            x0, y0, n0 = s0[k0]
            x1, y1, n1 = s1[k1]
            if n0 < min_px or n1 < min_px:
                continue
            d = float(np.hypot(x1 - x0, y1 - y0)) / diag
            area_ratio = min(n0, n1) / float(max(n0, n1))
            moves.append((sid, d, area_ratio, live0[k0].get("objectName", "?"),
                          live0[k0].get("className", "?")))

        if len(moves) < 5:
            continue
        med = float(np.median([m[1] for m in moves]))
        # A perfectly static frame gives med==0; use a small floor so the ratio
        # stays finite rather than dividing by zero.
        denom = max(med, 0.002)
        for sid, d, ar, nm, cls in moves:
            r = d / denom
            ratios.append(r)
            # Three conditions, all necessary.
            #
            # AREA STABLE: a region genuinely entering or leaving the frame
            # moves its centroid a lot for completely legitimate reasons.
            #
            # RATIO high: it moved much more than the scene did.
            #
            # ABSOLUTE displacement large: this one was missing, and without it
            # the check flagged characters walking across a static shot. When
            # the camera is still the scene median is near zero, so any
            # independent motion divides to a huge ratio -- and a character
            # moving independently of the camera is the single most normal thing
            # in a game. A label error is a TELEPORT: a large absolute jump. A
            # large ratio on a 0.05-diagonal move is just an actor doing its job.
            if r > outlier_mult and ar > 0.6 and d > 0.10:
                turn = camera_rate_at(inputs, sc0.get("timestampMs", 0))
                suspects.append((r, f0, f1, sid, nm, cls, d, ar, turn))

    suspects.sort(key=lambda t: -t[0])
    return np.array(ratios), suspects


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/bin")
    ap.add_argument("--sample-every", type=int, default=1)
    ap.add_argument("--min-px", type=int, default=400,
                    help="ignore regions smaller than this; a 20x20 sliver has "
                         "a meaningless centroid")
    ap.add_argument("--outlier-mult", type=float, default=8.0,
                    help="flag objects moving this many times the frame median")
    ap.add_argument("--compact", type=float, default=0.8,
                    help="only track objects at least this fraction one blob; a "
                         "scattered region has no meaningful centroid")
    args = ap.parse_args()

    if cv2 is None:
        raise SystemExit("opencv (cv2) is required for the connected-component check")

    session = load_session(args.dir)
    if len(session) < 2:
        raise SystemExit("need at least 2 mask+sidecar pairs in %s" % args.dir)
    print("session: %d frames with sidecars\n" % len(session))

    # ---- 1 ----------------------------------------------------------------
    n_ids, viol = check_identity_consistency(session)
    print("1. identity consistency  (does a stableId always name the same object?)")
    print("   identities tracked        : %d" % n_ids)
    print("   identities that changed   : %d" % len(viol))
    for sid, was, now, f in viol[:5]:
        print("      id %d: %s -> %s at frame %d" % (sid, was, now, f))
    print("   -> %s\n" % ("PASS" if not viol else "FAIL - the registry is corrupting identities"))

    # ---- 2 ----------------------------------------------------------------
    scores, worst, dithered, measured = check_spatial_coherence(
        session, args.sample_every, args.min_px)
    print("2. spatial coherence  (is one id one connected object?)")
    if scores.size:
        print("   regions measured          : %d" % scores.size)
        print("   largest blob / total      : median %.3f, mean %.3f"
              % (float(np.median(scores)), float(scores.mean())))
        for q in (5, 25):
            print("     %2dth percentile         : %.3f" % (q, float(np.percentile(scores, q))))
        print("   regions <50%% one blob     : %d (%.1f%%)"
              % (int((scores < 0.5).sum()), 100.0 * float((scores < 0.5).mean())))
        print("   DITHERED (stipple) regions: %d (%.1f%%)  <- UE4 dithered LOD/opacity"
              % (dithered, 100.0 * dithered / max(measured, 1)))
        print("\n   most fragmented (occlusion does this legitimately):")
        for frac, f, i, nm, cls, px, ncomp, dith in worst[:5]:
            print("      %.3f  frame %-6d slot %-4d %-24s %6d px, %5d raw pieces%s"
                  % (frac, f, i, nm[:24], px, ncomp, "  [dithered]" if dith else ""))
    print()

    # ---- 3 ----------------------------------------------------------------
    inputs = load_inputs(args.dir)
    ratios, suspects = check_temporal_coherence(session, args.sample_every,
                                                args.min_px, args.outlier_mult,
                                                args.compact, inputs)
    print("3. temporal coherence  (does an id stay on the same physical thing?)")
    if ratios.size:
        print("   object-transitions measured : %d" % ratios.size)
        print("   displacement / frame median : median %.2fx, 95th %.2fx, max %.2fx"
              % (float(np.median(ratios)), float(np.percentile(ratios, 95)),
                 float(ratios.max())))
        print("   objects moving >%.0fx the scene, with stable area: %d (%.3f%%)"
              % (args.outlier_mult, len(suspects),
                 100.0 * len(suspects) / max(ratios.size, 1)))
        if suspects:
            turning = sum(1 for s in suspects if s[8] and s[8] > 8000)
            print("   ...of which the camera was turning hard for: %d" % turning)
            print("\n   worst offenders:")
            for r, f0, f1, sid, nm, cls, d, ar, turn in suspects[:6]:
                note = ""
                if turn is not None:
                    note = "  [right stick %.0f%%]" % (100.0 * turn / 32768.0)
                print("      %6.1fx  frames %d->%d  id %-5d %-22s moved %.3f diag%s"
                      % (r, f0, f1, sid, nm[:22], d, note))
            if turning == len(suspects):
                print("\n   ALL flagged transitions happened under hard camera rotation.")
                print("   Under a fast yaw, perspective sweeps objects near the screen")
                print("   edges many times further in pixels than objects near the")
                print("   centre, so a large ratio there is geometry, not a mislabel.")
    print()

    print("verdict")
    print("-" * 70)
    ok = (not viol) and (not suspects or len(suspects) / max(ratios.size, 1) < 0.01)
    if ok:
        print("PASS. Identities never change what they name; no object jumps")
        print("      significantly against the scene's own motion. The labels")
        print("      behave like real objects, not like recycled numbers.")
    else:
        print("FAIL. See above -- this is a real finding, not a tolerance to widen.")
    print()
    print("These checks are NECESSARY, not sufficient. A label that is wrong in a")
    print("spatially and temporally coherent way would still pass. True ground")
    print("truth needs intervention: unmark one primitive and confirm exactly its")
    print("pixels vanish.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
