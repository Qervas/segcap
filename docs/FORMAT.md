# Output format

A capture session produces, for every captured frame:

| file | contents |
|---|---|
| `segcap_mask_<N>.pgm` | 8-bit per-pixel object ids, binary PGM (P5) |
| `segcap_frame_<N>.ppm` | the rendered frame, binary PPM (P6), RGB24 |
| `segcap_mask_<N>.json` | the slot → object table for that frame |

and once per session:

| file | contents |
|---|---|
| `segcap_input.jsonl` | every controller state delivered to the game, timestamped |

## Actions, for world models

A world model learns `P(next frame | frame, action)`. A video without the actions
that produced it is half a training pair, so the input is recorded alongside the
frames.

The unusual property here is that the pipeline **synthesises** the input, so the
action is known exactly rather than inferred. There is no hook into the game's
input handling, no guessing at deadzone curves, no sampling race against the
game's polling. Each record is literally the report handed to the virtual pad
driver, written at the moment it was sent:

```json
{"t":1786260344126,"lx":0,"ly":24000,"rx":9000,"ry":0,"lt":0,"rt":0,"buttons":4096}
```

`buttons` is the XUSB bitfield (`0x1000` = A). Sticks are the native
−32768..32767; triggers 0..255.

**Joining frames to actions.** Every sidecar carries `timestampMs`, stamped at
`Present` — not when the readback lands, which is several frames later and would
shift every action label. `segcap_input.jsonl` uses the same clock. Both come
from `GetSystemTimeAsFileTime`, deliberately a wall clock rather than
`QueryPerformanceCounter`: the two logs are written by *different processes*
(the injected DLL and `vpad.exe`), so anything process-relative cannot be joined
across that boundary at all.

The join is a step function — the last sample at or before the frame — because
that is what a gamepad is. Interpolating between samples would invent stick
positions the game never saw. Frames further than 2s from any sample are
reported as having no known action rather than being attributed the nearest one.

The pad re-sends its held state at ~10Hz rather than only on change, so the log
has regular samples instead of one record per multi-second hold. Measured on a
300-frame session: **300/300 frames matched to an action.**

`tools/make_demo.py` draws this as a live controller panel under the video.

`<N>` is the same monotonic frame index in all three, so a mask and its frame
are paired by filename alone. They are the *same* frame by construction: both
are copied during the same `Present` call, so there is no synchronisation step
and nothing to drift.

## Why the sidecar is not optional

The stencil channel is 8 bits. Slot 42 in one frame and slot 42 in another can
be different objects, because 255 slots have to describe a level with ~33,000
markable primitives. The slot is a **lease**, not an identity.

The sidecar resolves it:

```json
{
  "frameIndex": 6145,
  "timestampMs": 1786646498443,
  "width": 1280,
  "height": 720,
  "bindings": [
    {"slot": 51, "stableId": 51,  "className": "SkeletalMeshComponent", "objectName": "CharacterMesh0",  "serial": 8148, "released": false},
    {"slot": 126,"stableId": 126, "className": "SkeletalMeshComponent", "objectName": "Droid_Head",      "serial": 5423, "released": false},
    {"slot": 10, "stableId": 272, "className": "SplineMeshComponent",   "objectName": "NODE_AddSplineMeshComponent-0", "serial": 0, "released": false}
  ]
}
```

The envelope matters to anyone writing a parser: the top level is an **object**,
not an array, and `bindings` is where the rows live. `frameIndex` is the same
monotonic index as the `.pgm` filename, so a sidecar can be matched to its mask
without parsing the name.

Two per-binding fields carry the identity machinery:

| field | meaning |
|---|---|
| `serial` | the engine's own serial number for that object. It is half of the `(pointer, serialNumber)` key behind `stableId`, and it is what makes a recycled address detectable rather than silently merging two objects. |
| `released` | the slot's lease had been handed back by the time this frame's table was written. The row is kept rather than dropped, because pixels carrying that id may still be in *this* frame — a released row is how you tell "the label was retired" apart from "the label was never there". |

`stableId` is a 64-bit identity keyed on `(pointer, serialNumber)`. It survives
losing a slot, so an object that goes off screen and comes back resumes its
original track instead of being counted as a new object. Note slot 10 holding
stableId 272 above: slots have been recycled, and the sidecar is what makes that
recoverable rather than corrupting.

The sidecar is written for **every** dumped mask, never separately, and it
describes the table as it stood when that frame's copy was submitted — not when
the mask arrived several frames later. See DEBUGGING.md §7.8 for why that
distinction cost real label accuracy.

## Container

`tools/pack.py` bundles a session into one self-describing `.segcap` file.

```
python tools/pack.py --dir build/bin --out session.segcap --verify
python tools/pack.py --list session.segcap
python tools/pack.py --extract session.segcap --to out/
```

```
magic     8 bytes   "SEGCAP01"
hdrlen    u32
header    utf-8 JSON  {"width":..,"height":..,"codec":..,"records":..,"frames":[..]}
records   ...       each: type u8, frame u64, rawlen u32, complen u32, payload
index     utf-8 JSON  [{type, frame, offset, rawlen, complen}, ...]
idxlen    u32
footer    8 bytes   "SEGCAPIX"
```

The index is at the **end**, with its length in the last 12 bytes. A reader can
seek to any frame without scanning the file, and the writer never has to know
the record layout in advance — the same reason zip puts its directory last.

### Compression lives in the tool, not the DLL

The capture path runs inside a shipped game, on the thread that presents frames.
Everything on it has been kept to the minimum that cannot be done later: a GPU
copy onto the game's own queue, a fence poll, a memcpy out of a mapped readback
buffer. Linking a compressor in there would add third-party code to the game's
address space and CPU time to its present loop, to save disk that is not scarce.

Compression is a pure function of bytes already written. It belongs outside the
process being instrumented.

zlib is the default because it is Python standard library, so a container reads
anywhere with no install step. The codec is recorded **per record**, so adding
zstd later does not invalidate existing files.

### Measured, 75 frames at 1280x720

| | raw | packed | ratio |
|---|---|---|---|
| masks | 69.1 MB | 0.7 MB | **104.6x** |
| colour frames | 207.4 MB | 115.5 MB | 1.8x |
| sidecars | 1.3 MB | 0.1 MB | 11.9x |
| total | 277.8 MB | 116.3 MB | 2.4x |

Reported separately on purpose. Masks are 8-bit indexed images with large flat
regions — close to the best case for a dictionary coder. Colour frames are
photographic and barely compress. A single "2.4x" would hide both facts and
imply the format does something for colour that it does not.

`--labels-only` omits colour and produces 0.8 MB for the same 75 frames, which
is the useful artifact for anything consuming the labels.

### Two properties enforced in code

**Lossless is verified, not asserted.** `--verify` decompresses every record and
byte-compares it against the file it came from — 225/225 identical on the
session above. A compression bug corrupting one frame in a thousand would
otherwise surface as a mysteriously bad training example months later.

**A mask cannot be packed without its sidecar.** The packer refuses the whole
container if any mask lacks one, because a mask without its slot table is
undecodable rather than merely incomplete. Worth enforcing rather than
documenting and hoping.
