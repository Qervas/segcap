# tools

Everything here is a standalone CLI script unless noted. Nothing imports most of
them, which is normal and not a sign of decay — `overlay.py` is a program, not a
library.

Use base python, not a conda env with a broken `_ctypes`. `capture.py` detects
that case and re-execs itself, and says so.

## Running a capture

| | |
|---|---|
| `capture.py` | **the entry point.** One command per title: `python tools/capture.py inzoi --inject` |
| `games.py` | `GameProfile` per title — exe, menu route, load signals, transport position |
| `runner.py` | the run itself: prepare → launch → into_gameplay → capture → report. Shared by every title, so a fix applies to both by construction |
| `harness.py` | Win32 primitives — process liveness, window handles, log waits, input |

`--preflight` walks the whole control flow with the game stubbed out, in about a
second. Run it first; four runs died to harness mistakes that only surfaced four
minutes in, each costing a loaded game world.

## Helpers the runner shells out to

| | |
|---|---|
| `act.ps1` | one input or one screenshot, DPI-aware. Clicks by *fraction* of the window, never absolute pixels |
| `archive_capture.ps1` | moves the previous run's output into `captures/<title>_<stamp>/` before the log is deleted |
| `reset_markers.ps1` | **the single authoritative list of marker files.** Adding a marker to `dllmain.cpp` means adding it here, once. Markers outlive the run that created them and have corrupted three runs |
| `shoot_window.ps1` | window screenshot on its own |
| `live.ps1` | drive a run that is ALREADY in a loaded world. Getting inZOI to a save costs ~6 minutes, so iterating without relaunching is worth having |

## Looking at the output

| | |
|---|---|
| `overlay.py` | one mask + its frame → a colourised PNG. Refuses to align rather than guess |
| `make_demo.py` | the paired video: rendered left, labelled mask right |
| `mask_movie.py` | masks alone, for when there is no colour underlay |
| `mask_align.py` | the alignment maths, imported by the two above |
| `identity_report.py` | per-session id statistics — objects per mask, slot churn, identity survival |
| `pack.py` | masks + sidecars → a single `.segcap` container, losslessly |

## Checking that it is real

| | |
|---|---|
| `verify_labels.py` | does every id in every mask resolve through its sidecar? Necessary, **not sufficient** — it cannot see an id that resolves to the *wrong* object |
| `validate_mask.py` | structural sanity of a single mask |
| `mask_sanity.py` | connectivity and stability across frames |
| `ab_diff.py` | did marking change what the player sees? Judged against the game's own temporal-AA noise floor, not against zero |
| `dither_probe.py`, `dither_phase.py` | characterise the stippled regions — UE dithered LOD, a real CustomDepth constraint |
| `probe_controls.py` | discover which button does what by measuring, rather than hardcoding a title's controls |

The check that outranks all of these lives in the DLL, not here: `--groundtruth`
unmarks one object mid-run and requires exactly its pixels to vanish. It is the
only test that a coherent-but-wrong mask cannot pass. See `docs/DEBUGGING.md`
§8.13–8.15.

## Not on the live path

`legacy/` — the PowerShell harness that `capture.py` replaced. Two of these began
as a copy of each other and drifted, and each ended up carrying bugs the other did
not; that is precisely why the runner is now shared. Kept because
`docs/DEBUGGING.md` refers to what they did.

`renderdoc/` — offline RenderDoc capture analysis, from the route that was closed
when it turned out Nanite writes its visibility buffer through a UAV, which is
invisible to anything watching `OMSetRenderTargets`. Kept for the same reason.
