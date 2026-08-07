"""
rdc_stencil_hunt.py -- exhaustively determine whether ANY draw in the frame
writes stencil, and if so, which pass and with what reference values.

Run:
    set RDC_PATH=...\\capture.rdc
    qrenderdoc.exe --python rdc_stencil_hunt.py

Why a dedicated script: the recon sampled 12 of 1365 draws and reported
"writes stencil: no". Unreal's CustomDepth pass is often only a handful of
draws, so sparse sampling cannot answer this question -- absence of evidence
was being reported as evidence of absence.

Method: depth-stencil state in D3D12 is baked into the PSO, not set on the
command list. So every draw sharing a PSO shares its stencil configuration.
Walking every draw in ascending EID order (which keeps RenderDoc's replay
incremental rather than seeking) and grouping by pipelineResourceId gives an
exhaustive answer for the cost of one pass over the frame.

Output identifies, per PSO:
  - whether stencil writes are enabled and with what write mask
  - the stencil reference values used (the actual per-object IDs, if any)
  - which depth target it renders to, and the EID range
"""

import os
import glob
from collections import OrderedDict

import renderdoc as rd

OUT = []


def emit(line=""):
    OUT.append(str(line))
    print(line)


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


def main():
    path = resolve_capture_path()
    emit("capture: %s" % path)

    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
        raise SystemExit("open failed")
    _, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)

    try:
        textures = {t.resourceId: t for t in ctrl.GetTextures()}
        actions = flatten(ctrl.GetRootActions())
        null = rd.ResourceId.Null()

        draws = [a for a in actions if a.flags & rd.ActionFlags.Drawcall]
        draws.sort(key=lambda a: a.eventId)   # ascending keeps replay incremental
        emit("drawcalls to inspect: %d" % len(draws))
        emit()

        psos = OrderedDict()
        failures = 0

        for i, a in enumerate(draws):
            if i % 200 == 0:
                emit("  ... %d/%d" % (i, len(draws)))
            try:
                ctrl.SetFrameEvent(a.eventId, True)
                st = ctrl.GetD3D12PipelineState()
                pso = st.pipelineResourceId
                om = st.outputMerger
                dss = om.depthStencilState

                rec = psos.get(pso)
                if rec is None:
                    rec = {
                        "draws": 0,
                        "stencilEnable": bool(dss.stencilEnable),
                        "depthEnable": bool(dss.depthEnable),
                        "depthWrites": bool(dss.depthWrites),
                        "writeMasks": set(),
                        "refs": set(),
                        "targets": set(),
                        "first": a.eventId,
                        "last": a.eventId,
                        "ops": set(),
                    }
                    psos[pso] = rec

                rec["draws"] += 1
                rec["last"] = a.eventId
                if a.depthOut != null:
                    rec["targets"].add(a.depthOut)

                # Reference value is dynamic (OMSetStencilRef), so it must be
                # collected per draw even though the rest is baked into the PSO.
                if dss.stencilEnable:
                    for f in (dss.frontFace, dss.backFace):
                        rec["writeMasks"].add(f.writeMask)
                        rec["refs"].add(f.reference)
                        rec["ops"].add(str(f.passOperation))
            except Exception as e:
                failures += 1
                if failures <= 3:
                    emit("  !! eid=%d failed: %r" % (a.eventId, e))

        emit()
        emit("=" * 78)
        emit("PSOs WITH STENCIL WRITES ENABLED")
        emit("=" * 78)

        writers = [(p, r) for p, r in psos.items()
                   if r["stencilEnable"] and any(r["writeMasks"])]

        if not writers:
            emit("NONE. No PSO in this frame enables stencil writes.")
            emit()
            emit("This is now an exhaustive result, not a sample: every drawcall")
            emit("was inspected and grouped by PSO.")
            emit()
            emit("Consequence: Stray's CustomDepth pass is either disabled or")
            emit("depth-only (r.CustomDepth = 0 or 1, not 3). The stencil route")
            emit("requires forcing r.CustomDepth 3 through IConsoleManager at")
            emit("runtime, which moves the engine-introspection work earlier.")
        else:
            for pso, r in sorted(writers, key=lambda kv: -kv[1]["draws"]):
                tgts = ", ".join(
                    "%s(%s)" % (t, textures[t].format.Name() if t in textures else "?")
                    for t in r["targets"])
                emit("PSO %s" % pso)
                emit("    draws        : %d   EIDs %d..%d" % (r["draws"], r["first"], r["last"]))
                emit("    depth target : %s" % (tgts or "none"))
                emit("    writeMask    : %s" % sorted(r["writeMasks"]))
                emit("    references   : %s" % sorted(r["refs"]))
                emit("    pass op      : %s" % sorted(r["ops"]))
                emit()

        emit("=" * 78)
        emit("SUMMARY")
        emit("=" * 78)
        emit("unique PSOs in frame     : %d" % len(psos))
        emit("PSOs with stencilEnable  : %d" % sum(1 for r in psos.values() if r["stencilEnable"]))
        emit("PSOs writing stencil     : %d" % len(writers))
        emit("state read failures      : %d" % failures)

        # Stencil TEST without WRITE still matters: it means something upstream
        # populated the buffer, which would be a route to identity we did not
        # allocate ourselves.
        testers = [(p, r) for p, r in psos.items()
                   if r["stencilEnable"] and not any(r["writeMasks"])]
        if testers:
            emit()
            emit("NOTE: %d PSO(s) TEST stencil without writing it." % len(testers))
            emit("Something is populating that buffer -- worth following up.")
            for pso, r in testers[:5]:
                emit("    PSO %s  draws=%d  EIDs %d..%d  refs=%s"
                     % (pso, r["draws"], r["first"], r["last"], sorted(r["refs"])))

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
