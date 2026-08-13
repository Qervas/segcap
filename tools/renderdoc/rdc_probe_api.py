"""
rdc_probe_api.py -- interrogate the RenderDoc Python API shape for this build.

Run:
    set RDC_PATH=...\\capture.rdc
    qrenderdoc.exe --python rdc_probe_api.py

Why this exists: rdc_recon.py was written against assumed field names and used
defensive getattr chains. When the guesses were wrong the chains returned None,
and the report confidently printed "0 depth targets write stencil" instead of
failing. Wrong field name became a wrong answer, silently.

This script prints the ACTUAL attributes of the objects the recon needs, so the
recon can be written against facts. It deliberately does NOT catch exceptions
around the things it is probing -- a traceback here is a useful result.
"""

import os
import glob

import renderdoc as rd

OUT = []


def emit(s=""):
    OUT.append(str(s))
    print(s)


def members(obj, keep=None):
    """Public attributes of obj, optionally filtered by substring."""
    names = [n for n in dir(obj) if not n.startswith("_")]
    if keep:
        names = [n for n in names if keep.lower() in n.lower()]
    return sorted(names)


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
        # ---- TextureDescription: how do we identify the swapchain buffer? ----
        emit("\n=== TextureDescription members ===")
        textures = ctrl.GetTextures()
        emit(members(textures[0]))

        emit("\n=== TextureCategory enum values ===")
        emit(members(rd.TextureCategory))

        emit("\n=== textures flagged SwapBuffer (the real backbuffer) ===")
        for t in textures:
            if t.creationFlags & rd.TextureCategory.SwapBuffer:
                emit("  %s  %dx%d  %s" % (t.resourceId, t.width, t.height, t.format.Name()))

        emit("\n=== all DepthTarget textures, with creationFlags decoded ===")
        for t in textures:
            if not (t.creationFlags & rd.TextureCategory.DepthTarget):
                continue
            cats = [n for n in members(rd.TextureCategory)
                    if n[0].isupper() and isinstance(getattr(rd.TextureCategory, n, None), int)
                    and t.creationFlags & getattr(rd.TextureCategory, n)]
            emit("  %-18s %-22s %5dx%-5d  flags=%s"
                 % (t.resourceId, t.format.Name(), t.width, t.height, ",".join(cats)))

        # ---- ActionFlags: how are clears actually flagged? ----
        emit("\n=== ActionFlags enum values ===")
        emit(members(rd.ActionFlags))

        actions = flatten(ctrl.GetRootActions())
        emit("\n=== ActionDescription members ===")
        emit(members(actions[0]))

        emit("\n=== first 25 actions whose flags include any Clear bit ===")
        sdfile = ctrl.GetStructuredFile()
        shown = 0
        for a in actions:
            names = [n for n in ("Clear", "ClearDepthStencil", "ClearColour", "ClearColor")
                     if hasattr(rd.ActionFlags, n) and (a.flags & getattr(rd.ActionFlags, n))]
            if not names:
                continue
            emit("  eid=%-6d flags=%-28s name=%s" % (a.eventId, ",".join(names), a.GetName(sdfile)))
            shown += 1
            if shown >= 25:
                break
        if shown == 0:
            emit("  NONE -- clears are not flagged the way we assumed")

        # ---- D3D12 pipeline state: what is the depth target field called? ----
        emit("\n=== D3D12 pipeline state, sampled at a mid-frame draw ===")
        draws = [a for a in actions if a.flags & rd.ActionFlags.Drawcall]
        emit("drawcalls: %d" % len(draws))
        probe = draws[len(draws) // 2]
        ctrl.SetFrameEvent(probe.eventId, True)

        pipe = ctrl.GetD3D12PipelineState()
        emit("D3D12State members: %s" % members(pipe))

        om = pipe.outputMerger
        emit("\nOM members: %s" % members(om))

        emit("\ndepthTarget members: %s" % members(om.depthTarget))
        emit("depthTarget repr: %s" % repr(om.depthTarget))

        emit("\ndepthStencilState members: %s" % members(om.depthStencilState))
        dss = om.depthStencilState
        emit("  stencilEnable = %s" % getattr(dss, "stencilEnable", "<missing>"))
        for face in ("frontFace", "backFace"):
            f = getattr(dss, face, None)
            if f is not None:
                emit("  %s members: %s" % (face, members(f)))

        # The API-agnostic accessor is more portable than the D3D12-specific one;
        # find out whether it gives us what we need.
        emit("\n=== abstract PipeState accessors ===")
        ps = ctrl.GetPipelineState()
        emit("PipeState members (depth/stencil/target): %s"
             % members(ps, "depth") + " | " + str(members(ps, "target")))
        try:
            dt = ps.GetDepthTarget()
            emit("GetDepthTarget() -> %s ; members: %s" % (repr(dt), members(dt)))
        except Exception as e:
            emit("GetDepthTarget() raised: %r" % (e,))

    finally:
        ctrl.Shutdown()
        cap.Shutdown()
        rep = path + ".apiprobe.txt"
        try:
            with open(rep, "w", encoding="utf-8") as f:
                f.write("\n".join(OUT))
            print("\n[written %s]" % rep)
        except Exception as e:
            print("write failed: %s" % e)
    os._exit(0)


main()
