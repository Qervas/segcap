"""
rdc_validate_state.py -- verify that pipeline-state reads actually track the
current event before trusting any conclusion drawn from them.

Run:
    set RDC_PATH=...\\capture.rdc
    qrenderdoc.exe --python rdc_validate_state.py

Motivation: a scan using SetFrameEvent(eid, force=False) reported "1 unique PSO"
across 1550 drawcalls and concluded no draw writes stencil. One PSO for an entire
frame is impossible, so the state was not refreshing and the conclusion was
worthless. That is the third time in this project a measurement silently failed
and produced a confident answer.

So: measure the instrument. Read the same small set of draws twice, once with
force=False and once with force=True, and compare. If they disagree, force=False
is unusable here and every result derived from it must be discarded.

Also dumps the CustomDepth candidate's clear parameters, which is what actually
decides whether the stencil route is viable.
"""

import os
import glob
import time

import renderdoc as rd

OUT = []


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


def sample_state(ctrl, eids, force):
    """Read pipeline state at each eid; return per-eid observations."""
    rows = []
    t0 = time.time()
    for eid in eids:
        ctrl.SetFrameEvent(eid, force)
        st = ctrl.GetD3D12PipelineState()
        dss = st.outputMerger.depthStencilState
        rows.append({
            "eid": eid,
            "pso": str(st.pipelineResourceId),
            "stencilEnable": bool(dss.stencilEnable),
            "depthEnable": bool(dss.depthEnable),
            "writeMask": dss.frontFace.writeMask,
            "ref": dss.frontFace.reference,
        })
    return rows, time.time() - t0


def main():
    path = resolve_capture_path()
    emit("capture: %s" % path)

    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
        raise SystemExit("open failed")
    _, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)

    try:
        actions = flatten(ctrl.GetRootActions())
        draws = sorted([a for a in actions if a.flags & rd.ActionFlags.Drawcall],
                       key=lambda a: a.eventId)

        # 24 draws spread across the frame: enough to prove or disprove that the
        # state tracks the event, cheap enough to afford force=True.
        step = max(1, len(draws) // 24)
        eids = [a.eventId for a in draws[::step]][:24]

        head("INSTRUMENT CHECK: force=False vs force=True on identical events")
        emit("sampling %d draws out of %d" % (len(eids), len(draws)))

        soft, t_soft = sample_state(ctrl, eids, False)
        hard, t_hard = sample_state(ctrl, eids, True)

        emit("force=False: %.1fs   force=True: %.1fs" % (t_soft, t_hard))
        emit("distinct PSOs  force=False: %d   force=True: %d"
             % (len({r["pso"] for r in soft}), len({r["pso"] for r in hard})))
        emit()

        emit("%-8s %-26s %-26s %s" % ("eid", "PSO (force=False)", "PSO (force=True)", "agree"))
        emit("-" * 78)
        disagreements = 0
        for a, b in zip(soft, hard):
            agree = a["pso"] == b["pso"] and a["stencilEnable"] == b["stencilEnable"]
            if not agree:
                disagreements += 1
            emit("%-8d %-26s %-26s %s" % (a["eid"], a["pso"], b["pso"], "yes" if agree else "NO"))

        emit()
        if disagreements:
            emit("VERDICT: force=False is UNRELIABLE here (%d/%d disagree)."
                 % (disagreements, len(eids)))
            emit("Every result produced with force=False must be discarded.")
        else:
            emit("VERDICT: the two agree. If both report 1 distinct PSO across the")
            emit("frame, the problem is pipelineResourceId itself, not the force flag.")

        head("STENCIL STATE FROM THE TRUSTWORTHY PASS (force=True)")
        writers = [r for r in hard if r["stencilEnable"] and r["writeMask"]]
        emit("draws sampled            : %d" % len(hard))
        emit("with stencilEnable       : %d" % sum(1 for r in hard if r["stencilEnable"]))
        emit("with a nonzero writeMask : %d" % len(writers))
        for r in writers[:10]:
            emit("   eid=%-7d writeMask=0x%02X ref=%d" % (r["eid"], r["writeMask"], r["ref"]))
        emit()
        emit("NOTE: %d samples of %d draws cannot prove absence of stencil writes."
             % (len(hard), len(draws)))
        emit("This pass exists to validate the instrument. The stencil question is")
        emit("settled by the usage analysis below, which requires no replay at all.")

        # ---- the actually decisive evidence ---------------------------------
        head("CUSTOMDEPTH CANDIDATE: ResourceId::1932")
        emit("From GetUsage (no replay, therefore trustworthy):")
        for t in ctrl.GetTextures():
            if not (t.creationFlags & rd.TextureCategory.DepthTarget):
                continue
            if "S8" not in t.format.Name():
                continue
            try:
                usages = ctrl.GetUsage(t.resourceId)
            except Exception as e:
                emit("  %s GetUsage failed: %r" % (t.resourceId, e))
                continue
            kinds = {}
            for u in usages:
                kinds.setdefault(str(u.usage).split(".")[-1], 0)
                kinds[str(u.usage).split(".")[-1]] += 1
            emit("  %-18s %dx%d  %s" % (t.resourceId, t.width, t.height, t.format.Name()))
            emit("      %s" % kinds)

        emit()
        emit("A full-res D32S8 target that is cleared every frame but never bound as")
        emit("a DepthStencilTarget and never read is the signature of UE's CustomDepth")
        emit("pass being ENABLED with NO primitives opted in (bRenderCustomDepth=false")
        emit("everywhere). If so, the stencil route needs no CVar forcing -- only")
        emit("per-primitive opt-in via the engine layer.")

    finally:
        ctrl.Shutdown()
        cap.Shutdown()
        rep = path + ".validate.txt"
        try:
            with open(rep, "w", encoding="utf-8") as f:
                f.write("\n".join(OUT))
            print("\n[report -> %s]" % rep)
        except Exception as e:
            print("write failed: %s" % e)
    os._exit(0)


main()
