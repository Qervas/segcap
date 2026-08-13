# Architecture

A map of the codebase, for someone deciding whether it is a system or two
demos in a trenchcoat. The short answer is at the bottom of every section: what
is general, what is per-title, and where the line is.

## What this is

A DLL injected into a **shipping, unmodified** game, plus a Python harness that
drives the game unattended and collects the output.

There is no Unreal Engine source, no UE SDK, no plugin, and no editor. Nothing
here is built against the engine. The DLL finds the engine's structures in the
running process by measurement, hooks D3D12, and reads what the GPU was already
producing. Both supported titles are ordinary Steam installs, launched by their
own executables.

Dependencies are **MinHook** (function hooking, vendored) and **ViGEmClient**
(virtual gamepad, vendored) — plus `d3d12`/`dxgi`, which ship with Windows.

## The four layers

Measured, not asserted — `#include` graph, no cycles:

```
                    ┌─────────────────────────────────────────┐
  entry point       │ dllmain.cpp                             │
                    │ markers, threads, world probe           │
                    └───┬──────────┬──────────┬───────────┬───┘
                        │          │          │           │
                    ┌───▼──────────▼───┐  ┌───▼─────┐ ┌───▼──────┐
  D3D12 interception│ hooks*.cpp       │  │         │ │          │
                    │ install/election │  │         │ │          │
                    │ idprobe/capture  │  │         │ │          │
                    │ detours          │  │         │ │          │
                    └───┬──────────┬───┘  │         │ │          │
                        │          │      │         │ │          │
                    ┌───▼──────┐   │      │         │ │          │
  policy            │customdepth│  │      │perception│ │         │
                    │ what to  │   │      │         │ │          │
                    │ mark,    │   │      │         │ │          │
                    │ slot     │   │      │         │ │          │
                    │ budget   │   │      │         │ │          │
                    └───┬───┬──┘   │      └────┬────┘ │          │
                        │   │      │           │      │          │
                    ┌───▼─┐ │  ┌───▼────┐ ┌────▼───┐  │          │
  leaves            │ ue4 │ │  │readback│ │  ue4   │  │   log    │
  (no internal deps)│     │ └──►identity│ │        │  │          │
                    └─────┘    └────────┘ └────────┘  └──────────┘
```

**Leaves** — depend on nothing but `log.h`:

| module | responsibility |
|---|---|
| `ue4.*` | Find and read the engine: object array, FName pool, class hierarchy, reflected properties, `ProcessEvent` hook. Knows nothing about D3D12 or masks. |
| `readback.*` | Asynchronous GPU→CPU copy ring. Knows nothing about UE, ids, or which resource it is copying. |
| `identity.*` | The 255-slot table: which stencil value currently names which object, with serial numbers so a recycled slot cannot silently merge two objects. |
| `log.h` | Logging. |

**Policy** — `customdepth.*` decides *what to mark*: which components render, which are near the character, which are worth a slot when only 255 exist, and when to evict. This is where the interesting judgement lives, and it is engine-general.

**Interception** — `hooks*.cpp`, split by concern rather than by size:

| file | responsibility |
|---|---|
| `hooks_install.cpp` | Find and hook the D3D12/DXGI vtables. |
| `hooks_election.cpp` | Choose which render target carries our ids. |
| `hooks_idprobe.cpp` | Prove it carries them, by measurement. |
| `hooks_detours.cpp` | Record copies into the game's own command lists, at the game's own barriers. |
| `hooks_capture.cpp` | Turn a delivered buffer into a mask + sidecar on disk. |

**Entry point** — `dllmain.cpp` reads mode markers, starts threads, and probes world state.

## The seam: what is general, what is per-title

This is the question an evaluator actually wants answered.

**Title-specific facts live in exactly one file: `tools/games.py`.** A
`GameProfile` is a data record — where the executable is, how to get from the
main menu into gameplay, which level names mean "still in a menu", how many
objects mean "the engine is up", which log line means "rendering has started",
and the ceilings for each wait. Steam install paths are *discovered* through the
registry and `libraryfolders.vdf`, not hardcoded.

**The DLL is title-agnostic by construction**, and the two places that could
have gone wrong are worth naming because both were caught:

- *Renderable components* are found by walking the **class hierarchy** —
  anything descending from `UMeshComponent`. There is also a name list
  containing `ToyoSplineMeshComponent`, which is Stray's internal prefix, but it
  is an **additional accept**, not the mechanism: a title naming its components
  anything else still matches through the hierarchy. Removing the name list
  would only drop a few components that derive from `UPrimitiveComponent`
  directly.
- *The id carrier* is found by **measurement, not assumption**. UE 5.6 + Nanite
  does not write the stencil plane at all; it writes the stencil *value* into a
  separate `R16G16_UINT` colour target. The probe copies candidate targets and
  asks one question — what fraction of non-zero texels carry a stencil value we
  currently lease — and accepts only on a high fraction *and* enough distinct
  leased values to rule out coincidence.

Where a title name appears in the C++ it is nearly always in a **comment
explaining why a rule exists**, which is deliberate: several of those rules were
wrong once, and the comment records the evidence that changed them.

## Adding a third title

Write a `GameProfile`. In practice that is the executable location, the route
through the menus (mouse steps, key steps, or virtual-pad steps), the menu level
names, the object-count floor, and the ceilings — roughly 15 lines of data. Then
`python tools/capture.py <name> --preflight` walks the entire control flow with
no game running and fails in about a second if anything is wrong.

What may need code, and honestly:

- If the engine version moves the object array or FName pool, `ue4.cpp`'s
  discovery has to find them. It scans and validates rather than using offsets,
  so this is usually nothing.
- If the title exports ids somewhere new, the probe already searches — but a
  genuinely novel carrier could need a new candidate rule in
  `hooks_idprobe.cpp`.
- **D3D11 or Vulkan titles are out of scope.** The whole capture path is D3D12,
  and the single-plane stencil copy that makes it cheap has no D3D11 equivalent.

## Where it is thin

Stated because a reviewer will find it anyway, and because knowing it is part of
the design:

- **`dllmain.cpp` is 1,243 lines** and does mode-marker parsing, thread
  management, and world-state probing. Those are three responsibilities; it
  should be three files.
- **`Hooks` is a large class.** `hooks.h` is 717 lines and the class holds
  election state, probe state, capture state and several rings. The `.cpp` split
  keeps each concern readable, but they share one object and therefore one
  lifetime.
- **8 bits is a hard ceiling.** `CustomDepthStencilValue` is clamped 0–255 by
  the engine, so 255 concurrent ids is the budget regardless of scene
  complexity. The carrier on Nanite is 16-bit; the limit is the engine's
  property, not the transport's.
- **Sub-object labelling is not expressible.** The stencil value is per
  primitive, so a single mesh covering half the screen gets one id and is
  accurate but coarse.
- **The colour frame includes the HUD** on inZOI, because the copy happens at
  Present, after UI compositing. Copying before the UI pass would fix it.

## Where to start reading

1. `docs/DEBUGGING.md` — the largest artifact here, and the one that shows how
   the decisions above were reached, including the wrong turns.
2. `src/segcap/readback.h` — the header comment explains the transport design in
   one page.
3. `src/segcap/customdepth.cpp` — the marking policy, where the real judgement
   is.
4. `tools/games.py` — the whole per-title surface, in one file.
