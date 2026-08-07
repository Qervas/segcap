"""
rdc_stencil_hunt.py -- determine whether ANY draw in the frame writes stencil,
and which depth targets are actually used.

Run:
    set RDC_PATH=...\\capture.rdc
    qrenderdoc.exe --python rdc_stencil_hunt.py

Two lessons are baked into this version.

1. SetFrameEvent(eid, force=True) replays the frame FROM THE START every call.
   Scanning 1550 draws that way replays ~1.2M draws -- quadratic, and it ran for
   20 minutes at 1 CPU-second per minute before being killed. Passing force=False
   lets RenderDoc replay incrementally when stepping forward, so a single
   ascending pass is linear.

2. GetUsage(resourceId) returns every event that touched a resource, and how,
   with no replay at all. Most of what the previous version was paying replay
   costs to discover was already available for free.

A wall-clock budget bounds the sampling pass, and whatever was covered is
reported honestly rather than silently truncated.
"""

import os
import glob
import time
from collections import OrderedDict

import renderdoc as rd

OUT = []
BUDGET_SECONDS = float(os.environ.get("RDC_BUDGET", "420"))


def emit(line=""):
    OUT.append(str(line))
    print(line)


def head(t):
    emit()
    emit("=" * 78)
    emit(t)
    emit("=" * 78)


def resolve_capture_path():
    p = os.environ.get("RDC_PATH", "").strip('"')
    if p and os.path.isfile(p):
        return p
    d = os.environ.get("RDC_DIR") or os.path.join(os.environ.get("TEMP", "."), "RenderDoc")
    hits = sorted(glob.glob(os.path.join(d, "*.rdc")), key=os.path.getmtime)
    if not hits:
        raise SystemExit("no capture found; set RDC_PATH")
    return hits[-1]


def flatten(actions, out=None):
    out = [] if out is None else out
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def usage_name(u):
    s = str(u)
    return s.split(".")[-1] if "." in s else s


def main():
    path = resolve_capture_path()
    emit("capture: %s" % path)
    emit("time budget for the replay pass: %.0fs" % BUDGET_SECONDS)

    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
        raise SystemExit("open failed")
    _, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)

    try:
        textures = {t.resourceId: t for t in ctrl.GetTextures()}
        actions = flatten(ctrl.GetRootActions())
        null = rd.ResourceId.Null()

        ds_targets = [t for t in textures.values()
                      if (t.creationFlags & rd.TextureCategory.DepthTarget)
                      and ("S8" in t.format.Name())]

        # ---- free census via GetUsage ---------------------------------------
        head("DEPTH TARGET USAGE (no replay -- straight from the capture)")
        for t in sorted(ds_targets, key=lambda x: -(x.width * x.height)):
            try:
                usages = ctrl.GetUsage(t.resourceId)
            except Exception as e:
                emit("%s: GetUsage failed: %r" % (t.resourceId, e))
                continue

            kinds = OrderedDict()
            for u in usages:
                kinds.setdefault(usage_name(u.usage), []).append(u.eventId)

            emit("%s  %dx%d  %s"
                 % (t.resourceId, t.width, t.height, t.format.Name()))
            if not usages:
                emit("    NEVER USED in this frame (allocated but idle)")
            for k, eids in kinds.items():
                emit("    %-28s %5d events   EIDs %d..%d"
                     % (k, len(eids), min(eids), max(eids)))
            emit()

        # ---- stencil state, linear pass with force=False ---------------------
        head("STENCIL STATE (linear scan, force=False)")

        draws = [a for a in actions if a.flags & rd.ActionFlags.Drawcall]
        draws.sort(key=lambda a: a.eventId)
        emit("drawcalls: %d" % len(draws))

        psos = OrderedDict()
        failures = 0
        inspected = 0
        started = time.time()
        budget_hit = False

        for a in draws:
            if time.time() - started > BUDGET_SECONDS:
                budget_hit = True
                break
            try:
                # force=False is the whole point: it permits incremental replay.
                ctrl.SetFrameEvent(a.eventId, False)
                st = ctrl.GetD3D12PipelineState()
                dss = st.outputMerger.depthStencilState
                pso = st.pipelineResourceId
                inspected += 1

                rec = psos.get(pso)
                if rec is None:
                    rec = {"draws": 0, "stencilEnable": bool(dss.stencilEnable),
                           "writeMasks": set(), "refs": set(), "targets": set(),
                           "first": a.eventId, "last": a.eventId}
                    psos[pso] = rec
                rec["draws"] += 1
                rec["last"] = a.eventId
                if a.depthOut != null:
                    rec["targets"].add(a.depthOut)
                if dss.stencilEnable:
                    for f in (dss.frontFace, dss.backFace):
                        rec["writeMasks"].add(f.writeMask)
                        rec["refs"].add(f.reference)
            except Exception as e:
                failures += 1
                if failures <= 3:
                    emit("  !! eid=%d failed: %r" % (a.eventId, e))

        elapsed = time.time() - started
        emit("inspected %d/%d draws in %.1fs (%.1f draws/s)"
             % (inspected, len(draws), elapsed, inspected / max(elapsed, 0.001)))
        if budget_hit:
            emit("BUDGET EXHAUSTED -- coverage is PARTIAL. Results below describe")
            emit("only the first %d draws; raise RDC_BUDGET to extend." % inspected)
        emit("unique PSOs seen: %d" % len(psos))
        emit()

        writers = [(p, r) for p, r in psos.items()
                   if r["stencilEnable"] and any(r["writeMasks"])]
        testers = [(p, r) for p, r in psos.items()
                   if r["stencilEnable"] and not any(r["writeMasks"])]

        if writers:
            emit("PSOs WRITING STENCIL: %d" % len(writers))
            for pso, r in sorted(writers, key=lambda kv: -kv[1]["draws"]):
                tg = ", ".join(str(t) for t in r["targets"])
                emit("  PSO %s  draws=%d  EIDs %d..%d" % (pso, r["draws"], r["first"], r["last"]))
                emit("      target=%s writeMask=%s refs=%s"
                     % (tg, sorted(r["writeMasks"]), sorted(r["refs"])))
        else:
            emit("NO PSO writes stencil%s."
                 % (" in the portion inspected" if budget_hit else " anywhere in this frame"))

        if testers:
            emit()
            emit("PSOs that TEST stencil without writing: %d" % len(testers))
            for pso, r in testers[:8]:
                emit("  PSO %s  draws=%d  EIDs %d..%d  refs=%s"
                     % (pso, r["draws"], r["first"], r["last"], sorted(r["refs"])))
            emit("  (something upstream populates that buffer -- worth following)")

        if failures:
            emit()
            emit("state read failures: %d" % failures)

    finally:
        ctrl.Shutdown()
        cap.Shutdown()
        rep = path + ".stencil.txt"
        try:
            with open(rep, "w", encoding="utf-8") as f:
                f.write("\n".join(OUT))
            print("\n[report -> %s]" % rep)
        except Exception as e:
            print("write failed: %s" % e)
    os._exit(0)


main()
