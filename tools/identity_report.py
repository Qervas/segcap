"""
identity_report.py -- does the 8-bit channel actually carry stable identity?

    python tools/identity_report.py --dir build/bin

The design claim under test
---------------------------
The stencil channel is 8 bits: 255 usable slots against ~33,000 markable
primitives in a Stray level. So a slot cannot be an identity. The design splits
them:

    slot      uint8, 1..255. A LEASE. Recycled aggressively. Meaningless alone.
    stableId  uint64, keyed on (pointer, serialNumber). The actual identity.

Three things have to be true for that to be more than a nice story, and all
three are checkable from the captured sidecars alone:

  1. NO AMBIGUITY WITHIN A FRAME. One slot must never name two objects in the
     same frame, or that frame's mask is unreadable at those pixels.

  2. SLOTS ACTUALLY GET REUSED. If they never did, the whole mechanism would be
     untested by the data and the 8-bit problem would be hypothetical.

  3. IDENTITY SURVIVES SLOT LOSS. An object that goes off screen, loses its
     slot, and comes back must resume its ORIGINAL stableId under whatever slot
     it is given. If it were issued a new identity instead, every occlusion
     would fragment one object into several tracks -- which is the specific
     failure that makes segmentation data useless for anything temporal.

Point 3 is the one worth caring about, and it is only observable across frames,
which is why this reads a whole session rather than a single sidecar.

The report also cross-checks the MASKS against the sidecars: an id present in
the pixels but absent from that frame's table is a labelled region nobody can
decode. That number should be ~0 and is reported honestly if it is not.
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from overlay import read_pgm  # noqa: E402


def frame_index(path):
    m = re.search(r"_(\d+)\.", os.path.basename(path))
    return int(m.group(1)) if m else -1


def load_sidecars(directory):
    out = []
    for p in sorted(glob.glob(os.path.join(directory, "segcap_mask_*.json")),
                    key=frame_index):
        with open(p) as f:
            sc = json.load(f)
        out.append((frame_index(p), sc, p))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/bin")
    ap.add_argument("--check-masks", action="store_true", default=True,
                    help="also verify every mask id appears in its sidecar")
    args = ap.parse_args()

    sidecars = load_sidecars(args.dir)
    if not sidecars:
        raise SystemExit("no sidecars found in %s" % args.dir)

    frames = [f for f, _, _ in sidecars]
    print("session: %d frames, %d .. %d" % (len(frames), frames[0], frames[-1]))
    print()

    # ---- 1. ambiguity within a frame ------------------------------------
    ambiguous = []
    for f, sc, path in sidecars:
        seen = {}
        for b in sc.get("bindings", []):
            s = b["slot"]
            if s in seen and seen[s] != b["stableId"]:
                ambiguous.append((f, s, seen[s], b["stableId"]))
            seen[s] = b["stableId"]

    # ---- 2/3. slot reuse and identity persistence ------------------------
    slot_occupants = defaultdict(set)     # slot -> {stableId}
    id_slots = defaultdict(set)           # stableId -> {slot}
    id_frames = defaultdict(list)         # stableId -> [frame]
    id_name = {}

    for f, sc, _ in sidecars:
        for b in sc.get("bindings", []):
            sid = b["stableId"]
            slot_occupants[b["slot"]].add(sid)
            id_slots[sid].add(b["slot"])
            id_frames[sid].append(f)
            id_name[sid] = (b.get("objectName", "?"), b.get("className", "?"))

    reused_slots = {s: ids for s, ids in slot_occupants.items() if len(ids) > 1}

    # An identity that was absent for a stretch and then came back. This is the
    # observable signature of "lost its slot, got it back" -- and it is only
    # meaningful because the id is the SAME on both sides of the gap.
    gapped = []
    for sid, fs in id_frames.items():
        positions = [frames.index(f) for f in fs]
        gaps = sum(1 for a, b in zip(positions, positions[1:]) if b - a > 1)
        if gaps:
            gapped.append((sid, gaps, len(fs), len(id_slots[sid])))
    gapped.sort(key=lambda r: -r[1])

    # Identities that held MORE THAN ONE slot over the session: the strongest
    # evidence, because the object was labelled with different pixel values at
    # different times and the sidecar still resolves both to one object.
    multi_slot = [(sid, sorted(id_slots[sid])) for sid in id_slots if len(id_slots[sid]) > 1]

    print("1. ambiguity within a frame")
    print("   one slot naming two objects in the same frame: %d" % len(ambiguous))
    print("   -> %s" % ("PASS - every frame's mask is unambiguously decodable"
                        if not ambiguous else "FAIL"))
    for row in ambiguous[:5]:
        print("      frame %d slot %d: id %d and id %d" % row)
    print()

    print("2. slot reuse (is the 8-bit constraint actually being exercised?)")
    print("   distinct identities seen : %d" % len(id_slots))
    print("   distinct slots used      : %d of 255" % len(slot_occupants))
    print("   slots that carried >1 identity over the session: %d" % len(reused_slots))
    if reused_slots:
        worst = max(reused_slots.items(), key=lambda kv: len(kv[1]))
        print("   busiest slot: %d carried %d different objects" % (worst[0], len(worst[1])))
    print("   -> %s" % ("recycling is exercised, so point 3 is a real test"
                        if reused_slots else "slots never recycled in this session"))
    print()

    print("3. identity survives slot loss")
    print("   identities that disappeared and came back: %d" % len(gapped))
    print("   identities that held more than one slot   : %d" % len(multi_slot))
    if multi_slot:
        print()
        print("   examples -- same object, different pixel value at different times:")
        for sid, slots in multi_slot[:6]:
            name, cls = id_name[sid]
            print("      id %-5d slots %-18s %s (%s)"
                  % (sid, ",".join(str(s) for s in slots), name, cls))
    print()

    # ---- 4. mask ids vs sidecar -----------------------------------------
    if args.check_masks:
        total_ids = 0
        unbound_ids = 0
        unbound_px = 0
        total_px = 0
        checked = 0
        for f, sc, _ in sidecars:
            mpath = os.path.join(args.dir, "segcap_mask_%d.pgm" % f)
            if not os.path.exists(mpath):
                continue
            mask = read_pgm(mpath)
            bound = {b["slot"] for b in sc.get("bindings", [])}
            import numpy as np
            ids, counts = np.unique(mask, return_counts=True)
            for i, c in zip(ids, counts):
                if i == 0:
                    continue
                total_ids += 1
                total_px += int(c)
                if int(i) not in bound:
                    unbound_ids += 1
                    unbound_px += int(c)
            checked += 1

        print("4. do the pixels agree with the tables?")
        print("   masks checked          : %d" % checked)
        print("   distinct ids in pixels : %d" % total_ids)
        print("   ids with no binding    : %d (%.2f%% of ids, %.3f%% of labelled pixels)"
              % (unbound_ids,
                 100.0 * unbound_ids / max(total_ids, 1),
                 100.0 * unbound_px / max(total_px, 1)))
        print()
        print("   An unbound id is a labelled region nobody can decode. The known")
        print("   cause is render-proxy latency: a primitive unmarked on the game")
        print("   thread keeps writing its old stencil value for a frame or two")
        print("   while the render thread rebuilds its proxy. See DEBUGGING.md 7.8.")

    return 0 if not ambiguous else 1


if __name__ == "__main__":
    sys.exit(main())
