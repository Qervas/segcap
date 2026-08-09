# segcap

Per-pixel object-ID segmentation masks extracted from a running commercial game,
aligned with the rendered frames.

Target: **Stray** (Unreal Engine 4, D3D12, retail Steam build, no source, no
symbols, shipping binary with debug names stripped).

![overlay](docs/evidence/STRAY-GAMEPLAY-OVERLAY.png)

Every coloured region is one game object with a stable id. The sidecar for that
frame resolves them to `CharacterMesh0` (the cat), `Droid_Head` (B-12),
Morusque's mesh, spline meshes and level geometry — read out of the engine by
reflection at runtime, not hardcoded.

---

## Start here

| | |
|---|---|
| **The demo** | [`docs/evidence/stray-gameplay-demo.mp4`](docs/evidence/stray-gameplay-demo.mp4) — 75 frames, rendered left, mask overlaid right |
| **The debugging story** | [`docs/DEBUGGING.md`](docs/DEBUGGING.md) — every crash and wrong turn, and what actually found each one |
| **Where AI helped and where it was overridden** | [`docs/AI-USAGE.md`](docs/AI-USAGE.md) |
| **Output format** | [`docs/FORMAT.md`](docs/FORMAT.md) — masks, sidecars, actions, container |
| **PSO vs dynamic state** | [`docs/PIPELINE-STATE.md`](docs/PIPELINE-STATE.md) — why this decided the architecture |

If you read one thing, read `DEBUGGING.md` §7 and §8.

---

## Run it

Nothing here needs a human at the keyboard.

```powershell
.\build.ps1
.\tools\run_auto.ps1 -Seconds 340
```

That disables the screensaver, launches Stray's shipping executable suspended,
injects `segcap.dll` before its first D3D call, resumes it, focuses the window,
drives the menus with a virtual Xbox pad, patrols for scene variety, captures,
and restores your settings.

```powershell
python tools\overlay.py build\bin\segcap_mask_<N>.pgm `
    --frame build\bin\segcap_frame_<N>.ppm --sidecar build\bin\segcap_mask_<N>.json
python tools\identity_report.py --dir build\bin
python tools\verify_labels.py   --dir build\bin
python tools\pack.py --dir build\bin --out session.segcap --verify
python tools\make_demo.py --dir build\bin --out demo.mp4
.\build\bin\test_identity.exe
```

---

## How it works

**Render target identification.** Debug names are stripped in shipping builds,
so the CustomDepth target is elected from runtime-observable signals only:
stencil-capable format, scene resolution (matched by aspect ratio, *not* by
equality with the backbuffer — screen percentage is a thing), cleared every frame
but receiving almost no draws, persistent across frames. Incumbency is **earned**
— a target only gets thrash protection once it has produced a non-empty mask, so
an early wrong guess cannot entrench itself.

**Capture.** `ExecuteCommandLists` is sniffed for the command queue (it is not
reachable from the swapchain). `CreateRTV`/`CreateDSV` map opaque descriptor
handles to resources. `ResourceBarrier` shadows resource state so a transition to
`COPY_SOURCE` is correct rather than guessed. The stencil plane is copied with
`PlaneSlice=1` on the game's own queue into a 3-deep readback ring, collected by
fence polling — never blocking.

**Engine introspection.** `GUObjectArray` is located by structural search, not
byte patterns. Property offsets come from UE4 reflection by name
(`bRenderCustomDepth` is a *packed bit*, mask `0x40`, sharing a byte with
`bUseAsOccluder` — a byte write would corrupt the neighbours). Marking calls the
engine's own `SetRenderCustomDepth` UFunction, because writing the property
directly leaves the render proxy stale and nothing renders.

**Identity in 8 bits.** 255 slots against ~33,000 markable primitives. A slot is
a *lease*, not an identity; a 64-bit `stableId` keyed on `(pointer, serial)` is
the identity, and it survives losing a slot. Slots are leased only to primitives
the engine says it actually drew (`WasRecentlyRendered`), and handed back when
objects leave, so the working set follows the camera.

**Why through the engine rather than the renderer.** Stencil *enable*, *write
mask* and *pass op* are all baked into the PSO; only the reference value is
dynamic. So an injected DLL cannot make the game's draws write arbitrary IDs
without building stencil-writing twins of thousands of PSOs — a shadow renderer,
not instrumentation. UE's CustomDepth pass already has those PSOs and already
feeds `CustomDepthStencilValue` to `OMSetStencilRef`, so the intervention is
setting an integer on a game object. That is also why the ID is 8 bits: it is the
width of the pass the engine already runs. See
[`docs/PIPELINE-STATE.md`](docs/PIPELINE-STATE.md).

**Safety.** Writes happen on the game thread via a proven `ProcessEvent` hook,
are read-modify-write on the masked bit only, are verified after the fact, and
are reversible. The D3D12 hooks stay strictly read-only: none alters an argument,
none issues GPU work.

---

## Measured results

| | |
|---|---|
| Objects per mask | 60–89 distinct ids |
| Pixels labelled | 94–99.9% |
| Every mask id resolves via its sidecar | **0 unbound**, 0.000% of pixels |
| Identity survives slot loss | 52 objects left and returned; 55 held >1 slot |
| Slot ambiguity within a frame | **0** |
| Identity never renamed | **0 of 344** changed what they named |
| Region is one connected blob | median **1.000**, mean 0.907 |
| Labels that jump against the scene | 3 of 3,068 (0.098%); 2 explained by camera turn |
| Dithered (stipple) regions | 1.0% — UE4 dithered LOD, a real CustomDepth constraint |
| Did we change what the player sees? | **1.04× the noise floor** — PASS |
| Mask compression, lossless | **104.6×** (225/225 records verified byte-identical) |
| Descriptor misses | 0 |

The A/B number needs its context: with a completely static camera, Stray's
temporal AA still changes 12.2% of pixels between two consecutive frames. Judged
against zero, that test reports a visual change every single time regardless of
what you did. The noise floor is the measurement.

---

## Layout

```
src/segcap/      the injected DLL
  hooks.*        D3D12 interception, target election, capture
  readback.*     async GPU->CPU ring
  ue4.*          GUObjectArray, FName, reflection, ProcessEvent
  customdepth.*  visibility-driven marking
  identity.*     slot leases and stable 64-bit ids
src/injector/    suspended launch + inject + resume
src/vpad/        ViGEm virtual Xbox pad
src/tests/       25 assertions, no game required
tools/           harness, overlay, A/B diff, container, identity analysis
docs/            DEBUGGING.md, AI-USAGE.md, FORMAT.md, evidence/
```

---

## Notes

Two reversible changes to the machine, both made deliberately and both
documented: `steam_appid.txt` beside Stray's shipping executable (the documented
Steamworks mechanism that stops `SteamAPI_RestartAppIfNecessary` relaunching the
process out from under the injector), and Stray's `GameUserSettings.ini` set to
windowed 1280×720 with a backup at `GameUserSettings.ini.segcap-backup`.

The game is a legitimately owned retail copy. Marking is gated behind a marker
file so a run cannot mutate game state by accident.
