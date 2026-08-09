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
| **Property reflection** | **yes, after a fix** | 0 properties → **153** on `UPrimitiveComponent`; see below |
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

## Where it stopped, and how it was fixed

```
introspect: 9512 DISTINCT CLASSES across 246284 objects
introspect: --- PrimitiveComponent : 0 reflected properties ---
introspect: --- SceneComponent : 0 reflected properties ---
```

The classes were found. Walking their property chain returned nothing. This was
the real UE4→UE5 boundary, and everything downstream depended on it: property
reflection is how `bRenderCustomDepth` is located as a packed bit, how
`CustomDepthStencilValue` is written, and how the agent reads the pawn
transform. No properties meant no marking and no perception.

### Step 1: dump the struct instead of guessing

`DumpStructLayout` prints each qword of a `UStruct` and tries to decode it under
both the pre-4.25 and post-4.25 interpretations. Side by side with Stray:

| offset | Stray (UE4) | inZOI (UE5) |
|---|---|---|
| `+0x40` | `SceneComponent` (SuperStruct) | `SceneComponent` — same |
| `+0x48` | `WasRecentlyRendered` (Children) | `WasRecentlyRendered` — same |
| `+0x50` | ptr → decodes `MinDrawDistance` | ptr → decodes **nothing** |

So `ChildProperties` was at the same offset and was a valid pointer. The chain
was there. What had moved was the layout *inside* `FField`.

### Step 2: find the new offset by trying all of them

`ProbeFieldNameOffset` walks every aligned offset in the first 0x48 bytes of the
first field, follows the chain at several candidate `Next` offsets, and prints
which combinations decode into readable names:

```
name@+0x14 next@+0x18 -> 8 links: NavAvoidanceMask, NavAvoidanceMask, NavAvoidanceMask...
name@+0x20 next@+0x18 -> 8 links: MinDrawDistance, LDMaxDrawDistance,
                                  CachedMaxDrawDistance, DepthPriorityGroup
```

The second row is the answer — those are the same four properties, in the same
order, as Stray's UE4 dump. **UE5 shifted `FField` back 8 bytes:
`NamePrivate` 0x28 → 0x20, `Next` 0x20 → 0x18.**

The first row matters too. It decoded eight links, so a test that counted
successful decodes would have accepted it — but every link is the *same name*.
Printing the decoded sample rather than a count is what exposed it, and the
calibration below scores on **distinct** names for that reason.

### Step 3: calibrate at runtime, do not swap one constant for another

`CalibrateFieldLayout()` runs once after discovery on every run. It tries the
candidate offsets against classes known to have properties and keeps whichever
yields the most distinct decoded names. The UE4 values remain the default, so a
build where calibration cannot run behaves exactly as it did before. The same
delta is applied to the `FProperty` fields that follow the header
(`ArrayDim`, `ElementSize`, `OffsetInternal`, and the `FBoolProperty` bit
fields), since the whole struct moved together.

```
ue4: field layout calibrated: name@+0x20 next@+0x18
     (12 distinct names; shift -8 vs UE4.25-4.27)
introspect: --- PrimitiveComponent : 153 reflected properties ---
introspect:   +0x0271  1  BoolProperty  bRenderCustomDepth
introspect:   +0x02A4  4  IntProperty   CustomDepthStencilValue
```

**Both marking properties are found on UE5**, at completely different offsets
from Stray's (`+0x216` mask `0x40`, and `+0x220`) — located by name, which is
the whole point of doing this by reflection.

Verified not to regress UE4: a full Stray capture run after the change produced
201 masks / 200 frames / 201 sidecars, resolved `bRenderCustomDepth` at the
unchanged `+0x216 mask 0x40`, and passed label verification with 0 identities
renamed.

### What is still not done on inZOI

Reflection works, so the route is open, but marking has not been attempted:
`ProcessEvent` discovery still has no UE5 answer, and it is the game-thread
execution point every write depends on. That is the next wall, and unlike this
one it is genuinely dangerous — probing vtable slots is what killed the game
earlier in this session.

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
