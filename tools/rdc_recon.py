"""
rdc_recon.py -- offline RenderDoc capture analysis for segmentation-capture recon.

Run:
    set RDC_PATH=C:\\path\\to\\capture.rdc
    qrenderdoc.exe --python rdc_recon.py

If RDC_PATH is unset, the newest .rdc under RDC_DIR (or %TEMP%\\RenderDoc) is used.
Report is written next to the capture as <capture>.recon.txt and echoed to stdout.

Answers, without a human clicking through the UI:
  1. Which graphics API did the capture use?
  2. How many depth-stencil textures exist, at what formats / dimensions / sample counts?
  3. Which are plausible UE CustomDepth targets?
  4. Where are the depth-stencil clears, and against which resource?
  5. Is stencil actually written -- stencilEnable, write masks, and varying refs?

Written defensively: the RenderDoc Python API varies across versions, so every
optional field is probed with getattr and failures degrade to a note in the report
rather than a traceback.
"""

import os
import sys
import glob

import renderdoc as rd

# ---------------------------------------------------------------- utilities

OUT = []


def emit(line=""):
    OUT.append(str(line))
    print(line)


def head(title):
    emit()
    emit("=" * 78)
    emit(title)
    emit("=" * 78)


def safe(obj, *path, default=None):
    """getattr down a dotted path, returning default on any miss."""
    cur = obj
    for p in path:
        cur = getattr(cur, p, None)
        if cur is None:
            return default
    return cur


def fmt_name(fmt):
    """Human-readable format name across API versions."""
    for attempt in (lambda: fmt.Name(), lambda: str(fmt.type), lambda: str(fmt)):
        try:
            return attempt()
        except Exception:
            continue
    return "<unknown>"


def resolve_capture_path():
    p = os.environ.get("RDC_PATH", "").strip('"')
    if p and os.path.isfile(p):
        return p
    d = os.environ.get("RDC_DIR") or os.path.join(
        os.environ.get("TEMP", "."), "RenderDoc"
    )
    hits = sorted(glob.glob(os.path.join(d, "*.rdc")), key=os.path.getmtime)
    if not hits:
        raise SystemExit(
            "No capture found. Set RDC_PATH to a .rdc file, or RDC_DIR to a folder "
            "containing one. Searched: %s" % d
        )
    return hits[-1]


# ---------------------------------------------------------------- analysis


def is_depth_stencil(tex):
    """True if the texture was created usable as a depth-stencil target."""
    flags = safe(tex, "creationFlags", default=0)
    try:
        return bool(flags & rd.TextureCategory.DepthTarget)
    except Exception:
        return "depth" in fmt_name(tex.format).lower()


def has_stencil_plane(tex):
    n = fmt_name(tex.format).upper()
    return "S8" in n or "G8" in n or "S8X24" in n


def flatten(actions, out=None):
    out = [] if out is None else out
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def analyze(path):
    head("CAPTURE")
    emit("file: %s" % path)

    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, "", None)
    if result != rd.ResultCode.Succeeded:
        raise SystemExit("could not open capture: %s" % str(result))

    if cap.LocalReplaySupport() != rd.ReplaySupport.Supported:
        raise SystemExit("local replay unsupported for this capture")

    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise SystemExit("could not init replay: %s" % str(result))

    try:
        api = safe(ctrl.GetAPIProperties(), "pipelineType", default="?")
        emit("API (pipeline type): %s" % str(api))
        emit("Q1 ANSWER -- API is %s" % str(api))

        # ---- Q2/Q3: depth-stencil textures ----------------------------------
        head("DEPTH-STENCIL TEXTURES")

        textures = ctrl.GetTextures()
        resources = {r.resourceId: r for r in ctrl.GetResources()}

        # Backbuffer resolution = the largest 2D colour target, used as the
        # yardstick for "full resolution" without hardcoding 1920x1080.
        colour_dims = [
            (t.width, t.height)
            for t in textures
            if not is_depth_stencil(t) and t.width > 0 and t.height > 0
        ]
        bb_w, bb_h = max(colour_dims, key=lambda wh: wh[0] * wh[1]) if colour_dims else (0, 0)
        emit("inferred backbuffer resolution: %dx%d" % (bb_w, bb_h))
        emit()

        ds = [t for t in textures if is_depth_stencil(t)]
        emit("total depth-stencil textures: %d" % len(ds))
        emit()
        emit(
            "%-22s %-28s %9s %7s %8s %s"
            % ("resourceId", "format", "dims", "samples", "stencil?", "debug name")
        )
        emit("-" * 100)

        candidates = []
        for t in sorted(ds, key=lambda x: -(x.width * x.height)):
            name = safe(resources.get(t.resourceId), "name", default="") or "<stripped>"
            stencil = has_stencil_plane(t)
            full_res = (t.width, t.height) == (bb_w, bb_h)
            emit(
                "%-22s %-28s %9s %7d %8s %s"
                % (
                    str(t.resourceId),
                    fmt_name(t.format),
                    "%dx%d" % (t.width, t.height),
                    safe(t, "msSamp", default=1),
                    "yes" if stencil else "NO",
                    name,
                )
            )
            if stencil and full_res:
                candidates.append(t)

        emit()
        emit("Q2 ANSWER -- %d depth-stencil textures; %d full-res WITH a stencil plane"
             % (len(ds), len(candidates)))
        if len(candidates) < 2:
            emit("  NOTE: fewer than 2 full-res stencil targets. If only one exists it is")
            emit("  almost certainly scene depth, meaning CustomDepth is NOT allocated.")
            emit("  -> contingency: force r.CustomDepth 3 via IConsoleManager at runtime.")

        # ---- Q4: depth-stencil clears ---------------------------------------
        head("DEPTH-STENCIL CLEARS")

        actions = flatten(ctrl.GetRootActions())
        emit("total actions in frame: %d" % len(actions))
        emit()

        clears = []
        for a in actions:
            flags = safe(a, "flags", default=0)
            try:
                is_clear = bool(flags & rd.ActionFlags.Clear)
            except Exception:
                is_clear = False
            if not is_clear:
                continue
            for rid in (safe(a, "outputs", default=[]) or []):
                if rid == rd.ResourceId.Null():
                    continue
                tex = next((t for t in ds if t.resourceId == rid), None)
                if tex is not None:
                    clears.append((a.eventId, rid, a.GetName(ctrl.GetStructuredFile())))

        if clears:
            emit("%-8s %-22s %s" % ("EID", "resourceId", "action"))
            emit("-" * 80)
            for eid, rid, nm in clears:
                emit("%-8d %-22s %s" % (eid, str(rid), nm))
        else:
            emit("no depth-stencil clears matched (may be cleared via a different path)")

        emit()
        emit("Q4 ANSWER -- %d depth-stencil clear(s). Distinct targets cleared: %d"
             % (len(clears), len({c[1] for c in clears})))

        # ---- Q5: is stencil actually written? -------------------------------
        head("STENCIL WRITE ACTIVITY")

        draws = []
        for a in actions:
            flags = safe(a, "flags", default=0)
            try:
                if flags & rd.ActionFlags.Drawcall:
                    draws.append(a)
            except Exception:
                pass

        emit("total drawcalls: %d" % len(draws))

        # Sample across the frame rather than every draw -- replaying thousands of
        # SetFrameEvent calls is slow and adds nothing.
        step = max(1, len(draws) // 250)
        sampled = draws[::step]
        emit("sampling %d draws (every %d) for OM stencil state" % (len(sampled), step))
        emit()

        per_target = {}   # depth target resourceId -> dict of observations
        errors = 0

        for a in sampled:
            try:
                ctrl.SetFrameEvent(a.eventId, True)
                pipe = ctrl.GetD3D12PipelineState()
                om = safe(pipe, "outputMerger")
                if om is None:
                    continue

                dt = safe(om, "depthTarget", "resource", default=None)
                if dt is None or dt == rd.ResourceId.Null():
                    continue

                dss = safe(om, "depthStencilState")
                rec = per_target.setdefault(
                    dt,
                    {"draws": 0, "stencil_enabled": 0, "refs": set(),
                     "write_masks": set(), "first_eid": a.eventId, "last_eid": a.eventId},
                )
                rec["draws"] += 1
                rec["last_eid"] = a.eventId

                if safe(dss, "stencilEnable", default=False):
                    rec["stencil_enabled"] += 1
                    for face in ("frontFace", "backFace"):
                        f = safe(dss, face)
                        if f is not None:
                            rec["refs"].add(safe(f, "reference", default=None))
                            rec["write_masks"].add(safe(f, "writeMask", default=None))
            except Exception:
                errors += 1

        if errors:
            emit("note: %d draws could not be inspected (state unavailable)" % errors)
            emit()

        stencil_writing_targets = 0
        for rid, rec in sorted(per_target.items(), key=lambda kv: -kv[1]["draws"]):
            tex = next((t for t in ds if t.resourceId == rid), None)
            refs = sorted(r for r in rec["refs"] if r is not None)
            masks = sorted(m for m in rec["write_masks"] if m is not None)
            writes = rec["stencil_enabled"] > 0 and any(m for m in masks)

            emit("depth target %s  (%s)" % (
                str(rid),
                fmt_name(tex.format) if tex else "?"))
            emit("    sampled draws      : %d" % rec["draws"])
            emit("    EID range          : %d .. %d" % (rec["first_eid"], rec["last_eid"]))
            emit("    stencilEnable draws: %d" % rec["stencil_enabled"])
            emit("    stencil write masks: %s" % (masks or "none"))
            emit("    stencil refs seen  : %s%s" % (
                refs[:16] or "none",
                " ...(%d distinct)" % len(refs) if len(refs) > 16 else ""))
            emit("    -> WRITES STENCIL  : %s" % ("YES" if writes else "no"))
            emit()
            if writes:
                stencil_writing_targets += 1

        emit("Q5 ANSWER -- %d depth target(s) observed writing stencil" % stencil_writing_targets)
        if stencil_writing_targets:
            emit("  Varying stencil refs across draws is the CustomStencil signature.")
            emit("  A target with few draws and many distinct refs is the CustomDepth pass;")
            emit("  scene depth typically has many draws and a constant (often 0) ref.")

        # ---- Q6: are debug names present? -----------------------------------
        head("DEBUG NAME AVAILABILITY")
        named = [r for r in resources.values()
                 if safe(r, "name", default="") and not str(r.name).startswith("Resource ")]
        emit("resources with non-default names: %d / %d" % (len(named), len(resources)))
        emit("Q6 ANSWER -- debug names are %s"
             % ("PRESENT (day 3 gets easier, but we will not depend on them)"
                if len(named) > len(resources) * 0.2
                else "STRIPPED, as expected for a Shipping build"))

    finally:
        ctrl.Shutdown()
        cap.Shutdown()


# ---------------------------------------------------------------- entry


def main():
    path = resolve_capture_path()
    try:
        analyze(path)
    finally:
        report = path + ".recon.txt"
        try:
            with open(report, "w", encoding="utf-8") as f:
                f.write("\n".join(OUT))
            print("\n[report written to %s]" % report)
        except Exception as e:
            print("could not write report: %s" % e)

    # Hard-exit so the RenderDoc GUI never opens and Qt teardown cannot hang.
    sys.stdout.flush()
    os._exit(0)


main()
