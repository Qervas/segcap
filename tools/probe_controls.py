"""
probe_controls.py -- discover a game's control scheme by trying it.

    python tools/probe_controls.py --process inZOI-Win64-Shipping

Why this exists
---------------
Hardcoding a control scheme per title does not work and does not generalise. It
was wrong for inZOI twice in one session: once assuming Stray's menu flow on a
game that auto-resumes straight into a save, and once assuming a bare right
stick turns a camera that actually requires LB held. Both were guesses. Both
were answerable in about a minute by pressing one button and looking at the
screen.

So this presses one input at a time and measures what changed on screen. The
output is a table of input -> effect, derived from the game in front of it
rather than from an assumption about the genre.

Method, and the two things that make it honest
----------------------------------------------
For each candidate input:

    settle -> baseline shot -> apply input -> shot during -> release
           -> settle -> shot after

and three numbers come out:

    during   how much the frame changed while the input was held
    after    how much it stayed changed once released
    drift    how much the frame changes with NO input at all

DRIFT IS THE POINT. A living game is never still: inZOI has pedestrians,
foliage, day/night, and animated UI, so a naive "did the screen change" test
answers yes to every button including ones that do nothing. Drift is measured
between every probe and used as the noise floor, exactly like the TAA noise
floor in the A/B capture test. An input counts as doing something only if it
moves the screen substantially more than the game moves on its own.

The during/after split separates kinds of effect. A camera pan changes the frame
while held and stops when released. Opening a menu changes it and KEEPS it
changed. That distinction is what tells a movement control from a UI control
without anyone having to recognise the UI.

Safety: START and BACK are excluded by default. On most titles one of them is
pause and the other opens a system menu, and on inZOI something in that family
appears to have quit the game during an earlier blind sweep.
"""

import argparse
import json
import os
import time

import numpy as np
from PIL import ImageGrab

# The window rect is passed in rather than looked up here. This interpreter's
# _ctypes is broken (conda env with a missing dependency), and the caller --
# PowerShell -- already has GetWindowRect. Not worth fighting the environment
# for one number that the shell can hand over.


def grab(bbox, scale=4):
    """Downscaled greyscale frame.

    Downscaled hard on purpose: the question is "did a large region change",
    and full resolution mostly adds sensitivity to film grain, TAA jitter and
    single-pixel UI ticks -- noise this test then has to subtract back out.
    """
    img = ImageGrab.grab(bbox=bbox, all_screens=True).convert("L")
    w, h = img.size
    img = img.resize((max(1, w // scale), max(1, h // scale)))
    return np.asarray(img, dtype=np.int16)


def diff(a, b):
    """Mean absolute difference, and the fraction of pixels that moved a lot."""
    if a.shape != b.shape:
        return 999.0, 1.0
    d = np.abs(a - b)
    return float(d.mean()), float((d > 24).mean())


def foreground_pid():
    """PID owning the foreground window, via PowerShell (this interpreter's
    _ctypes is broken)."""
    import subprocess
    ps = (
        "Add-Type @'\n"
        "using System; using System.Runtime.InteropServices;\n"
        "public class FgQ { [DllImport(\"user32.dll\")] public static extern IntPtr GetForegroundWindow();\n"
        " [DllImport(\"user32.dll\")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);\n"
        " public static uint Pid(){ IntPtr f=GetForegroundWindow(); if(f==IntPtr.Zero) return 0;\n"
        "   uint p; GetWindowThreadProcessId(f, out p); return p; } }\n"
        "'@\n[FgQ]::Pid()"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True)
    try:
        return int(out.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return 0


def game_pid(process_name):
    import subprocess
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"$p=Get-Process -Name '{process_name}' -ErrorAction SilentlyContinue|"
         f"Select-Object -First 1; if($p){{$p.Id}}else{{0}}"],
        capture_output=True, text=True)
    try:
        return int(out.stdout.strip())
    except ValueError:
        return 0


class Pad:
    """Drives vpad's --serve mode through its command file."""

    def __init__(self, path):
        self.path = path
        self.ack = path + ".ack"
        self.seq = 0
        for p in (self.path, self.ack):
            if os.path.exists(p):
                os.remove(p)

    def send(self, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0, btn="", ms=700):
        self.seq += 1
        line = (f"seq={self.seq} lx={lx} ly={ly} rx={rx} ry={ry} "
                f"lt={lt} rt={rt} btn={btn or '-'} ms={ms}")
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            f.write(line + "\n")
        os.replace(tmp, self.path)      # atomic: the server never sees a half-line
        return self.seq

    def wait_ack(self, seq, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                with open(self.ack) as f:
                    if int(f.read().strip() or 0) >= seq:
                        return True
            except (FileNotFoundError, ValueError):
                pass
            time.sleep(0.05)
        return False

    def quit(self):
        self.seq = -1
        with open(self.path, "w") as f:
            f.write("seq=-1\n")


# (label, kwargs). Sticks are held long enough for movement to be visible.
def build_probes(include_dangerous):
    # Sticks are held for 2.2s, not 0.9s.
    #
    # A first pass at 0.9s could not separate a camera nudge from the scene's
    # own motion: inZOI's idle drift measured 7.7 mean-abs on a downscaled
    # frame, with individual samples ranging 1.7 to 12.4, and a short nudge sits
    # inside that. Holding longer lets displacement accumulate well past the
    # noise, which is the only way a threshold against a noisy floor can mean
    # anything.
    HOLD = 2200
    P = [
        ("L stick forward",      dict(ly=26000, ms=HOLD)),
        ("L stick back",         dict(ly=-26000, ms=HOLD)),
        ("L stick left",         dict(lx=-26000, ms=HOLD)),
        ("L stick right",        dict(lx=26000, ms=HOLD)),
        ("R stick right",        dict(rx=26000, ms=HOLD)),
        ("R stick left",         dict(rx=-26000, ms=HOLD)),
        ("R stick up",           dict(ry=26000, ms=HOLD)),
        ("LB + R stick right",   dict(rx=26000, btn="LB", ms=HOLD)),
        ("LB + R stick left",    dict(rx=-26000, btn="LB", ms=HOLD)),
        ("RB + L stick forward", dict(ly=26000, btn="RB", ms=HOLD)),
        ("B",                    dict(btn="B", ms=180)),
        ("X",                    dict(btn="X", ms=180)),
        ("Y",                    dict(btn="Y", ms=180)),
        ("LB",                   dict(btn="LB", ms=300)),
        ("RB",                   dict(btn="RB", ms=300)),
        ("LT",                   dict(lt=255, ms=400)),
        ("RT",                   dict(rt=255, ms=400)),
        ("D-pad up",             dict(btn="DU", ms=200)),
        ("D-pad down",           dict(btn="DD", ms=200)),
        ("D-pad left",           dict(btn="DL", ms=200)),
        ("D-pad right",          dict(btn="DR", ms=200)),
    ]
    if include_dangerous:
        # A is in here on evidence, not caution. On inZOI it opened an external
        # browser to the game's news page -- twice, reproducibly -- which stole
        # the foreground and invalidated every probe that followed. B could not
        # undo it because the game was no longer the thing receiving input.
        #
        # START and BACK are pause and system-menu on most titles.
        P += [("A", dict(btn="A", ms=180)),
              ("START", dict(btn="START", ms=200)),
              ("BACK", dict(btn="BACK", ms=200))]
    return P


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--process", default="inZOI-Win64-Shipping")
    ap.add_argument("--bbox", required=True,
                    help="left,top,right,bottom of the game window")
    ap.add_argument("--cmd", default="build/bin/vpad_cmd.txt")
    ap.add_argument("--out", default="build/bin/control_probe.json")
    ap.add_argument("--shots", default="build/bin/probe_shots")
    ap.add_argument("--settle", type=float, default=1.4)
    ap.add_argument("--include-dangerous", action="store_true",
                    help="also probe START and BACK (may pause or quit the game)")
    args = ap.parse_args()

    bbox = tuple(int(v) for v in args.bbox.split(","))
    if len(bbox) != 4 or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        raise SystemExit(f"bad --bbox {args.bbox}")
    print(f"window: {bbox[0]},{bbox[1]} .. {bbox[2]},{bbox[3]}")
    os.makedirs(args.shots, exist_ok=True)

    pad = Pad(args.cmd)
    probes = build_probes(args.include_dangerous)
    results = []
    drifts = []

    gpid = game_pid(args.process)
    if not gpid:
        raise SystemExit(f"{args.process} is not running")
    print(f"game pid {gpid}")

    for i, (label, kw) in enumerate(probes):
        # GUARD: is the game still the thing on screen?
        #
        # This screenshots a REGION, not a window, so anything covering that
        # region is captured instead and the numbers keep coming out as though
        # nothing is wrong. On the first run, pressing A opened an external
        # browser to the game's news page; the browser took the foreground and
        # every subsequent probe measured a static webpage, reporting
        # drift=0.00 and "no visible effect" for twelve inputs in a row.
        #
        # Twelve confident, entirely meaningless rows. Checking foreground
        # ownership before each probe is what makes the rest of the table mean
        # anything at all.
        fg = foreground_pid()
        if fg != gpid:
            print(f"  !! foreground is pid {fg}, not the game ({gpid}) -- "
                  f"stopping before '{label}'")
            print("     (something took focus; the remaining rows would be "
                  "measuring whatever is on top)")
            break

        # Drift: how much the frame moves with no input, measured right before
        # this probe so it tracks whatever the scene is doing now.
        d0 = grab(bbox)
        time.sleep(args.settle)
        d1 = grab(bbox)
        drift_mean, _ = diff(d0, d1)
        drifts.append(drift_mean)

        base = grab(bbox)
        seq = pad.send(**kw)
        if not pad.wait_ack(seq, timeout=10):
            print(f"  !! no ack for '{label}' -- is vpad --serve running?")
            break
        during = grab(bbox)
        time.sleep(args.settle)
        after = grab(bbox)

        dm, dp = diff(base, during)
        am, ap_ = diff(base, after)

        # RECOVER: if the change stuck, try to back out before the next probe.
        #
        # Without this, one input that opens a menu poisons every measurement
        # after it -- the game is no longer in the state the table claims to be
        # describing. B is the near-universal cancel; whether it worked is
        # recorded rather than assumed, and a failure to recover stops the run
        # instead of producing rows about a screen nobody understands.
        recovered = None
        if am > 2.5 * max(drift_mean, 0.01):
            for _ in range(3):
                s = pad.send(btn="B", ms=150)
                pad.wait_ack(s, timeout=6)
                time.sleep(args.settle)
                chk = grab(bbox)
                if diff(base, chk)[0] <= 2.5 * max(drift_mean, 0.01):
                    recovered = True
                    break
            else:
                recovered = False

        results.append(dict(label=label, input=kw, drift=drift_mean,
                            during_mean=dm, during_pct=dp,
                            after_mean=am, after_pct=ap_, recovered=recovered))
        # Keep the frames for anything that looked interesting, so a surprising
        # row in the table can be looked at rather than argued about.
        if max(dm, am) > 2.5 * max(drift_mean, 0.01):
            from PIL import Image
            slug = "".join(c if c.isalnum() else "_" for c in label)[:28]
            for tag, arr in (("base", base), ("during", during), ("after", after)):
                Image.fromarray(arr.astype(np.uint8)).save(
                    os.path.join(args.shots, f"{i:02d}_{slug}_{tag}.png"))

        print(f"  {label:22s} drift={drift_mean:6.2f}  during={dm:6.2f}  after={am:6.2f}")

    pad.quit()

    floor = float(np.median(drifts)) if drifts else 0.0
    # 2.5x the game's own motion. Lower flags foliage and pedestrians; higher
    # misses a slow camera pan.
    THRESH = 2.5

    print("\n" + "=" * 74)
    print(f"drift floor (game's own motion, no input): {floor:.2f}")
    print("=" * 74)
    print(f"{'input':24s} {'during':>8s} {'after':>8s}  effect")
    print("-" * 74)
    for r in sorted(results, key=lambda r: -r["during_mean"]):
        d_ratio = r["during_mean"] / max(floor, 0.01)
        a_ratio = r["after_mean"] / max(floor, 0.01)
        rec = r.get("recovered")
        if d_ratio < THRESH and a_ratio < THRESH:
            effect = "no visible effect"
        elif rec is True:
            # B undid it, so it put the game into a different MODE -- a menu,
            # a panel, a dialog. Movement cannot be undone by pressing cancel.
            effect = "opened something (B closed it)"
        elif rec is False:
            effect = "PERSISTENT, B did not undo it"
        elif d_ratio >= THRESH:
            effect = "changes the view while held (move/camera)"
        else:
            effect = "delayed change"
        print(f"{r['label']:24s} {d_ratio:7.1f}x {a_ratio:7.1f}x  {effect}")

    with open(args.out, "w") as f:
        json.dump(dict(process=args.process, drift_floor=floor,
                       threshold=THRESH, results=results), f, indent=2)
    print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
