# Does any of this generalize? inZOI (UE5, D3D12)

Stray is UE4. Everything measured in this project was measured on one title, and
the renderable allowlist in `customdepth.cpp` literally contains
`ToyoSplineMeshComponent` — Stray's internal prefix. Generalization was the
weakest claim in the project, so it got tested.

**inZOI**: UE5, D3D12-only, 287 MB shipping binary, `BlueClient` project, no
anti-cheat, already installed. It is also a life sim, which turns out to matter.

Every run below is **census mode**: no marking, no GPU work on the game's queue.

---

## Result summary

| layer | generalizes? | evidence |
|---|---|---|
| Injection | **yes** | `VERIFIED: segcap.dll loaded in pid ...` on a 287 MB UE5 binary |
| D3D12 hooking | **yes** | queue sniffed, swapchain hooked, **0 descriptor misses** across 451 targets |
| Render-target election | **yes** | elects a scene-res depth-stencil target and holds it |
| `GUObjectArray` discovery | **yes** | found in 15 ms; 8,719 → 534,153 slots |
| `FName` resolution | **yes** | `block0[0]="None"`, 5 blocks corroborated, names decode correctly |
| Class identification | **yes** | 9,512 distinct classes, 99.8% resolve rate |
| **Property reflection** | **NO** | every class returns **0 reflected properties** |
| ProcessEvent discovery | **NO — and it crashed the game** | see below |

Two of the three layers carried over unchanged. The engine layer carried over
further than expected and then stopped at a specific, identifiable place.

---

## What worked, and why

### The D3D12 layer, unchanged

```
captured DIRECT command queue 000001DB4EF83240
backbuffer 2560x1600 format=R10G10B10A2_UNORM
frame 12901: 451 distinct targets observed, 0 descriptor misses
ELECTED 000001DFC482ABC0 (score 200)
```

Nothing in this layer knows what engine it is looking at, and it shows. Queue
sniffing via `ExecuteCommandLists`, descriptor→resource mapping, resource-state
shadowing and the election all worked on first contact.

One detail decided this. inZOI renders its scene at **1280×800 against a
2560×1600 backbuffer** — 50%, presumably DLSS/TSR. The election originally
hard-required backbuffer dimensions and would have rejected every real depth
target, exactly as it did on Stray with `ScreenPercentage=50` (DEBUGGING.md
§7.3). The fix made there — identify the scene cohort by **aspect ratio and
largest of that shape** — is what let this work untouched. A bug fixed properly
on one title generalized; a bug patched with a magic number would not have.

### The object array and FName, unexpectedly

I predicted this would break. UE5 changed a great deal about `FUObjectArray`,
the name pool and the property system, and RESUME.md said so in writing.

```
ue4: FOUND object array at 00007FF64003CBF0 after 15ms
ue4:   NumElements=8719 MaxElements=2162688 NumChunks=1 MaxChunks=33
ue4: FOUND FName blocks, block0[0]="None", 5 further blocks corroborated
ue4: [t+180s] array has 534153 slots
ue4:   resolved=19457  nullObject=1088  failedValidation=0  unreadable=0
```

It found both. The reason is a design choice made months earlier for a different
purpose: the search validates candidates by **structural shape** — chunk counts,
element sizes, pointer plausibility, and whether decoded names are readable text
— rather than matching a version-specific byte pattern. Shape survived the
version change. A signature scan would not have.

Names decode correctly, including inZOI's own classes. Every game prefixes its
types; Stray uses `Toyo`, inZOI uses `B1`:

```
19409  B1Image          17301  HorizontalBoxSlot     8352  Package
 7516  B1HorizontalBox  14574  CanvasPanelSlot       3220  B1Button
 6524  B1TextBlock      10562  ScriptStruct          2143  Texture2D
```

Also worth noting: the first four discovery attempts fail with
`no FChunkedFixedUObjectArray found`, then attempt five succeeds at t≈10s. That
is not a bug, it is the retry loop doing its job — the engine has not built the
array yet at t=2s.

---

## Where it stops: property reflection

```
introspect: 9512 DISTINCT CLASSES across 246284 objects
introspect: --- PrimitiveComponent : 0 reflected properties ---
introspect: --- SceneComponent : 0 reflected properties ---
introspect: --- Actor : 0 reflected properties ---
```

The classes are found. Walking their property chain returns nothing.

This is the real UE4→UE5 boundary for this project. The offsets in `ue4.cpp` —
`UStruct::ChildProperties` at `+0x50`, `PropertiesSize` at `+0x58`, the
`FField`/`FProperty` layout with its `FName` at `+0x28` — are UE 4.25–4.27
values. UE5 moved them.

**Everything downstream depends on this.** Property reflection is how
`bRenderCustomDepth` is located as a packed bit, how `CustomDepthStencilValue`
is written, and how the agent reads the pawn transform. Without it there is no
marking and no perception on UE5.

It is also the most tractable of the failures. `Engine::DumpStructLayout` was
written for exactly this situation — it dumps a `UStruct`'s raw qwords and tries
to interpret each under both the pre-4.25 and post-4.25 layouts, so the correct
offset can be **read off** rather than guessed. That is a bounded piece of work,
not a redesign.

---

## Where it stops badly: ProcessEvent, and a crash that was ours

The second census run killed inZOI. The log ends here:

```
ue4: PE candidate 69 -> 1169 valid, 506460 invalid (rejected)
ue4: PE candidate 70 ->  810 valid, 432080 invalid (rejected)
<dead>
```

Finding `ProcessEvent` means hooking UObject vtable slots 60..80 one at a time
and seeing which behaves like it. On Stray that converged on slot 68 with a 4:1
valid ratio. On UE5 the ratios are 1169:506460 and 810:432080 — the probe is
hooking hot functions that are *not* `ProcessEvent` and calling them with
`ProcessEvent`'s signature. One of them took the process down.

**The honest part: this run was described as read-only, and it was not.**

Census mode suppressed GPU work and suppressed UObject writes. It never gated
vtable hooking, because when census mode was written the only mutations worth
thinking about were the ones it did gate. Probing twenty vtable slots on a live
engine is neither a read nor safe, and on an unfamiliar engine it is a loaded
gun.

The mode was doing most of what it promised, which is exactly why nobody noticed
it was not doing all of it. Fixed: census mode now returns before ProcessEvent
discovery and says so in the log.

---

## What this says about the design

Ranked by how well each layer travelled:

1. **Graphics-API-level code generalized completely.** It depends on D3D12, not
   on the engine, and D3D12 does not change between UE versions.
2. **Structural search generalized.** Validating by shape rather than by
   signature was chosen to avoid brittleness against patches; it turned out to
   survive a whole engine major version.
3. **Hardcoded struct offsets did not, and could not.** They are the one place
   the code encodes a specific engine build. That they are confined to a handful
   of named constants in `ue4.cpp` — rather than scattered — is why the fix is
   bounded.

The failure is where a reasonable person would predict, and the parts that
travelled are the parts built to be validated rather than assumed.

---

## inZOI-specific observations

- **It drives itself.** It is a life sim with autonomous agents, so it generates
  evolving gameplay with no input at all. For dataset collection that removes
  the "who plays the game" problem which motivated the closed-loop agent on
  Stray.
- **837 `InstancedStaticMeshComponent`s.** The latent instancing issue flagged
  in the Stray writeup — one component representing many visual instances would
  receive one ID — is live here. Stray never exercised it.
- **The object array is UI-dominated at the menu.** ~90% of the 246k objects at
  the character creator are widgets (`B1Image`, `HorizontalBoxSlot`,
  `CanvasPanelSlot`). Any census taken before reaching a world describes the
  menu, not the game.
- **The screensaver bit again.** `desktop=Screen-saver`, focus impossible, all
  gamepad input discarded — the same failure as DEBUGGING.md §7.1, in a script
  that ported the "disable the setting" half and not the "kill the running one"
  half. It is listed as a known trap in RESUME.md and it still happened.
