"""
rdc_trigger.py -- trigger a RenderDoc capture remotely, with no in-game hotkey.

Run:
    qrenderdoc.exe --python rdc_trigger.py

Uses RenderDoc's target-control protocol to find the hooked process and ask it to
capture the next frame. This sidesteps the capture hotkeys entirely, which matters
here because F12 is Steam's screenshot key and Print Screen is bound to the Windows 11
Snipping Tool.

Optional environment variables:
    RDC_HOST    target host           (default "localhost")
    RDC_FRAMES  frames to capture     (default 1)
"""

import os
import time

import renderdoc as rd

HOST = os.environ.get("RDC_HOST", "localhost")
FRAMES = int(os.environ.get("RDC_FRAMES", "1"))


def enumerate_targets(host):
    """Walk RenderDoc's ident space for live hooked processes."""
    found = []
    ident = rd.EnumerateRemoteTargets(host, 0)
    while ident != 0:
        found.append(ident)
        ident = rd.EnumerateRemoteTargets(host, ident)
    return found


def main():
    idents = enumerate_targets(HOST)

    if not idents:
        print("NO HOOKED TARGETS FOUND on %s." % HOST)
        print()
        print("That means renderdoc.dll is not live in any running process. Check:")
        print("  - the game is actually running")
        print("  - it was launched through renderdoccmd (Steam launch options)")
        print("  - --opt-hook-children is present, since Steam runs the Stray.exe shim")
        print("    and the real renderer is its child process")
        os._exit(2)

    print("found %d hooked target(s): %s" % (len(idents), idents))

    ident = idents[-1]  # the most recently registered target
    ctrl = rd.CreateTargetControl(HOST, ident, "claude-auto-trigger", True)
    if ctrl is None:
        print("could not open target control on ident %d" % ident)
        os._exit(3)

    try:
        print("connected to: %s  (pid %s, api %s)" % (
            ctrl.GetTarget(), ctrl.GetPID(), ctrl.GetAPI()))
        print("triggering capture of %d frame(s)..." % FRAMES)
        ctrl.TriggerCapture(FRAMES)

        # Wait for the capture-complete message so we can report the real path
        # rather than guessing where RenderDoc put it.
        deadline = time.time() + 90
        while time.time() < deadline:
            msg = ctrl.ReceiveMessage(None)
            if msg is None:
                continue
            if msg.type == rd.TargetControlMessageType.NewCapture:
                print()
                print("CAPTURE COMPLETE")
                print("  frame  : %d" % msg.newCapture.frameNumber)
                print("  path   : %s" % msg.newCapture.path)
                print("  local  : %s" % msg.newCapture.local)
                os._exit(0)
            if msg.type == rd.TargetControlMessageType.Disconnected:
                print("target disconnected before the capture landed")
                os._exit(4)

        print("timed out after 90s waiting for the capture")
        os._exit(5)
    finally:
        try:
            ctrl.Shutdown()
        except Exception:
            pass


main()
