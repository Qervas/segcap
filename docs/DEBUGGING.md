# Debugging log

Every failure in this project, what it looked like, what actually caused it, and
what fixed it.

> *"how you diagnosed things when they didn't work, the debugging story matters
> more to me than the final coverage number"* — Antoine

---

## The pattern underneath almost all of it

Roughly two thirds of the time lost on this project went to a single failure
shape:

> **A measurement that could not observe the thing it was being used to judge,
> reporting the answer that happened to match my hypothesis.**

Not one of these threw an exception. Every one produced a plausible number in a
log, and I acted on it. The fix was never cleverness — it was always making the
instrument show its work, or finding a second independent way to ask the same
question.

Eight instances, listed in §2. The most expensive cost three runs; the most
embarrassing had me telling the user their machine was locked when it was not.

There is a second, smaller pattern worth naming: **three of my "fixes" made
things worse**, because I bounded, guarded, or optimised the wrong thing without
checking the arithmetic first.

---

## 1. Crashes — things that actually broke the game

### 1.1 Swapchain rotation destroyed in-flight readback resources

**Symptom.** Stray exited mid-run: `game exited early -- possible crash`. First
and only crash caused by our code, immediately after adding colour capture.

**Cause.** A swapchain rotates between several backbuffers, so
`GetBuffer(GetCurrentBackBufferIndex())` returns a **different**
`ID3D12Resource*` each frame. `Readback::Prepare` compared the resource
**pointer** to decide whether it was already initialised:

```cpp
if (preparedFor_ == target && width_ == desc.Width && ...) return true;
```

So every frame it believed it had a new target, called `ReleaseResources()` —
destroying readback buffers and command lists **while copies were still in
flight** — and rebuilt them. A resource-destruction storm on the render thread.

**Fix.** Key on the **layout** (dimensions, format, plane slice), not the
pointer. Only the layout determines ring sizing, and `Enqueue` already receives
the actual resource to copy from, so a rotating source needs no special handling.

**What I should have done.** Verified on the fixture first. It has a 3-buffer
swapchain and reproduces the exact rotation; it caught the fix in 12 seconds
without touching the user's game. I went straight to Stray because the feature
"obviously" only mattered there.

---

## 2. Measurements that lied

### 2.1 Defensive `getattr` turned wrong field names into confident zeros

**Symptom.** The first RenderDoc analysis reported: backbuffer `8160x8160`,
`0 full-res stencil targets`, `0 depth-stencil clears`, `debug names present
12592/12592`. All four wrong.

**Cause.** The script was written against *assumed* RenderDoc API field names,
wrapped in `getattr` chains with `None` fallbacks. A wrong guess returned `None`,
which the caller rendered as zero. The resolution came from a heuristic
("largest colour texture") that picked a shadow atlas.

**Fix.** `rdc_probe_api.py` — dump the actual attributes of every object before
using it, then write the analysis against facts. Read failures now report rather
than degrade.

**Lesson.** When probing an unfamiliar API, defensive fallbacks are *harmful*.
They convert "I don't know this API" into "here is your answer". A crash would
have been more useful.

### 2.2 Sampling 12 of 1365 draws and reporting absence

**Symptom.** `writes stencil: no` for Stray's scene depth.

**Cause.** The scan sampled every ~113th draw. UE's CustomDepth pass is a
handful of draws; that sampling would miss it nearly every time. The result meant
"I did not look hard enough", not "there is no stencil".

**Fix.** Depth-stencil state is **baked into the PSO**, so grouping draws by
`pipelineResourceId` and inspecting each PSO once is exhaustive rather than
sampled. (Taken directly from the brief's own "dynamic vs baked into a PSO".)

### 2.3 A probe that filtered out the evidence it was built to find

**Symptom.** Layout probe of `UPrimitiveComponent` showed `SuperStruct` correctly
but printed nothing at `+0x48` / `+0x50` where the property chain lives.

**Cause.** The probe only printed entries where at least one interpretation
decoded successfully. So "this pointer is null" and "this pointer is present but
I'm decoding it wrong" looked **identical** — and those need completely different
fixes.

**Fix.** Print every non-null qword including ones that decode as nothing.

### 2.4 Name lookup returned a stub object

**Symptom.** `0 properties declared directly on UPrimitiveComponent`, and all
four required properties `MISSING`. Two runs spent suspecting my `FField` /
`UStruct` offsets.

**Cause.** My offsets were **correct the entire time**. `FindClass` matched the
first object named `PrimitiveComponent` whose class was `Class` — and that object
had `Children` *and* `ChildProperties` both null. Structurally coherent (its
`NumStructBasesInChainMinusOne` was 3, the right inheritance depth) but
unpopulated.

**Fix.** Diagnosed by getting the same class a **second independent way** —
reading `ClassPrivate` off a live `StaticMeshComponent` instance — and comparing.
Different pointer, fully populated, `ChildProperties` at `+0x50` decoding to
`"ForcedLodModel"`. `FindClass` now requires a non-null property chain and reports
how many stubs it skipped.

### 2.5 Photographing the buffer 38 seconds before anything was in it

**Symptom.** Three consecutive runs concluded "CustomDepth doesn't work": masks
came back `distinct stencil values: 0` every time.

**Cause.** `OnMaskReady` dumped only the **first three frames it ever saw**.
Those landed at `t=3.1s`. Marking did not happen until `t=41.2s`.

```
[segcap  3.125] mask frame 2 dumped ... distinct stencil values: 0
[segcap 41.250] marked 267 primitives
```

Both lines were in every log. I read them repeatedly across three runs as
*separate facts* and never as a sequence.

**Fix.** Dump on **content** rather than frame number, removing the timing
coupling entirely.

**Cost.** Two correct fixes were written for hypotheses that were not the
problem — the render-proxy staleness (§3.2) was a real bug, and the engine setters
were the right call, but neither was why the mask was empty.

### 2.6 The stale memory map — six separate bugs from one snapshot

`IsReadable` works off a committed-memory map built once. The game allocates
constantly. Every time the map went stale, `IsReadable` rejected valid addresses
and the caller interpreted "I can't see this" as "this isn't what I'm looking
for". **Failing closed looks like a finding.**

| # | Symptom | Reality |
|---|---|---|
| 1 | resolved object count *dropped* as the level loaded | new chunks invisible |
| 2 | `PrimitiveComponent descendants: 622` | actually 38,479 |
| 3 | `unreadable=8165` of a 21,273 sample | array grew 196k → 319k slots |
| 4 | PE candidate 68: `30145 valid/0 invalid` one run, `742/654` the next | `UFunction`s in new memory counted as "not a UFunction" |
| 5 | only 267 of ~29,000 primitives marked | `MarkPrimitives` never refreshed |
| 6 | `ProcessEvent not found in slots 60..80` | same as #4, sank the whole run |

**Fix.** Refresh before every walk, and before every `ProcessEvent` candidate.

### 2.7 Focus check compared the wrong thing

**Symptom.** `window focused: False` on runs where the game was plainly in front.

**Cause.** The check required `GetForegroundWindow() == MainWindowHandle`. A
fullscreen D3D12 title frequently has a **different HWND** in the foreground than
the one .NET reports as `MainWindowHandle`. The focus was fine; the question was
wrong.

**Fix.** Compare owning **PIDs** — "is the game the active application" is the
question that actually matters.

### 2.8 I diagnosed the user's machine instead of my instrument

**Symptom.** `foreground pid 0 = Idle`. I concluded the session was locked and
told the user so.

**Cause.** `GetForegroundWindow()` returns `NULL` transiently — notably during an
exclusive-fullscreen mode switch, which is exactly what Stray does ~20s after
launch, precisely when the harness polled. `GetWindowThreadProcessId(NULL, ...)`
yields 0, which I printed as "Idle".

**Fix.** `HasForegroundWindow()` distinguishes "no foreground window" from "a
window owned by PID 0". The log now says `<none - mid mode-switch>`. Poll longer,
and treat a null foreground as *wait*, not *escalate*.

**Note.** Verified afterwards that the automation process runs on
`WinSta0\Default` and `GetForegroundWindow()` works correctly from it — the check
I should have run *before* asserting anything about the user's machine.

---

## 3. Wrong model of the engine

### 3.1 `bRenderCustomDepth` is a packed bit, not a byte

**Symptom.** None — and that is the point. Reflection reported it at `+0x216`,
size 1, and everything looked fine.

**Cause.** Seven properties printed at the **same offset** `+0x212`
(`bReceivesDecals`, `bOwnerNoSee`, `bRenderInMainPass`, …). UE packs bools into
individual bits. Writing `1` to the whole byte would have silently flipped
`bUseAsOccluder`, `bSelectable`, and the occlusion/visibility flags — exactly the
"did you change what the player sees" failure, and it would have *looked like it
worked*.

**Fix.** Read `FBoolProperty::FieldMask` (`+0x7B`); every write is
read-modify-write, then verified by reading back and asserting **only** the masked
bit changed.

**Honest note.** This surfaced only because seven names printed at one offset —
visible only because I had made the probe verbose two steps earlier for an
unrelated reason. A lucky catch, not foresight.

### 3.2 Writing the property is not enough — the render proxy is stale

**Symptom.** 64 primitives marked, 64 writes **verified**, and the CustomDepth
target still `binds=0` with an empty mask. The writes were correct and did
nothing.

**Cause.** The renderer does not read `bRenderCustomDepth` per frame; it reads a
`FPrimitiveSceneProxy` built on the render thread.

```cpp
void UPrimitiveComponent::SetRenderCustomDepth(bool bValue) {
    if (bRenderCustomDepth != bValue) {
        bRenderCustomDepth = bValue;
        MarkRenderStateDirty();      // ← this is what we skipped
    }
}
```

**Fix.** Find `SetRenderCustomDepth` as a `UFunction` in the class's `Children`
chain and **invoke it through `ProcessEvent`** — asking the engine to do the
thing rather than poking at its state. Which is precisely what the brief means by
*"hooking ProcessEvent to call engine functions safely on the game thread"*.

---

## 4. Fixes that made things worse

### 4.1 Bounding the array walk cut coverage by 94%

**Change.** Capped `MarkPrimitives` at 3000 objects per pass, to avoid stalling
the game thread.

**Result.** Marked primitives went **267 → 16**.

**Cause.** 3000 per pass against ~350,000 slots sweeps 4% of the array, and the
low indices are almost entirely engine bootstrap objects (`Class`, `Package`,
`Function`). Renderable components live much further in.

**Real problem.** I bounded the wrong thing. The array walk is cheap read-only
CPU work; the expensive part is the two `UFunction` calls per match, which run
inside `ProcessEvent`.

**Fix.** Split them: `CollectCandidates` walks the whole array on any thread;
`MarkBatch(n)` marks a bounded batch on the game thread.

### 4.2 The harness aborted on failure, discarding the diagnosis

**Change.** Break out of the idle loop when the log showed the ProcessEvent search
had finished — including when it **failed**.

**Result.** Failed runs terminated before the object samples ran, throwing away
exactly the data needed to explain the failure.

**Fix.** Only success short-circuits. Stopping early on success is an
optimisation; on failure it destroys evidence.

### 4.3 Persistence scoring left scene depth *positive*

**Change.** Added a persistence bonus (+60) to election scoring.

**Result.** Scene depth — whose stencil UE4 fully owns — went from `-200` to
**`+10`**. Still electable. Losing the real CustomDepth target would have meant
silently emitting masks made of lighting-channel bits.

**Fix.** Scene depth is now a **hard rejection**, not a penalty. "No viable
candidate" is the correct answer there; a confidently wrong mask is worse than no
mask.

---

## 5. Election instability (three rounds)

| version | frames | census blocks | ≈ election changes |
|---|---|---|---|
| per-frame signals only | 4201 | 1099 | ~1085 |
| + persistence, incumbency, deterministic tie-break | 5101 | 141 | ~124 |
| + accumulated evidence | 2701 | **11** | **~2** |

**Round 1.** Two candidates scored **identically** at +200, and ties fell to
`std::sort`'s ordering of equal elements — a coin flip re-tossed 30 times a
second. Elections split 541/540. The two winners turned out to be **transient
loading-screen depth buffers** alive for 25 seconds; the real candidate lived all
118.

**Round 2.** Added persistence (`framesSeen` accumulated across the session),
deterministic tie-break by resource address, and an incumbency bonus. Still
flapped.

**Round 3.** Root cause: `targets_` was rebuilt from scratch every frame, so a
candidate not bound or cleared in a given frame was **absent from the ballot
entirely** — an incumbency bonus cannot help something that is not on it. Two
candidates alternating their absence traded the win back and forth. Election now
reads **accumulated evidence**, with entries pruned after 600 unseen frames so a
destroyed buffer cannot win on stale persistence.

---

## 6. Tooling and environment

### 6.1 RenderDoc reported success while nothing was hooked

`renderdoccmd` printed `Launched as ID 38920` — true. No capture appeared, F12 did
nothing. `renderdoc.dll` was absent from both Stray processes, and the parent
chain explained why:

```
steam.exe → Stray.exe (shim) → Stray-Win64-Shipping.exe
```

The hooked process called `SteamAPI_RestartAppIfNecessary` — *"if I wasn't
launched by Steam, ask Steam to relaunch me, then exit"* — exited, and Steam
spawned a fresh unhooked chain.

**Both "the tool said it worked" and "the tool did work" were true while the
outcome was wrong.** Only checking loaded modules distinguished them. This is why
the injector now verifies module presence rather than trusting a return value.

### 6.2 Suspended launch alone is useless

Injecting into a running process misses every descriptor created before
injection, and there is no API to enumerate existing descriptors — permanently
blind. Added `--launch` to start suspended.

**Still 950 misses.** `LoadLibraryW` returns as soon as `DllMain` returns, and
`DllMain` only spawns the init thread, so the injector resumed the game during the
~300 ms spent building dummy devices.

**Fix.** A named event the DLL signals once hooks are live, which the injector
waits on before `ResumeThread`. Misses went **950 → 0**.

### 6.3 `renderdoccmd` splits its own arguments on whitespace

```
Launching 'Files' with params: (x86)\Steam\steamapps\...
```

It consumed `C:\Program` as the `-d` value and took `Files` as the executable,
then reported `Failed to launch process` — pointing at the target, not the
parsing. Fix: pass a single pre-quoted argument string.

### 6.4 `_wfopen_s` locks the log against all readers

The `_s` variants default to `_SH_DENYRW`, unlike plain `fopen`. The log was
unreadable while the game ran, **and** the linker could not rebuild the DLL. For a
tool whose only live output is that file, this turned "did the hook fire?" into a
question answerable only after quitting. Fix: `_wfsopen(..., _SH_DENYWR)`.

### 6.5 PowerShell does not interpolate inside `-`-prefixed native arguments

`-DCMAKE_BUILD_TYPE=$Config` reached CMake as the literal string `$Config`, and
Ninja failed several steps later with `expected newline, got lexing error` —
pointing nowhere near the cause. On the **same line**, `-S $root` expanded
correctly, because there the variable is its own token. Fix: quote the whole
argument.

### 6.6 `SendInput` does not reach Stray

Four scan-code `Enter` taps, foreground set, and the object array stayed at the
menu's 173,598 slots instead of growing to ~320,000. Proven, not assumed — the
array size is the measurement.

**Fix.** ViGEm virtual gamepad: a kernel bus driver the game cannot distinguish
from real hardware. Also solves scene variety for dataset collection, since a pad
that *moves* generates varied frames.

### 6.7 `Engine().Discover()` constructed a temporary

Discovered every address into an object destroyed on the same line. Reported
success and kept nothing.

### 6.8 `--launch` greedily consumed `--dll`

Everything after `--launch` was treated as target arguments, swallowing `--dll`
and producing a usage dump instead of an error naming the real problem. Fix:
explicit `--args`.

### 6.9 Mask PGMs written into the game's install directory

A relative filename resolves against the **game's** working directory. Three 4 MB
dumps landed in Stray's folder. Fix: absolute path next to the DLL via
`__ImageBase`. The stray files were removed.

### 6.10 Reporting a crash that never happened

I reported the fixture as `CRASHED` after injection based on `Get-Process`
returning nothing. It had exited cleanly — **exit code 0 both with and without
injection**. `Get-Process` cannot distinguish a crash from a clean exit; the exit
code can.

### 6.11 Build environment

- **CMake 4.1 predates VS 18.** Its generator list stops at `Visual Studio 17
  2022`, and `vswhere.exe` is not installed, so CMake could not find the compiler
  at all. Resolved with Ninja + an imported `vcvars64.bat` environment.
- **Stale `CMakeCache.txt`** makes a generator switch fail as a bare
  `configure failed`, which reads as a toolchain problem. `build.ps1` detects and
  clears it.
- **`project(... LANGUAGES CXX)`** is insufficient once MinHook (C) is added:
  `can not determine linker language for target: minhook`.

---

## 7. Getting to unattended gameplay capture

Everything above was proved at a menu or on a fixture. Making the system run a
real level start to finish, with nobody at the keyboard, broke it in seven more
places. Five of the seven produced *plausible healthy-looking logs*, which is
the same pattern as §2.

### 7.1 The screensaver — four consecutive failed runs

Symptom: `window focused: False`, the virtual gamepad ignored, the run stranded
at the main menu producing menu frames. `GetForegroundWindow()` returned NULL
for 60 seconds straight.

I had already misdiagnosed this exact symptom twice: once as an
exclusive-fullscreen mode switch (plausible — it does transiently return NULL
during one), and once, worse, as a locked session, which meant telling the user
their machine was locked when it was not. They corrected me. I was wrong both
times and, critically, I had *inferred* both answers rather than measuring them.

The measurement that settles it is the **input desktop name**:

```
input desktop name : 'Screen-saver'      <- not 'Default'
screensaver running: True
screensaver timeout: 60 s
ScreenSaverIsSecure: 0                   <- NOT locked
```

With the user AFK there is no physical input, so after 60 seconds Windows
activates the screensaver and switches the input desktop from `Default` to
`Screen-saver`. From `Default`, `GetForegroundWindow()` then correctly returns
NULL — there genuinely is no foreground window there any more — and no injected
input can be delivered.

Three states produce the identical symptom and need different fixes:

| State | `OpenInputDesktop` | Desktop name | Fix |
|---|---|---|---|
| Mode switch in progress | succeeds | `Default` | wait |
| Screensaver active | succeeds | `Screen-saver` | disable + dismiss |
| Session locked | **denied** | — | nothing in user space |

`SetThreadExecutionState(ES_DISPLAY_REQUIRED)` — which the harness already
called, and which I had assumed covered this — prevents the *display powering
down*. The screensaver is a separate mechanism. Necessary but not sufficient,
and "I already handle that" is why it took four runs.

Fix: save `SPI_GETSCREENSAVEACTIVE`, disable it for the run, kill an
already-running `scrnsave.scr`, restore the user's setting afterwards. Focus now
lands at **t=2s** every run.

### 7.2 Steam relaunch, and what fixing it unlocked

`SteamAPI_RestartAppIfNecessary` relaunches the game through Steam and exits, so
the suspended process we injected into was never the process that rendered. That
had forced `--watch`-mode injection (attach after the fact) for the whole
project, which permanently lost every descriptor view created before we arrived.

Writing `steam_appid.txt` next to the shipping executable makes that call a
no-op. It is the documented Steamworks mechanism for running your own build, not
a DRM bypass — Steam still has to be running.

That one 8-byte file unlocked three things at once: injection before the first
D3D call (**0 descriptor misses**), `-windowed` to remove the fullscreen mode
switch entirely, and `-ResX/-ResY` to drop readback bandwidth to a quarter.

### 7.3 The election rejected every real depth target

In-level, the census contained exactly **one** depth-stencil target: a 1×1
dummy. Read literally that says the game renders a 3D scene with no depth
buffer, which is absurd — but the log said it flatly and nothing errored.

The census can only report targets it successfully resolved, so it cannot
distinguish "absent" from "filtered out by me". Logging every distinct resource
passed to `CreateDepthStencilView`, *upstream* of the filter, answered it in one
run:

```
CreateDSV #2: R32G8X24_TYPELESS 640x360 ... D32_FLOAT_S8X24_UINT
CreateDSV #5: R32G8X24_TYPELESS 640x360
CreateDSV #6: R32G8X24_TYPELESS 640x360
```

They were 640×360 against a 1280×720 backbuffer, because the game was configured
with `ScreenPercentage=50`. The election hard-required dimensions *equal to the
backbuffer*, on the reasoning that anything smaller is a shadow map.

That reasoning was never sound. Screen percentage, dynamic resolution and
upsampling are all normal. The scene cohort is identified by **matching the
backbuffer's aspect ratio and being the largest of that shape** — shadow atlases
are square, downsample chains are a fraction. Same §2 lesson: a filter that eats
its own evidence.

### 7.4 Incumbency cemented a first-frame guess

The +40 incumbency bonus exists to stop election thrash (§5) and it works. But
it was granted for *being elected*, so a target elected at frame 2 — when only
one candidate had been observed at all — kept winning forever:

```
+270  CFB02B30  few binds (2) => CustomDepth pass; INCUMBENT     <- 230 + 40
+250  EEA5320   cleared but never bound => CustomDepth with no opt-ins
```

`EEA5320` is the correct target and says so on its own merits; 230+40 beat it
every frame for the entire session and every mask came back empty.

Fix: incumbency must be **earned, not granted**. The bonus now applies only once
the elected target has produced a non-empty mask. A target that has never
yielded a single non-zero pixel has no stream worth protecting, so the election
stays free to correct itself.

### 7.5 Slot thrash — 8,745 evictions in 150 seconds

```
marked 250 this batch (0 refused); 255 live slots, 15000 identities, 14745 evictions
```

`markPass_` advanced once per *batch*, so the registry's "never evict something
seen this pass" guard only ever protected the current 250. The next batch evicted
all of them. The arithmetic was never survivable — ~38,000 markable primitives,
255 slots — and cycling does not label more of the scene, it just guarantees no
object holds an id long enough to be tracked across two frames.

Note how healthy `marked 250 this batch` reads. It reports work done, not work
retained.

Fix: take only as many candidates as there are free slots. Evictions went to **0**.

### 7.6 255 slots spent on things nobody can see

With the thrash fixed the working set was stable, 0 evictions — and every
in-level mask was still empty, or contained exactly one object.

The 255 marked primitives were simply the first 255 found. Against 32,836
candidates with a few hundred visible at any moment, essentially none of them
were on screen. It had *appeared* to work at the menu only because there were
few enough objects there that some marked ones happened to be visible.

`255 live slots, 0 evictions` is a perfectly healthy way to describe labelling
255 objects nobody can see.

Fix: ask the engine. `UPrimitiveComponent::WasRecentlyRendered` was sitting in
the reflected function list at `+0x48` the whole time. Slots are now leased only
to primitives the renderer actually drew, and `RefreshVisibility` hands slots
back when objects leave the screen, so the working set follows the camera.
Acquisition uses a 0.3s tolerance and release 1.0s — deliberately unequal, or an
object hovering at the edge of visibility thrashes its own slot.

Measured: ~23% of tested primitives visible at once, 60–89 distinct ids per
mask, 94–99% of pixels labelled.

### 7.7 Photographing a transient

First run with visibility marking: 21 masks, all with one object. Recording
started at t+201.4s and the last capture was at t+203.0s — **all 21 captures in
a 1.6-second window**, taken while the working set was still filling.

Two causes compounding. Marking ran only after the staged diagnostic samples
finished at t+180, leaving ~40s of a 240s run; and a capture stride of 5 frames
at 60fps spends a 40-capture budget in 3.3 seconds.

Fix: marking moved onto its own thread starting the moment ProcessEvent is
verified — sampling is diagnostics, marking is the product, and the product
should not wait on the diagnostics. Stride raised to 30 frames so 150 captures
span 75 seconds.

The data was never wrong. It was a photograph of a transient, correctly taken.

### 7.8 The sidecar described a different moment than its mask

Caught by the overlay, not by any log line. A gameplay mask contained ids 154,
156, 242 and 255 with **no binding at all** in the sidecar written beside it —
5.4% of the frame labelled with an id whose meaning had already been recycled.

Readback is asynchronous by design (that is what keeps the render thread off the
GPU's critical path), and the marking thread continuously releases and re-leases
slots. Reading the slot table when a mask *arrives* describes a different moment
than the one that wrote its pixels.

Fix: the marker publishes an immutable `shared_ptr` snapshot whenever the table
changes — a few times a second, not per frame — and `Present` records which
snapshot was live when it submitted each copy. Copying a `shared_ptr` per frame
is free; rebuilding a 250-entry table per frame would not have been. History
misses went from unmeasured to **1 in 76 frames**, and the remaining one is
logged as a warning rather than silently trusted.

### 7.8b The residual, measured and then fixed

The paragraph above originally ended "a small number of ids can outlive their
binding. Not yet fixed." That was true and, as an estimate, wrong by an order of
magnitude — because I had estimated it instead of measuring it.

`tools/identity_report.py` cross-checks every mask's pixels against its sidecar.
On a 37-second window it found 22 unbound ids, 0.27% of labelled pixels: small
enough to justify the wave-through. On a 150-second window with real slot
turnover it found **505 unbound ids, 3.17% of labelled pixels**. Same code, same
defect; the short window simply did not contain enough of the event.

Fix: `ReleaseSlot` moves the binding into a `recentlyReleased_` map instead of
dropping it, and the sidecar emits those alongside the live ones flagged
`"released": true`. The entry is removed the instant the slot is reissued, so
the two maps stay disjoint and a slot can never appear twice. A consumer wanting
only current labels filters on the flag; without it those pixels were simply
undecodable.

Re-measured on a fresh 150-second session: **0 unbound ids, 0.000% of labelled
pixels**, across 3,476 distinct ids over 76 masks.

The lesson is not "fix the residual". It is that "small enough to ignore" was a
guess wearing the clothes of a measurement, and the number that justified it
came from a window too short to contain the failure.

### 7.8c The observation window was hiding the headline result too

The same short window quietly undermined the project's central identity claim.

`identity_report.py` checks three things, of which the third is the one that
matters: an object that loses its slot and comes back must resume its ORIGINAL
stableId, or every occlusion fragments one object into several tracks. On the
37-second window:

```
2. slots that carried >1 identity over the session: 14   <- recycling happens
3. identities that disappeared and came back      : 0
   identities that held more than one slot        : 0    <- never observed
```

Slot recycling was exercised, so the data looked like a real test. It was not:
no object ever left and returned inside that window, so the property was
asserted by the design and demonstrated by nothing. I would have shipped
"identity survives slot loss" as a claim with zero supporting evidence, in a
report that otherwise looked thorough.

Two responses, because they cover different failures:

- A unit test for `ReleaseSlot`, which was new and had none. Eviction was
  covered; voluntary release was not. Eight assertions including the one that
  matters: the returning object resumes its original id *under a different
  slot*, while the slot it used to hold now belongs to something else.
- Capture stride raised from 30 to 60 so the observed window spans 150 seconds
  rather than 37.

On the longer window the property shows up plainly:

```
identities that disappeared and came back: 52
identities that held more than one slot  : 55

  id 26  slots 26,189,192   StaticMeshComponent0
  id 29  slots 29,191,194   StaticMeshComponent0
```

One object, three different pixel values over a session, one identity
throughout. That is the 8-bit channel actually carrying stable identity, rather
than a design document saying it does.

### 7.9 One set doing two jobs

`alreadyMarked_` meant both "queued in the candidate pool" and "currently holds
a slot". Once collection had run, every candidate was "already marked", so the
visibility sweep skipped all 300 candidates it scanned and could never mark
anything. Split into `pooled_` and `alreadyMarked_`.

### 7.10 A GPU fault, not attributed

One run ended with the game exiting early and `nvlddmkm` event 153 plus
`LiveKernelEvent 141` (video engine timeout) in the system log, timestamped
inside the run. Recording it because it happened during our capture and we
submit GPU work.

I did **not** establish that it was ours. The readback rate at the time (60
submissions/second) was identical to earlier runs that completed 9,300
submissions without incident, so the obvious suspect does not fit. It has not
recurred across the six runs since. Unattributed, and marked as such rather than
explained away.

### 7.11 The second crash — a data race I had been walking past all project

The game died 80 seconds into a run. The log simply stops, mid-collection:

```
79.859  ue4: [t+60s] array has 331701 slots      <- discovery thread
79.922  ue4: 16459 readable regions mapped       <- marking thread
<nothing>
```

Two threads, 63 milliseconds apart, both rebuilding the committed-memory map
that `IsReadable` binary-searches. The map was a bare global vector rebuilt in
place:

```cpp
void BuildReadableMap() {
    g_readable.clear();            // readers are mid-search here
    ...
    g_readable.push_back(...);     // and it reallocates under them
```

`IsReadable` is called from the game thread on every marking write, from the
discovery thread millions of times per structural scan, and from the render
thread. So this was always a race. It survived the whole project only because
exactly one thread ever refreshed the map, which made the window vanishingly
small. Giving the marking loop its own `CollectCandidates` (§7.7) added a second
writer *and* a continuous stream of game-thread readers, and it started killing
the process within 80 seconds.

Worth being precise about the mistake: I did not introduce the bug in §7.7, I
introduced the *conditions that exposed it*. The unsafe publication had been
sitting in the file since the first discovery run, reviewed several times, and I
never looked at it as shared state because only one thread happened to write it.

Fix: publish the map as an immutable snapshot behind a `shared_mutex`. The
builder fills a fresh vector off to the side and swaps it in under an exclusive
lock; readers take a shared lock, copy the `shared_ptr`, and search that. A
reader can therefore never observe a partially built map, and the map it is
searching cannot be freed underneath it. The duplicate collection pass on the
discovery thread was removed as well — two full walks of 350,000 slots were
producing the same pool.

Verified with a clean 320-second run: no crash, 76 masks, 393,415 visibility
tests, 0 evictions.

### 7.12 The shutter fired at the wrong moment, a third time

The recording gate started on the first mask containing *any* non-zero pixel.
That is the instant the first primitive is marked — and the working set then
takes another 30–60 seconds to fill 255 slots. With a dense capture stride the
whole budget was spent inside that ramp:

```
frame 4893:  11 objects,  29.3% of pixels labelled     <- what the demo showed
40s later:   62 objects,  87.0%                        <- same build, same level
```

Nothing was broken. The capture was a correct recording of the first ten seconds
of a process that needed a minute, exactly like §7.7 and exactly like the
earlier "masks dumped at t=3.1s, marking at t=41.2s" failure. Three instances of
one mistake: **a trigger condition that is technically satisfied long before the
thing it is meant to detect is actually happening.**

Fix: the gate now requires ≥24 distinct ids, not ≥1 pixel. "Content" should mean
a useful mask, not a non-empty one.

### 7.13 A budget that was silently half what it said

Asking for 150 captures produced 76 masks. Asking for 200 produced exactly 101.
Both looked like plausible numbers — some frames get skipped, recording starts
late, the run ends early. All of those are true and none of them was the cause.

`masksDumped_` was incremented in two places: once in the recording gate and
again after writing the sidecar. Every capture counted twice, so every budget
was halved.

It survived a dozen runs because the output was never *obviously* wrong — 76 is
a believable number of frames. It was caught by asking for a round 200 and
getting exactly 100, which is the kind of coincidence worth being suspicious of.

Worth noting what did **not** catch it: every capture run logged its mask count,
and I read that count many times. A number being plausible is not the same as a
number being checked, and a duplicated `++` produces plausible numbers forever.

### 7.14 Building a correctness check, and having it fail for the wrong reason

Every validation in this project proved the labels were *consistent*: 0 slot
ambiguity, 0 unbound ids, byte-identical round-trips, identity surviving slot
loss. None proved slot 71's pixels are the object the sidecar names. A bug
writing a consistent-but-wrong stencil value would pass all of it.

`tools/verify_labels.py` is the check that can fail in that case. Three tests a
wrong label breaks and a right one does not: a stableId must always name the
same object; a real object projects to a mostly-connected region; and across
0.1s an object should move roughly as much as the rest of the scene, because
that is the camera.

**The first run failed, and both failures were mine.**

*Failure one: "13,088 pixels in 13,088 pieces."* Every pixel its own connected
component. Not fragmentation — a **stipple**. UE4 renders dithered LOD
transitions and dithered opacity as a screen-door pattern, and CustomDepth
inherits it, so those objects arrive as a checkerboard. Counting raw components
called a perfectly well-formed label "completely fragmented". A 3×3
morphological close makes a stipple read as the one object it is; the raw count
is kept and reported separately, because dithering is a genuine constraint of
the CustomDepth route worth naming rather than silently repairing. Measured at
**1.0% of regions**.

*Failure two: an object "moving 156× the scene".* Slot 47, `StaticMeshComponent0`
— and its bounding box was the **entire frame**, in 4–5 pieces. A modular mesh
spread across the scene. As the camera panned, different pieces dominated and
the whole-region centroid slid across the screen far faster than any individual
piece moved. I was measuring the instability of my own statistic. Fixed by
tracking the largest blob's centroid and skipping regions that are not
predominantly one blob.

Even after that, three transitions were flagged — all of them real, none of them
explained. **The action log settled it in one query:** right stick at ±16000,
full deflection, the patrol's turn-in-place step. Under a fast yaw perspective
sweeps objects near the screen edges many times further in pixels than objects
near the centre, so a large ratio there is geometry.

The input recording was added for world-model training and paid for itself first
as a debugging instrument. The validator now reports the stick deflection
alongside every flagged transition, so this question answers itself next time.

**Final:** 344 identities, none ever changed what they named; median region is
100% one blob; 3 flagged transitions in 3,068 (0.098%), 2 explained by camera
rotation, **1 unexplained (0.03%)** — reported as unexplained rather than
rounded away.

Stated limit, because it matters: these checks are **necessary, not sufficient**.
A label wrong in a spatially and temporally coherent way — two objects that
always move together and are always adjacent — would still pass. True ground
truth needs intervention: unmark one primitive and confirm exactly its pixels
vanish. That is a live-run test and is not built.

### 7.15 Closing the loop: the pad had no idea whether it was moving

The patrol was nine hardcoded steps on a cycle. Open loop. It did not know where
it was, whether it had moved, or that it had been walking into the same wall for
four minutes. That was enough to prove the capture pipeline and it is the wrong
thing for a dataset collector: an unattended run that wedges in a corner after
ninety seconds produces thousands of near-identical frames, which is worse than
producing none, because it looks like data.

What made closing it cheap is that the sensor already existed. The segmentation
work had built `GUObjectArray` traversal, `FName` resolution and property lookup
by name; asking those the question "where is the player" is the same machinery
pointed somewhere else. Resolved entirely by name, no hardcoded offsets:

```
perception: Pawn +0x258  AcknowledgedPawn +0x2A8  RootComponent +0x138
perception: RelativeLocation +0x11C  RelativeRotation +0x128
```

"Am I stuck?" from a screenshot is a hard problem against a game whose temporal
AA changes 12% of pixels between two frames of a *static* scene. Given the pawn
transform it is a subtraction.

**Two things worth recording about the build.**

*The shared block is a seqlock, not a plain struct.* The writer is the game
thread, the reader is another process, and there is no lock between them. A torn
read would hand the agent a position that never existed — half of last frame's
coordinates and half of this frame's — which as a stuck-detector input is worse
than no reading at all, because it looks like motion.

*The first run silently did nothing.* vpad opened the shared section once at
startup, failed, and fell back to open loop for the entire run:

```
t+7.0s   vpad starts, OpenFileMapping fails
t+49.9s  segcap: publishing agent state
```

Perception cannot publish until ProcessEvent is verified and the engine layer
has resolved. The agent announced the fallback exactly once, forty seconds
before the thing it wanted appeared. Fixed by retrying the open every 2s and
attaching mid-run — and it is the same shape as every timing bug in §7: a check
performed once, at the wrong moment, reporting a true answer that stopped being
true immediately afterwards.

**Result.** 47 recoveries in a 260-second run, and the positions show real
traversal rather than circling:

```
(-1682, 4432) -> (-771, 4210) -> (606, 4064) -> (1722, 4492) -> (8, 3376)
```

Distinct objects encountered per session went from **~450 to 1,317** — roughly
2.9x — because the agent covers the level instead of one corridor. Within the
captured 30-second window the gain is smaller (344 to 451), which is the capture
budget, not the agent: the run explores for 260s and only 30s of it is
photographed.

Label quality is unchanged by any of it: 0 identities renamed, 0 objects moving
against the scene, median region still 0.997 of one blob.

### 7.16 My screenshots were cropping the evidence, and I read that as absence

**Symptom.** inZOI's character would not move under any input. Camera worked,
character did not. I inspected screenshot after screenshot, checked all four
corners for game-speed controls, found none, and concluded the game was sitting
in an edit view with no simulation running.

**What Frank did.** Sent his own screenshot, containing a whole HUD strip I had
never seen: a clock reading `7:03 AM`, a transport bar with pause / play / ×2 /
×3 / ×4, a Zoi portrait panel, and a money counter.

> *"bro this is full screenshots. yours not complete, why?"*

**Cause.** His capture was 1990×1244. Mine reported the window as 1463×914 — and
the window is really **2560×1600**. The display runs at 175% scaling and my
PowerShell capture process was **DPI-unaware**, so Windows handed it virtualised
coordinates. `GetWindowRect` and `CopyFromScreen` disagreed about what a pixel
was, and the result was a silently cropped image.

Not a corrupt image. Not an error. A perfectly good-looking screenshot of 57% of
the window, every single time, with the bottom strip — where the time controls
live — outside the frame.

**Fix.** `SetProcessDPIAware()` before any window or graphics call. The window
immediately reported 2560×1600, the missing HUD appeared, and with it a control
line I had never seen at all: `R — Toggle Top View / Shoulder View`.

**Why this belongs here.** It is §2.1, §2.3, §7.3 and §7.12 again, in a new
place: *the instrument could not observe the thing it was being used to judge,
and its silence read as a finding.* I spent several exchanges reasoning about why
a life sim would have no time controls, when the honest answer was that I had
never seen the bottom of the screen.

Generalises past DPI: **when a capture is the evidence, verify the capture covers
what you think it covers.** One comparison against a known-good screenshot would
have caught it instantly — and the person holding a known-good screenshot was
sitting right there.

**What it unblocked.** With the full HUD visible, the rest fell out in three
steps:

```
Y (radial)         reveals a legend the HUD does not otherwise show:
                     D-pad L/R   Change Game Speed
                     LT / RT     Switch Zoi
click ▶ transport  the sim was PAUSED. The d-pad only enters widget
                   navigation and cannot operate the transport bar, so
                   this genuinely needs a mouse click.
L stick            the Zoi walks.
```

Result: `Street (Editable)` → `North Beach (Editable)`, clock 7:04 → 7:21, Zoi
mid-stride. Character control on inZOI, driven entirely by automation.

---

## 8. inZOI (UE5): four wrong diagnoses of one bug

This is the longest chain of wrong answers in the project, and every one of them
was plausible. Worth reading as a sequence rather than as five separate entries,
because the interesting part is why each wrong answer survived as long as it did.

The symptom: on inZOI, marking worked perfectly (255 live slots, thousands of
identities, indefinitely stable) but the readback either produced masks with
nothing of ours in them or killed the game at ~45s.

### 8.1 "It's Enhanced Barriers" — wrong twice, for opposite reasons

The elected target showed zero observed `ResourceBarrier` transitions, so any
`StateBefore` we declared would be invented, and the guard refused to copy.
D3D12 has a second barrier API (`ID3D12GraphicsCommandList7::Barrier`); a game
using it never calls the old one. That fits perfectly.

I dismissed it first because the guard did not fire — it was evaluating a
different elected target. Then I revived it because the guard did fire — but
that message printed whenever `shadowUsable` was false, and I had folded
`armReady` into `shadowUsable`, so a run that was merely *waiting to be armed*
reported "ZERO ResourceBarrier transitions observed". It was evidence of
nothing. **A diagnostic that cannot separate two causes will always name the one
you were already expecting.**

Settled by hooking the API and counting: inZOI calls `Barrier()` **zero** times,
while a legacy target showed **7,340** transitions. Hypothesis dead, by
measurement rather than by argument.

### 8.2 "We elected a 128x128 shadow map" — true, but a symptom

Every DSV inZOI creates is 1280x800, 512x512 or 128x128; across 813 `CreateDSV`
calls there is no 2560x1600 one, because inZOI renders at half resolution and
upscales. The log showed `rejected: 128x128 != scene 2560x1600` — and yet a
128x128 target had been *elected*, scoring 260.

That contradiction was the real clue and I nearly filed it as a size-filter bug.
A target cannot be both rejected by a hard requirement and elected. The only way
both lines are true is if they describe **different resources at the same
address at different times**.

### 8.3 The actual cause: address identity

`ID3D12Resource*` is not an identity. D3D12 hands the same address back for a
different resource the moment the old one is released, and UE5's pooled
render-target allocator does it constantly — the run that found this logged
**700 address recycles in 210 seconds**.

`SnapshotTargets` merged only per-frame counters into accumulated evidence:

```cpp
acc.bindCount = fresh.bindCount;          // refreshed
acc.clearCount = fresh.clearCount;        // refreshed
// acc.format, acc.width, acc.height      <- never refreshed
```

So format and dimensions were fixed at first sighting forever. A dead
depth-stencil's identity stayed welded to its address while the new occupant's
binds piled on top of it. The 600-frame staleness prune — written specifically
against "a destroyed buffer keeps winning on accumulated persistence" — could
never fire, because the address kept being bound by its *new* owner, so
`lastSeenFrame` kept refreshing.

Everything falls out of this one fact:

| Symptom | Explanation |
|---|---|
| Zero barriers on the elected target | Nothing transitioned it; the thing we scored was gone |
| D3D12 error 527, StateBefore mismatch | Shadow state belonged to the previous occupant |
| Readback crash at ~45s | Copying from a resource that was not what we thought |
| `could not create readback buffer` | Sizing a stencil plane on an `R8G8B8A8_TYPELESS` |
| A target elected 204 times | It reported `R10G10B10A2_UNORM` by end of session |

The fix needs no `AddRef` and no dangling read. The fresh fingerprint comes from
a live `GetDesc` in `NoteBind`, so it is always the truth about whatever occupies
the address *now*. When it disagrees with the accumulated evidence on format or
dimensions — properties that cannot change without destroy/recreate — the
previous occupant is gone: discard the evidence, purge the shadow state and
barrier counts, void the election if it named that address.

**This is the same bug we had already fixed on the UObject side**, where a
generational handle `(objectIndex, serialNumber)` guards a stored pointer against
GC slot reuse. I wrote that guard, understood exactly why it was needed, and did
not think to ask whether the graphics API recycled handles the same way. The
lesson is not "check D3D12 pointers"; it is that a class of bug fixed in one
layer is worth re-asking in every other layer that stores a raw handle.

### 8.4 The log line that solved it

One diagnostic broke the deadlock: printing the elected target's **actual**
`GetDesc` at election time, next to what the election believed about it.

```
elected target PINNED 0000016D8B7105D0 512x512 fmt=DXGI_FMT_27
```

Format 27 is `R8G8B8A8_TYPELESS`. A target that had passed a hard
"must have a stencil plane" requirement was, in reality, not a depth buffer at
all. There is no way to argue with that line, and every competing hypothesis died
the moment it appeared. Before it, the log described the *model*; after it, the
log described the *resource*.

### 8.5 The capture budget was spent on the main menu

Separately: the first run after the fix reached gameplay perfectly and produced
61 masks — **all of the main menu**. The dump budget is a fixed frame count, and
the menu satisfied the recording gate at t=49s, so the budget was gone by t=65s,
two minutes before the world finished loading.

The recording gate was working exactly as designed; the design was wrong. Fixed
by holding the readback disarmed and having `run_inzoi_play.ps1` arm it itself,
25s after it clicks play. The script is the only thing that knows where in the
route it is, and asking a human to drop the arm file is precisely the manual
step this harness exists to remove.

### 8.6 Result: the fix was real, the win was not

The address-identity fix is correct and Stray improved measurably with it (94
masks against 21-31 before). But the inZOI result reported alongside it was
wrong, and it is worth recording exactly how a wrong result got signed off.

`verify_labels.py` returned PASS: 508 identities, none ever changing what they
name, no object drifting against scene motion. I reported that as inZOI working.
Then a follow-up probe -- built to characterise what looked like dithering --
measured 99.87% of the buffer labelled, every one of the 255 possible byte values
present, per-slot areas all within a factor of two of each other, and centroids
collapsed into 0.073 of the image diagonal.

Rendering the mask settled it in one look: **static**. No object shapes at all,
just faint horizontal banding. The inZOI masks are noise.

The PASS was structurally incapable of failing here:

  * identity consistency reads names from the SIDECAR, not from the pixels, so
    it is true no matter what the pixels contain
  * temporal coherence tracks region centroids, and the centroid of a uniform
    random scatter sits in the middle of its area and does not move -- so noise
    is maximally "coherent"

The file says so itself, in its own output: *"These checks are NECESSARY, not
sufficient."* I read that line, quoted it to the user, and still treated the PASS
as confirmation.

What the readback is NOT doing wrong: `GetCopyableFootprints` resolves plane 1 as
`R8_TYPELESS`, pitch 1536 for width 1280 -- the correct stencil plane, one byte
per pixel. The plane selection, the pitch and the dimensions are all right. The
bytes are simply not a per-object stencil.

The leading explanation is the one the address-recycling work already pointed at:
UE5 allocates render targets from pooled heaps, and transient resources **alias**
each other's memory within a frame. Copying at Present means copying long after
the CustomDepth pass ended, by which time that memory can have been handed to a
different pass. Correct plane, correct resource, wrong moment.

That makes the next job a change of timing rather than of target: issue the copy
at the point in the command stream where the CustomDepth pass ends -- observable
as the transition of that target out of `DEPTH_WRITE` -- instead of at Present.

Two tools now exist so this class of failure cannot be signed off again:
`tools/mask_sanity.py` (area distribution, centroid spread, id count) and
`tools/dither_probe.py` / `dither_phase.py` (lattice occupancy and phase). The
first version of `mask_sanity.py` flagged "coverage ~100%" as proof of noise and
**the known-good Stray control failed it** -- full coverage is normal, because
every pixel legitimately carries a stencil value. Running a new criterion against
a title known to work is what caught it, and is now the rule.

### 8.7 A crash with none of our code on the stack

The next inZOI run died 98 seconds in, and the game wrote the report itself:

```
LowLevelFatalError [D3D12Util.cpp:1136]
  hr failed at D3D12CommandList.cpp:277 with error E_INVALIDARG
CrashType = Assert
```

`segcap.dll` appears nowhere on the callstack. The instinct that follows -- "not
us, then" -- is wrong, and understanding why took the whole investigation.

`D3D12CommandList.cpp:277` had to be identified without engine source. Parsing
the shipping exe's `.pdata` exception directory gives function bounds; the
RIP-relative cross-reference to the `D3D12CommandList.cpp` string literal picks
the function; disassembling it finds `call qword ptr [rax+0x48]` at RVA
`0x3A495E3`. Offset 0x48 is vtable slot 9 of `ID3D12GraphicsCommandList`, which
is **`Close()`**. Four `.pdata` frames corroborate.

That single fact inverts the reading. D3D12's recording methods --
`CopyTextureRegion`, `ResourceBarrier` -- return `void`. An argument the runtime
rejects is not reported where it is made: it is **latched into the command list**
and surfaces later, from `Close()`, as `E_INVALIDARG`, on whichever thread the
game happens to call `Close()` on. So a crash site with none of our frames on it
is exactly what an invalid command recorded by us would look like. Deferred
error reporting means the stack names the messenger.

The minidump then puts it on our list specifically. Across 1.5 MB of captured
memory there are exactly three `0x80070057` dwords: two on the crashing thread's
stack, and one at `0x25C8CC9FC70` -- the D3D12Core command-list object at
`0x25C8CC9FBE0`, plus 0x90. The last line our log wrote before dying names that
list: `inject: FIRST copy recorded into the game's own command list
0000025C8CC9FBE0`.

Three theories were generated for what made the copy invalid. Two were refuted
under adversarial review, including the leading one -- a stale cached footprint
in `readback.cpp:186`, killed by the observation that the code revalidates every
frame. `StateBefore=0x40` was ruled out on principle rather than evidence:
resource-state correctness is checked by the debug layer, not by the core runtime
at record time, so a wrong `StateBefore` never reaches `Close()` at all.

**Nothing survived as a cause.** After three refuted guesses the correct move is
to stop guessing and measure -- and the measurement was blocked by our own code.
The debug layer was off that run, and `DrainInfoQueue()` was called only from the
Present path. The game died *between* our copy and the next Present, so every
validation message describing that copy went into the crash with the process.
There is no next Present after the call that kills you. The drain now also runs
inline at the end of `TryInjectCopy`, on the recording thread.

### 8.8 The instrument was drowning -- but it did not cause the freeze

Turning the debug layer on produced a new failure: the game hung. Not crashed --
hung. The log stopped mid-stream at t=177s, the pid stayed alive at 12.9 GB
resident, and it was still not responding twenty minutes later. No crash report
was written, because nothing crashed.

**I attributed this to the debug layer, and that was wrong.** The attribution is
kept here rather than edited out, because the way it failed is the point: the
message volume below is real, it was worth fixing on its own merits, and it was
sitting in plain view at the moment the freeze appeared. A cause that is true,
large, and *adjacent* is the easiest kind to accept without testing. The control
run -- same build, debug layer OFF -- froze the same way at t=156s. See §8.9.

The volume problem, then, on its own terms. 1.42 **million** validation messages in 177 seconds, of
which 1.4M were warnings the game generates about itself:

| id | count | message |
|----|-------|---------|
| 1008 | 739,726 | ResourceBarrier called on the same subresource in separate Barrier Descs |
| 1424 | 620,704 | waiting for a fence value of zero |
| 926 | 40,610 | |
| 527 | 7,137 | ERROR: before state does not match preceding ResourceBarrier |

`DrainInfoQueue` already declined to *log* warnings -- but declining to log is not
declining to **store**. The runtime formatted and queued all 1.4M, and the queue
was then walked from the Present thread and, once copies began, from UE's parallel
translate threads as well. Fixed with `PushStorageFilter`, keeping only ERROR and
CORRUPTION. An invalid `CopyTextureRegion` cannot be a warning -- it is precisely
what gets latched and reported from `Close()` -- so nothing diagnostic is lost.

Two things that run gave away for free, both worth more than the hang cost:

**The `ERROR [id 527]` barrier-state mismatches are the game's own.** 7,137 of
them were already logged at t=23.6s, at a moment when the counters read
`inject: attempts=0 recorded=0` -- segcap had issued no GPU work whatsoever.
inZOI ships with resource-state violations the runtime tolerates. That means the
debug layer is not a clean reference on this title: any error must be attributed
by resource and command-list address, never by "this looks new".

**82 copies recorded, no crash.** The 11:46 run died on its *first* copy. So the
crash is not deterministic on recording a copy, which quietly kills the whole
family of "our copy is malformed" explanations that assume a fixed defect in what
we record.

Two harness defects fell out of it as well. The script checked
`Get-Process` before each step, so a frozen game passed every check -- it clicked
transport-play on a dead window and then drove it with gamepad input for two more
minutes before reporting `0 masks`, which reads exactly like a capture bug rather
than the hang it was. `Responding` is the actual question and is now asked. And
`archive_capture.ps1` returned early when a run produced no images, *before* it
copied the log -- so this 410 MB of validation evidence, the entire product of a
diagnosis run, would have been deleted by the next run's cleanup.

### 8.9 Not a hang: the process was suspended

The control run -- debug layer off -- froze at t=156s, which killed the §8.8
explanation. The new `Responding` check caught it, and this time the frozen
process was still there to interrogate rather than already killed.

Sampling it turned "hung" into something much more specific:

  * 146 of 147 threads in `Wait` with `WaitReason = Suspended`
  * suspend count exactly **1** on every one of them
  * CPU flat at 330.2 seconds across a 12-second window -- **zero** cycles

Zero CPU rules out a spin. A suspend count rules out a lock: a deadlocked thread
is blocked, not suspended. Exactly-1-on-all-threads is the signature of a single
whole-process suspend that was never matched by a resume.

The decisive test was to undo it. Calling `ResumeThread` once per thread brought
the game back -- `Responding = True`, and the log jumped from t=156 to t=556 and
kept streaming. So: recoverable, and genuinely a suspension rather than damage.

What that rules out, in order of how much I wanted each to be the answer:

  * **MinHook.** It suspends every thread in the process to patch code, which
    matches the signature exactly, and I said so before checking. Every `MH_*`
    call in segcap is reachable only from `Install()` at startup -- `CreateHook`
    has thirteen call sites and all thirteen are inside `AcquireVTables()` or
    `AcquireSwapChainVTable()`. Nothing calls into MinHook at t=156. A mechanism
    that fits the evidence perfectly is not thereby the mechanism that ran.
  * **The D3D12 debug layer** -- reproduced with it off.
  * **Windows Error Reporting** -- no WerFault process, no Application event.
  * **UE's own crash handler** -- no `CrashReportClient`, no new `UECC-*` dir.

**What suspends it is still unknown.** The harness now detects the state (zero CPU
advance plus a majority of threads suspended, which distinguishes it from a
streaming stall, which burns cycles) and resumes it, loudly. That is a workaround
over an unexplained fault and is labelled as one in the code: a run that dies here
wastes twenty minutes of game time for a condition that costs milliseconds to
undo, but a *silent* recovery would erase the fault from the record.

### 8.10 The cause: a lock that was taken on one side only

`Readback::RecordInto` reads `footprint_`, `planeSlice_` and `slots_[].buffer`
under `foreignMutex_`. `Readback::Prepare` **rewrites all three, and calls
`ReleaseResources()` five times, holding nothing.**

For the injection ring those two run on different threads by design: `Prepare`
from `OnPresent`, `RecordInto` from whichever of UE's parallel translate threads
is recording the game's command list. So the Present thread can free a slot's
destination buffer and install a new footprint while a translate thread is
partway through recording a copy that uses them. The result is a
`CopyTextureRegion` whose placed footprint does not describe its destination.
D3D12 records that silently, latches the rejection into the command list, and the
GAME's `Close()` returns `E_INVALIDARG` -- §8.7's crash, with none of our code on
the stack, exactly as deferred error reporting predicts.

The timing lines up with both observed crashes. `Prepare` only rebuilds when the
layout key changes, and the layout key changes when the id-buffer probe switches
candidates. The 11:46 crash landed on the *first* copy, at arming, when the ring
was first built; the 13:40 crash landed seconds after the probe moved from
`R32_UINT` to `R16G16_UINT`. The runs that survived 83 and 128 copies were the
ones that sat on a single candidate and never rebuilt.

Why four earlier diagnoses missed it, which is the useful part:

  * **The evidence pointed at the copy's arguments, and it was right.** Every
    theory tried to find a wrong *value* -- a stale footprint, a bad
    `StateBefore`, the wrong subresource. The values were all computed correctly.
    They were correct at the moment they were computed and stale by the time they
    were used. "Is this argument right?" and "is this argument still right?" read
    identically in a code review.
  * **A refutation was accepted slightly too broadly.** The stale-footprint theory
    was killed with "the code revalidates every frame", which is true of the
    Present-path `readback_` and says nothing about `maskInjectRing_`. Two
    instances of one class, and the refutation checked the wrong one.
  * **The debug layer could not see it.** With validation on, the crash stopped
    reproducing -- the layer's overhead changes thread timing, and the race needs
    the two threads to land inside a window of a few instructions. I read "zero
    errors attributable to segcap across 128 copies" as evidence the copies were
    fine. It was evidence the race had not fired.

The fix is one `lock_guard` in `Prepare`, plus moving `RecordInto`'s
`readyForLayout_` check inside the lock -- it was read unlocked, so a thread could
pass it and then walk into buffers `Prepare` had already freed. The lock is
deliberately NOT taken inside `ReleaseResources()`: `Prepare` calls it five times
and the mutex is non-recursive, so locking in both would self-deadlock on the
first rebuild.

### 8.11 A threshold measured on the easy case

With the crash fixed the probe still had to find the id buffer, and it was failing
for a reason that had nothing to do with D3D12.

Its acceptance test requires >=6 distinct leased values present and no single
value carrying more than 60% of the match -- guards added after an earlier false
positive where values 1, 2 and 3 covered 58.7% of the screen. Sound guards. But
the probe was running them at t=46.6s with **8 slots leased**, and at 8 marked
objects those conditions cannot be satisfied by any buffer, correct or not: eight
objects genuinely are few distinct values with one dominant region. It measured
100% of non-zero texels carrying our slots in channel G16 of the R16G16_UINT
Nanite `CombinedCustomStencil` -- the right target, the right channel -- and
reported "one big region, not a set of object ids".

The guard was not wrong. It was being asked a question the sample could not
answer, and it answered "no" instead of "not yet".

Holding off until 32 slots made it converge immediately... in the main menu, and
never again. The world transition destroys every marked component (`106
dropped-destroyed`) and gameplay runs 23-31 live slots -- **below the threshold I
had read off the menu**. A number taken from the case that was easy to observe
became "never" in the case that mattered. It is now 16, derived from the guard it
protects (twice `kMinDistinctLeasedSeen`) rather than from one observation.

Related: a "matched, but the sample is too small" verdict no longer burns one of
the candidate's attempts. It had been counting toward permanent rejection, so the
correct buffer could be blacklisted for the rest of the session on the strength of
looking at it too early.

### 8.12 The same guard, failing the other way

§8.11 ends confidently: the acceptance test was being asked a question the sample
could not answer, the hold-off at 16 leased slots fixes it. That was true and it
was not enough.

A run today, with **155 slots leased** -- ten times the hold-off, nothing like a
small sample -- produced this eleven times in a row and then zero masks:

```
idbuf: 0000026F8A9B5090 channel G16(high) matched 100.0% but 85% of the
matching texels are a single value -- that is one big region, not a set of
object ids; not accepting
```

100.0%. Every non-zero texel in that buffer carried a stencil value we currently
leased. It is the correct target and the probe rejected it, because of the
dominance guard rather than the distinctness one this time.

The dominant value is inZOI's apartment shell. Floor, walls and ceiling are a
single `StaticMeshComponent0`, and indoors it legitimately covers 85% of the
labelled pixels. The same object is the one that takes ~56% of a normal outdoor
frame and that the README lists as a cosmetic wart.

**Dominance is not a property of the buffer. It is a property of the scene.**
"No single value carries more than 60% of the match" reads like a statement about
whether the data looks like object ids, and it is actually a statement about
whether the room you are standing in has one big object in it. There is no
threshold that separates those, because they are not the same question.

What makes it worse is that the guard has never earned its place. Both decoys on
record -- the `R8_UINT` `{0,1,2}` flag buffer, and the earlier false positive
where values 1, 2 and 3 covered 58.7% of the screen -- have **three** distinct
values, and are rejected by the distinctness test alone. Dominance has never been
the condition that caught anything. It has now been the condition that rejected
the answer twice.

The comment above the guard names three conditions. Only two were ever
implemented, and the missing third is the one that does the real work:

```
//   - enough distinct leased values actually present, ...
//   - no single value dominating the "match", ...
//   - a decent share of the leased set observed, not just its head   <- absent
```

So distinctness now scales with what is actually leased -- an eighth of the
leased set, floor of 6 -- and dominance is reported rather than enforced. At 155
slots that demands 19 distinct leased values instead of 6, which is a strictly
harder test than the one it replaces, and one a decoy cannot pass however its
pixels are distributed.

The tempting fix was to keep dominance and add the share test as a third
condition. That would have left `notDominated` implied by `enoughDistinct` -- a
condition that can never independently fail, which is the exact shape of
`(state & D3D12_RESOURCE_STATE_PRESENT) == PRESENT` in §8.7 and of the unbounded
"wait for better evidence" in §8.6. Two vacuous guards is a pattern; three would
be a habit. One test decides, the other is logged as evidence.

### 8.13 The proof that could not run

Separately, the ground-truth intervention -- unmark exactly one slot, require
exactly its pixels to vanish -- turns out to have been unable to reach a verdict
on inZOI at all, for a reason that is entirely self-inflicted.

Both counters tick on the same event, once per completed mask. The verdict waits
**30**. The coverage evictor fires at **8**:

```cpp
if (marked_.size() >= IdentityRegistry::kSlotCount - 8 &&
    tinyStreak_[mp.stencilValue] >= 8) { ...release the slot... }
```

Unmarking the target drives its slot to zero pixels, which is precisely the
signal `tinyStreak_` counts. The test's own success condition is what triggers
the mechanism that destroys the test: at tick 8 the slot is reclaimed and
reissued to another object, and at tick 30 the verdict correctly reports
INCONCLUSIVE because a different component is wearing the number.

It never showed up on Stray because that gate also requires
`marked_.size() >= 247`, and Stray's alleys never fill the pool. **The check was
built and tuned on the one title where its confound is unreachable.**

The fix pins the slot under measurement against that eviction and against that
one only. Pinning it against all three release paths was the obvious move and
would have been a silent disaster: the stale-object drop and the not-rendered
release are what stop this test reporting a false PASS when the object is
destroyed or simply walks off camera. Their removing it from `marked_` is what
makes the verdict's "is the slot still held by the component we unmarked?" check
fire. Pin everything and every camera cut becomes proof.

### 8.14 The verdict it reached instead

With the pin in, the test ran on inZOI for the first time and returned **FAIL --
the unmarked object's pixels are STILL THERE, so the slot does not mean what the
sidecar says**. That would be the worst result this project could produce, if the
line immediately above it were not this:

```
groundtruth: selected slot 64 with 23645 px (2.31% of frame). Requesting unmark.
groundtruth: slot 64 is not held by any marked primitive
groundtruth RESULT: slot 64 went 23645 -> 23533 px (0.5% removed) ... FAIL
```

Nothing was unmarked. The experiment had no independent variable, and the test
graded it anyway and blamed the labels.

Two defects compose to produce that:

**It chose a slot it could not act on.** Selection scanned the mask for the
largest id. Slot 64 was legitimately in the buffer with 23,645 px while no live
marked primitive held it -- not a contradiction: when a component is destroyed,
`RefreshVisibility` drops the entry *without* clearing the flag, because calling
into a freed component is precisely the crash that path exists to avoid. Its
pixels keep arriving from a render proxy we no longer track. Eligibility now
requires that we actually hold the slot.

**It could not tell "ran" from "requested".** The unmark is scheduled onto the
game thread and returns the component it cleared, or `nullptr`. That return was
discarded. The guard that should have caught it compares the slot's current
holder against the component recorded at selection -- and both were `nullptr`, so
two absences compared equal and the run was graded. A pin set only on the path
that really clears the flag now separates them, and an aborted selection
re-selects instead of producing a verdict.

The shape is worth naming, because it is the third variant of one thing in this
file: **a check that cannot distinguish "it did not happen" from "it happened and
produced nothing."** §8.6 had a mask passing a structural test while being noise;
§2 has counters reporting effort as result; this reports a null experiment as a
failed one. The correct output was never FAIL or PASS. It was "this did not run."

### 8.15 What it said once it could run

```
groundtruth: selected slot 102 with 161787 px (15.80% of frame);
             everything else holds 44351 px. Requesting unmark.
groundtruth: UNMARKED slot 102 (InstancedStaticMeshComponent)
groundtruth RESULT: slot 102 went 161787 -> 0 px (100.0% removed);
             all other slots 44351 -> 46370 px (grew 4.6%).
             VERDICT: PASS -- exactly the unmarked object's pixels disappeared
```

One primitive's flag cleared through the engine's own setter; 15.8% of the frame
went to **exactly zero**, and nothing else lost anything. The other slots *grew*
4.6%, which is not noise tolerated by the threshold but the mechanism itself:
the unmarked surface stopped occluding the geometry behind it, so objects that
were already marked became visible. An earlier version of this test required
other slots to stay within ±50% and returned INCONCLUSIVE on a perfect result
for exactly that reason -- the symmetric band punished the case that proves the
claim. It is one-sided now: growth is expected, loss is not.

**Replicated on a second run**, on a different object and at four times the
scale:

```
groundtruth: selected slot 1 with 696837 px (68.05% of frame);
             everything else holds 327163 px. Requesting unmark.
groundtruth: UNMARKED slot 1 (StaticMeshComponent)
groundtruth RESULT: slot 1 went 696837 -> 0 px (100.0% removed);
             all other slots 327163 -> 326971 px (0.1%).  VERDICT: PASS
```

Slot 1 is the sky sphere, and it behaves exactly as the mechanism predicts in
the *other* direction: the sky is behind everything, so removing it reveals
nothing, and the other slots stay flat instead of growing. Two runs, two
objects, 15.8% and 68.1% of the frame, both to exactly zero, with the
side-effect on everything else going the way the geometry says it should each
time. That is replication rather than one lucky measurement.

Two honest limits. The settle window is about half a second, which is short --
but the failure direction is the safe one: too little time shows the pixels
*still there*, a false FAIL, never a false PASS. And this is one slot per run,
not a survey. It does not prove every label is right; it proves the mechanism is
real, which is what nothing else here could establish.

This is what separates the inZOI result from §8.6, where a mask passed a
structural check while containing another buffer's bytes entirely. Every other
test in this project reads the buffer and asks whether it looks like ids.
This one changes the world and requires the buffer to follow.

### 8.16 Counting the crashes instead of describing them

The write-up has said for days that inZOI has "two crash families", `E_INVALIDARG`
from the game's own `Close()` and `E_ABORT` from `ResizeBuffers`, phrased as a
pair. Reading twelve consecutive crash reports instead of the two I happened to
remember:

```
15:15  E_ABORT ResizeBuffers (D3D12Viewport.cpp:554, 0x80004004)
15:07  E_ABORT ResizeBuffers
14:57  E_ABORT ResizeBuffers
10:25  E_ABORT ResizeBuffers
10:17  EXCEPTION_ACCESS_VIOLATION reading 0x18
08:21  E_ABORT ResizeBuffers
08:05  EXCEPTION_ACCESS_VIOLATION reading 0x430
07:41  Close() failure (D3D12CommandList.cpp:277)
07:20  E_ABORT ResizeBuffers
07:19  E_ABORT ResizeBuffers
07:11  E_ABORT ResizeBuffers
07:09  E_ABORT ResizeBuffers
```

Nine of twelve are one family. `Close()` -- the one I had been treating as
co-equal and had spent the most time on -- appears **once**. There is also a
third family the write-up never mentioned at all, two access violations. "Two
families" was an accurate description of the two reports I had read and a wrong
description of the population, and effort had been allocated on that basis.

What the dominant one says, read carefully:

```
Result failed at D3D12Viewport.cpp:554 with error 80004004
Viewport=0x..., Num=3, Size=(2560,1600), PF=18, DXGIFormat=0x18,
Fullscreen=0, AllowTearing=1
```

It is resizing to **the size it is already at**. That is not a window resize; it
is a mode or format transition. The callstack is thirteen `inZOI_Win64_Shipping`
frames then `kernel32`/`ntdll`, with segcap absent -- though §8.7 is the standing
warning that absence from a stack does not clear us when D3D12 defers its errors.

Two candidate causes eliminated cheaply, both by reading code rather than running
anything:

- *We hold a reference to a backbuffer, which blocks `ResizeBuffers`.* The most
  natural explanation, and already handled: the registration loop calls
  `Release()` immediately and keeps the raw pointer only for comparison.
  `Readback::Prepare` likewise stores `preparedFor_` without an `AddRef`.
- *Our harness triggers it by touching the window.* During the capture hold the
  harness does a process query, a file stat and `sleep(3)` -- no screenshots, no
  focus calls. It is only the menu phase that manipulates the window, and these
  deaths happen ~50s after gameplay begins.

So it remains unexplained, and it is now clearly the item worth the next block of
time rather than `Close()`.

The investigation did turn up a real bug beside it. `backBuffers_[]` is filled
once, under `if (backBufferCount_ == 0)`, and never invalidated. A resize the
game *survives* frees every pointer in it, and then `IsBackBuffer()` quietly
stops matching -- colour capture ends while every counter still reports success
-- or, worse, matches a recycled address belonging to a different resource, which
is precisely the address-identity failure of §8.3. It is now re-registered
whenever the swapchain's own description changes. That is a consequence of the
crash family, not a cause of it, and it is not offered as a fix for it.

### 8.17 A ceiling that was never once met, hidden by its own retry

Every inZOI run logs this, successful ones included:

```
[inzoi] waiting for the world to begin loading (attempt 1) (ceiling 25s)
[inzoi] the world to begin loading (attempt 1) NOT observed within 25s
[inzoi] Continue
[inzoi] first save slot
[inzoi] waiting for the world to begin loading (attempt 2) (ceiling 25s)
[inzoi] the world to begin loading (attempt 2) after 0s
```

I had read that for days as "the first click sometimes misses, the retry fixes
it" -- which is what the code comment says it is for. Measuring the gap between
`render_signal` and the first `world is changing` across all 58 archived runs:

```
min 37.4s   median 43.1s   warm max 46.9s   cold-start max 146.4s
caught by the 25s ceiling: 0 of 58
```

Not "sometimes". **Never.** The signal cannot arrive inside that window, so every
inZOI run ever made has clicked Continue and the save slot, given up, and clicked
both again into a world that was already loading. And `attempt 2 after 0s` is not
the second click succeeding -- it is the *first* click's load finally registering,
about 43s in, immediately visible to the next poll.

The first hypothesis was wrong and worth recording because measuring killed it in
one step. The segcap log is enormous and `tail()` reads a fixed 256 KB, so a
signal written during a census burst can scroll out before anyone polls -- the
worst 2-second window in a run writes **657 KB**, well past the tail. Plausible,
and not what happens: at each `world is changing` line only 0.4-9.3 KB followed
in the next two seconds. The mechanism is real, this instance is not it, and the
tail limit is now a known hazard for any *other* signal logged mid-burst.

**Then the fix failed, and the failure was more interesting than the bug.**

I raised the ceiling to 90s from that measurement and watched the next run report
`NOT observed within 90s`. The number was not the problem.

`world is changing` is what the DLL logs whenever the object count jumps. The
menu world settling produces it; a save loading produces it; garbage collection
produces it. It is **churn, not a load signal**, and I had measured the first
occurrence of it and called that "when the load starts". In the failing run the
menu world settled at t=39 and the save did not actually come up until t=118 --
so the ~43s median I had measured was, in many of those runs, the *menu* world,
not the thing being waited for.

A number derived from the wrong event landed close enough to look reasonable and
still could not work. That is worse than an obviously wrong number, and it is the
same failure as §8.6: a measurement that is coherent, precise, and about
something else.

Measured again on the unambiguous event -- the `UWorld` name changing from
`OpeningLevel2` to `RedCity_Map`, across 22 archived runs that reached a world:

```
min 41.5s   median 69.9s   p90 73.4s   max 74.5s
25s ceiling: 0/22      90s: 22/22
```

The gate now watches that instead, with a 120s ceiling to cover an observed
118.4s outlier from a session where the save was no longer disk-cached. The
detector was already written and already correct -- `wait_for_level_change`, used
twenty lines further down for exactly this reason, and documented there as the
fix for a *previous* bug where marked-component count could not tell menu from
gameplay. The right instrument existed and the gate above it was watching
something else.

One trap avoided in the process: switching the gate to the level name would have
broken Stray, whose profile has no such log line, by making it wait the full
ceiling for a signal that cannot arrive. It falls back to the old path when the
menu world's name is unknown.

Why it matters beyond tidiness: that spurious second click sends Continue and a
save-slot selection into an in-progress world load, which is exactly the sort of
thing that makes an engine tear down and recreate its viewport -- and the
dominant crash here is the game's own `ResizeBuffers` assert (§8.16). That is a
hypothesis, not a result. But it had been invisible, because the retry loop
reported success every time it fired.

**The pattern, for the sixth time in this file:** a threshold nobody remeasured,
sitting where the data never goes. What made this one survive longest is that it
had a fallback. A guard that fails loudly gets fixed; a guard that fails into a
retry that works looks like a guard that works.

### 8.18 `?` is truthy

Chasing the load gate one level further down turned up the thing that was
actually costing runs, and it is one character long.

The DLL prints its state line before the engine layer has resolved a live
`UWorld`:

```
state: BOOT level=? sim=? dilation=-1.00 objects=0 ...
state: MENU level=? sim=? dilation=-1.00 objects=0 ...
state: WORLD level=RedCity_Map sim=RUNNING objects=564553 ...
```

`?` is a sentinel meaning "not known yet". `current_level()` returned it
verbatim:

```python
return ln.split("level=")[1].split()[0]      # -> "?"
```

A one-character string is truthy. So `menu_level` became `"?"`, the log printed
`menu world is '?'` -- **exactly what it prints when the read fails**, so the two
cases were indistinguishable in every log this project has produced -- and the
poll I had just added to "wait until the name exists" exited immediately,
satisfied.

Worse, `wait_for_level_change(was="?")` waits to leave `"?"`. The menu's own
world, `OpeningLevel2`, is not `"?"`. So the wait was satisfied by **the menu
appearing**, and the harness announced `loaded world 'OpeningLevel2' after 10s`
and went on to resume the simulation and arm the capture. In the menu.

How much this cost, counted across the archive:

```
runs that reached CAPTURE ARMED:              42
  of those, never logged a WORLD state line:  20   (48%)
```

Nearly half of every run that got as far as arming was photographing the main
menu, and each one reported reaching gameplay on the way there. This is the same
failure as the screenshot in §7 that showed the harness clicking a loading
screen -- and it survived that fix because the replacement oracle, the level
name, had an "unknown" value that passed for a name.

`current_level` now treats `?` as unknown and returns empty, which is what the
callers already assumed it did.

**The lesson is not "check for the sentinel".** It is that `menu world is '?'`
was printed on every single run, in front of me, for days -- and it was
ambiguous between "the menu's world is called ?" and "I could not read the
world". A log line that renders two different states identically cannot be
evidence for either.

### 8.19 An honest instrument nobody consults

Fixing the detectors in §8.18 changed nothing on its own, twice, and the shape of
that is worth more than either fix.

**First.** With `current_level` returning the truth, a run reported:

```
no stable level change within 120s -- proceeding
world-settled signal not seen within 240s -- proceeding
no stable level change within 240s -- proceeding
ARMED -- every captured frame from here is gameplay
```

Every check failed, said so correctly, and the harness armed and spent 200
seconds photographing the main menu. Each of those waits is a *ceiling* that
proceeds on expiry, which is the right design -- a missed signal should not
deadlock a run -- but nothing downstream read the answer.

**Second, immediately after fixing that.** The pause probe stopped reporting a
false `sim IS running` (§8.18) and started correctly reporting `screen static`.
The next run:

```
inZOI-Win64-Shipping is not running        (x7)
screen static: still paused                (x7)
!! could not resume -- capturing a PAUSED world
ARMED in 'RedCity_Map' -- every captured frame from here is gameplay
holding for 220s
```

The process had already exited. Seven transport clicks into nothing, seven
correct "static" verdicts about a corpse, then a 220-second hold against a dead
pid -- three times in one four-attempt run.

Both fixes were right. Both were useless alone, because **the value of a
measurement is zero until something branches on it.** Every failure in §8.11
through §8.18 is a detector that answered wrongly; these two are detectors that
answered correctly into a void. The second kind is harder to see, because the
log now contains the truth and reads as though someone acted on it.

The rule that falls out: when you fix an instrument, grep for its callers before
claiming the fix. If the answer is only ever logged, it is documentation, not
control flow.

### 8.20 What the crash data says now, and what it does not

Four consecutive attempts failed after the harness work, which looks like a
regression and is not one. The claim is checkable rather than assertable: the DLL
has not been rebuilt since `9014787`, and the next run after that commit captured
401 masks and produced the second ground-truth PASS. Everything since is Python
in the harness. The code that touches the game is byte-identical to a known-good
run.

What the four deaths were:

```
t+27s  AccessViolation
t+77s  E_ABORT/ResizeBuffers
t+52s  Close()/E_INVALIDARG
t+86s  E_ABORT/ResizeBuffers
```

Three different families, all early, on the 51st-54th launch of the day. The
honest reading is that machine state degrades across a long session of
launch-crash cycles, and that a run of attempts under those conditions cannot
discriminate between hypotheses about our code.

One tempting inference to record as *not* supported. The `Close()` family went
from 1-in-16 before the probe fix to 4-in-12 after, which looks like the more
permissive probe causing more bad copies. But the probe fix is also what lets a
run reach gameplay and issue any copies at all -- a run that dies in the menu
cannot produce a crash from our own copy. The two populations are not comparable,
so the ratio is not evidence. Worth re-measuring on a fresh machine with equal
numbers of gameplay-reaching runs; not worth acting on now.

## 9. What I would do differently

1. **Verify on the fixture before the game, always.** The one crash would have
   been caught in 12 seconds by the 3-buffer fixture I already had.
2. **Never let a probe filter its own evidence.** Absent and misread must be
   distinguishable, or the log answers a question you did not ask.
3. **Compare timestamps, not just facts.** §2.5 cost three runs because two log
   lines were read as independent rather than as a sequence.
4. **Check the arithmetic before adding a bound.** 3000 of 350,000 is 4%, and I
   shipped it as a robustness improvement. Same error in §7.5: 255 slots against
   38,000 primitives was never going to work by cycling.
5. **Diagnose my instrument before diagnosing someone's machine.** §7.1 is the
   worst instance in the project: the same symptom, misdiagnosed twice by
   inference, settled in one command once I actually asked the OS what the input
   desktop was called.
6. **Distinguish "absent" from "filtered".** §2.4 and §7.3 are the same bug two
   months apart. Any filter that can hide evidence needs a log line upstream of
   itself, or its silence is unreadable.
7. **Ask what the metric would look like if it were wrong.** `marked 250 this
   batch`, `255 live slots, 0 evictions`, and `21 masks captured` were all true
   and all describing failures. A counter that only goes up measures effort, not
   result — pair it with one that can go down.
8. **The engine usually already knows.** `WasRecentlyRendered` was in the
   reflected function list from the first successful discovery run. I spent a
   run building a slot lottery before reading the list I had already printed.
9. **Re-audit shared state when you add a thread, not when it crashes.** §7.11
   was unsafe from the first commit and invisible because only one thread wrote
   it. Adding a writer is the moment to re-read every global the new thread can
   reach — that review takes minutes, and the crash cost a full run plus the
   diagnosis.
10. **A handle is not an identity, in any layer.** §8.3 is the same bug as the
    UObject generational handle, one layer down. I wrote that guard, understood
    exactly why GC slot reuse breaks a stored pointer, and never asked whether
    the graphics API recycled handles the same way. When you fix a class of bug
    in one layer, go and ask the question in every other layer that stores a raw
    handle — that sweep costs an hour and this one cost four wrong diagnoses.
11. **Print the object, not your model of it.** Four hypotheses died the instant
    one log line showed the elected resource's real `GetDesc` next to what the
    election believed. Every diagnostic before it described my own bookkeeping
    back to me, so all of them agreed with each other and none of them agreed
    with the GPU.