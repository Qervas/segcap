# Where AI was used, and where it was overridden

Antoine asked for this explicitly. This is the honest version, including the
parts that do not flatter the tooling.

The whole project was built by Claude Code working against a real shipped game,
with Frank steering. So the interesting question is not "was AI used" — it was,
for essentially all of the code — but **where it was reliable, where it was
confidently wrong, and what caught it.**

---

## 1. Where it was straightforwardly effective

**Boilerplate with a known shape.** D3D12 vtable hooking, the readback ring,
COM plumbing, the PE-header walking, the PowerShell/C# interop in the harness.
This is code where the correct answer is known and the work is transcription.
No override was ever needed here.

**Diagnostic tooling.** This turned out to be where AI paid for itself most, and
it is not the obvious answer. Almost every hard bug in this project was found by
a purpose-built instrument rather than by reasoning:

| instrument | what it caught |
|---|---|
| election score table with rejection reasons | scene depth scoring +10 after a "fix" |
| `CreateDSV` logging upstream of the census filter | the census said "no depth buffer exists"; there were three, at 640x360 |
| per-candidate ProcessEvent valid/invalid counts | which vtable slot was actually ProcessEvent, 4:1 |
| `identity_report.py` | that the project's central identity claim had zero supporting evidence |
| `ab_diff.py` noise floor | that 12.2% of pixels differ between two frames of a *static* camera |

Writing a bespoke instrument is normally not worth the time. When the marginal
cost of a good one drops to a couple of minutes, the correct engineering
strategy shifts — measure far more aggressively than you otherwise would. That
is a genuine change in how to work, not just faster typing.

**Analysis tooling.** `overlay.py`, `pack.py`, `make_demo.py`,
`identity_report.py` — all AI-written, all doing real work.

---

## 2. Where the human overrode the AI, and was right

These are the ones that changed the outcome.

**D3D11 vs D3D12.** The initial recommendation was D3D11, and part of the
argument was about *appearances* — that going against the brief's example and
not finishing would read badly. That is a status argument, not an engineering
one, and it should not have been in the analysis. Frank rejected it:

> *"if d3d12 is hard, let's not running away from it ok? if there's a hardship
> then let's face it"*

D3D12 was correct. It is also strictly harder — the command queue is not
reachable from the swapchain, descriptor handles carry no identity, resource
state must be shadowed — and every one of those difficulties maps onto a bullet
in the brief. Taking the easier path would have produced a smaller answer to a
question that was asking about the harder one.

**"It's not locked. Wrong identification."** I told Frank his session was
locked, based on `GetForegroundWindow()` returning a pid of 0. It was not
locked. The correct reading was that there was no foreground window at all,
which is a different thing, and the actual cause turned out to be the
screensaver switching the input desktop. I had blamed the user's machine for my
instrument's ambiguity. This is the worst single error in the project and it
recurred: I misdiagnosed the same symptom twice more before finally running
`OpenInputDesktop` and reading the desktop's name.

**"That frame is a menu, any inside of game frames?"** I presented main-menu
captures as progress. They were real masks, correctly produced, and they were
not the deliverable. The rejection was correct and forced the work that
uncovered three genuine bugs — the visibility problem, the capture-stride
problem, and the marking-starts-too-late problem — none of which are visible at
a menu, because a menu has few enough objects that marking arbitrary ones
happens to work.

**"We need to automate everything cuz this is game dataset collection job. It
can't be done by human in the future for production right?"** This reframed the
target from "produce a capture" to "produce a capture pipeline", and it was the
single most productive instruction in the project. Every remaining bug was found
by the system running unattended for five minutes at a time. A human in the loop
would have hidden the screensaver failure completely.

**"Every crash and fix we need to log them down."** Set the documentation
discipline. `DEBUGGING.md` exists because of this instruction, and it is
plausibly the most valuable artifact here given Antoine's stated priorities.

---

**The sim was never running, and a photograph proved it.** For hours I reported
captures as "live gameplay". Frank sent a photo of his screen showing the
transport bar paused:

> *"you didn't turn on the timer so it's paused"*

Everything I had used to claim otherwise was indirect and each one failed
differently. Object-count delta accepted +3 out of 564,553 objects as "time is
flowing" — that is garbage collection. UE's `IsGamePaused` reads false even in
the opening menu, because inZOI runs its own world clock and the engine flag
describes a different thing entirely. A full-frame screenshot comparison
reported motion because the *loading spinner* was animating.

He then had to say it twice more before I acted on the right thing:

> *"don press 1, it's not working, use mouse click"*
> *"you can web search first"*

The search took thirty seconds and settled a question I had built two rounds of
machinery around. The screenshot took none and showed the actual state
immediately: **the harness had been clicking a loading screen**, so no input
ever reached a transport bar. The coordinate I had spent hours blaming was
correct from the start.

**"That's not reliable, there must be more direct info."** I was inferring game
state from render-target counts and object-count deltas. Frank pushed for
something direct, which led to reading UE's own `UWorld` name — a fact, not a
correlate, and the signal that finally distinguished the menu from a loaded save
and fixed the timing. Worth noting that my *first* attempt at "direct" was also
wrong twice over: it read the Class Default Object instead of the live world,
and the property route returned zero properties on this engine build.

**"Why don't the objects near her show up?"** A single glance at a mask. The
answer was that 255 ids were being spent by object-scan order — a palm tree at
the end of the street held a slot while the chair the character sat on had none.
Feeding pixel-area back from the mask lifted coverage from 15-64% to 82-91%.
Nothing in my own testing had asked "are these the *right* 255".

**The pattern.** In every case above the human was looking at the artefact and I
was looking at instrumentation. My checks were not wrong so much as *aimed
slightly beside the question*, and each was defended with more confidence than
the evidence supported. Three separate times I reported something as verified —
the crash fixed, the sim resumed, the colour path implicated — on evidence that
could not carry the claim.

## 3. Where the AI corrected itself, and what did the correcting

Never by reasoning. Always by an instrument.

- **Defensive `getattr` produced four confident wrong answers** about the
  RenderDoc API. The fix was methodological: probe the real API shape first,
  never silently fall back. A fallback that returns a plausible value converts a
  crash into a wrong answer, which is strictly worse.
- **The election was designed against a synthetic fixture that could not
  exercise its own discriminator.** It looked correct until a real game produced
  1,085 target flips in one session.
- **A stale memory map caused six separate bugs** before the pattern was
  recognised as one cause.
- **The identity claim had no evidence.** `identity_report.py` was written to
  demonstrate that identity survives slot loss, and reported `0` — the property
  was real, the observation window was too short to contain it. Without the
  tool, a design document asserting the property would have shipped unchallenged.

---

## 4. The failure mode worth naming

Roughly two thirds of the time lost in this project went to **a measurement that
could not observe the thing it was being used to judge, reporting the answer
that matched the current hypothesis.** Not one of them threw an exception.

Concrete instances:

- a census that could only show targets it had resolved, used to conclude no
  depth buffer existed
- `marked 250 this batch` reported every 250ms while every batch evicted the
  previous one
- `255 live slots, 0 evictions` describing 255 labels on off-screen objects
- `21 masks captured` from a 1.6-second window during a transient
- "identity survives slot loss: 0 observed" from a window with no occlusions
- a residual estimated at "small" that was 3.17% of pixels

Every one of these reads as success. That is the property that makes them
expensive: a metric that only counts effort will confirm any hypothesis you
bring to it. The habit that actually helped was asking, before trusting a
number, **"what would this look like if the thing I am claiming were false?"** —
and if the answer is "the same", the metric is not evidence.

**The sibling failure mode: a guard that reads the world instead of the data.**
The id probe rejected the correct buffer eleven times running while reporting
that 100% of its texels carried ids we had leased, because a second condition
asks "does one value dominate the match?" That sounds like a question about
whether the buffer contains object ids. It is a question about whether the room
has one big object in it — and inZOI's apartment shell is a single mesh covering
85% of an indoor view. No threshold separates those two readings, because they
are not the same question, and the guard had never once caught the thing it was
added for. Asking "what is this condition actually a fact *about*?" is the same
move as asking what failure would look like, applied to a predicate instead of a
number.

**And the one I liked least: a test whose success condition destroys it.** The
ground-truth intervention unmarks one object and requires exactly its pixels to
vanish. Its verdict waits 30 masks; the slot evictor reclaims a slot that renders
nothing after 8, and *zero rendered pixels is what success looks like*. So on any
title where the slot pool is full, the measurement is guaranteed to be reported
INCONCLUSIVE — the stronger the result, the faster it is destroyed. It went
unnoticed because the test was written and tuned on Stray, whose scenes never
fill the pool, so the confound is unreachable there. Verifying a check on the
easy case tells you it runs, not that it can fail.

This is a failure mode LLMs are especially prone to, because generating a
plausible confirming explanation is cheap and generating doubt is not. The
countermeasure is not more careful reasoning. It is instruments that can return
bad news, and a human willing to say *"that frame is a menu."*

---

## 5. What I would tell someone starting a similar project

1. Spend the time on instruments, not on reasoning. The marginal cost of a good
   one is now minutes; use that.
2. Any filter that can hide evidence needs a log line upstream of itself, or its
   silence is unreadable. This bug occurred twice here, two months apart.
3. Pair every counter that goes up with one that can go down.
4. Check the observation window contains the event you are claiming, before
   reporting that the event did not occur.
5. When the human says the diagnosis is wrong, believe them and go measure. Both
   times it happened here, they were right.
