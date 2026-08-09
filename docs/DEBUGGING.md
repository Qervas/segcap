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

Residual, honestly stated: a primitive unmarked mid-frame can still write its old
stencil value for a frame or two while the render proxy rebuilds, so a small
number of ids can outlive their binding. Not yet fixed.

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

---

## 8. What I would do differently

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
