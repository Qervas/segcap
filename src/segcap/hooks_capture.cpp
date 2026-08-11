#include "hooks.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "customdepth.h"
#include "log.h"

// Linker-provided base of this module; avoids needing the HMODULE passed around
// just to find where our own DLL lives.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace segcap {

void Hooks::SetIntervene(bool on) {
    interveneState_ = on ? Intervention::Waiting : Intervention::Off;
}
namespace {

// Capture budget, expressed as coverage in TIME rather than in frames.
//
// At 720p a mask is 900KB and a colour frame 3.6MB, so 150 captures is ~675MB.
// The stride is what matters: at 60fps a stride of 5 frames means 12 captures a
// second, so a 40-capture budget was spent in 3.3 seconds. One run recorded all
// 21 of its masks between t+201.4s and t+203.0s -- a 1.6-second window that
// happened to fall while the marking working set was still filling, so every
// mask contained exactly one object. The data was not wrong, it was a
// photograph of a transient.
//
// A stride of 60 gives one capture a second, so 150 of them span 150 seconds of
// gameplay: long enough to cross rooms, change what is on screen, and -- the
// reason it was raised from 30 -- long enough for objects to LEAVE the working
// set and come back. At stride 30 the captured window was 37 seconds, over
// which not one identity was released and re-acquired, so the analysis could
// not demonstrate that identity survives slot loss. The mechanism was fine; the
// observation window was too short to contain the event being claimed.
// Defaults. Overridable at runtime via segcap.captures / segcap.stride, because
// a demo run and an identity-analysis run want opposite settings and neither is
// "the" correct value. See Hooks::SetCaptureProfile.
constexpr uint32_t kMaxCaptures = 150;
constexpr uint64_t kCaptureStride = 60;

// A/B mode needs its budget spread across the WHOLE session, not the start of
// it. Marking cannot begin until ProcessEvent is verified and the level has
// loaded, which is ~8,000 frames in; at stride 30 the 150-frame budget was
// exhausted by frame 4,500 and every captured frame was from before the
// transition being measured. There was no "after" side at all.
constexpr uint64_t kAbCaptureStride = 120;   // ~2s at 60fps -> ~300s of coverage

}  // namespace

// Writes the stencil plane as a binary PGM. Chosen because it is trivially
// readable by any validator without a decoder, and because a mask is exactly a
// single-channel 8-bit image -- which is also a reminder that the channel is 8
// bits and therefore holds a per-frame slot, not an identity.
void Hooks::OnMaskReady(const MaskFrame& frame) {
    // Scan for content BEFORE deciding whether to dump.
    //
    // An earlier version dumped the first three frames unconditionally. Those
    // landed at t=3.1s, while primitives were not marked until t=41.2s -- so
    // every dump was of an empty buffer taken 38 seconds before there was
    // anything to see, and the empty result was misread as CustomDepth being
    // broken. The pipeline was correct; the shutter fired at the wrong moment.
    //
    // Dumping on CONTENT rather than on frame number removes the timing
    // coupling entirely: the capture happens when there is something to capture,
    // whenever that turns out to be.
    // When the mask comes from a multi-byte id buffer rather than a stencil
    // plane, flatten the proven channel to 8 bits FIRST. Everything downstream --
    // the content scan, the leased-slot check, the PGM writer, every tool in
    // tools/ -- is written against one byte per pixel, and a slot is 1..255 by
    // construction (IdentityRegistry::kSlotCount), so nothing is lost.
    MaskFrame view = frame;
    if (frame.bytesPerPixel != 1 && maskChannel_ >= 0) {
        maskScratch_.assign(static_cast<size_t>(frame.width) * frame.height, 0);
        const uint32_t bpp = frame.bytesPerPixel;
        for (uint32_t y = 0; y < frame.height; ++y) {
            const uint8_t* src = frame.data + static_cast<size_t>(y) * frame.rowPitch;
            uint8_t* dst = maskScratch_.data() + static_cast<size_t>(y) * frame.width;
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint8_t* p = src + static_cast<size_t>(x) * bpp;
                uint32_t v = 0;
                if (bpp == 2) {
                    v = maskChannel_ == 0
                            ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8))
                            : (maskChannel_ == 1 ? p[0] : p[1]);
                } else {
                    const uint32_t w = static_cast<uint32_t>(p[0]) |
                                       (static_cast<uint32_t>(p[1]) << 8) |
                                       (static_cast<uint32_t>(p[2]) << 16) |
                                       (static_cast<uint32_t>(p[3]) << 24);
                    v = maskChannel_ == 0 ? (w & 0xFFFFu)
                                          : (maskChannel_ == 1 ? ((w >> 16) & 0xFFFFu) : w);
                }
                // Anything outside the slot range is not one of ours; drop it to
                // 0 (unlabelled) rather than let it alias onto a real slot.
                dst[x] = v < 256 ? static_cast<uint8_t>(v) : 0;
            }
        }
        view.data = maskScratch_.data();
        view.rowPitch = frame.width;
        view.bytesPerPixel = 1;
    }
    const MaskFrame& mf = view;

    bool seen[256] = {};
    for (uint32_t y = 0; y < mf.height; ++y) {
        const uint8_t* row = mf.data + static_cast<size_t>(y) * mf.rowPitch;
        for (uint32_t x = 0; x < mf.width; ++x) {
            seen[row[x]] = true;
        }
    }
    uint32_t distinctIds = 0;
    for (int i = 1; i < 256; ++i) {
        if (seen[i]) ++distinctIds;
    }

    // "Content" means a USEFUL mask, not merely a non-empty one.
    //
    // Gating on "any non-zero pixel" starts recording the instant the first
    // primitive is marked -- which is the worst possible moment, because the
    // working set then takes another 30-60 seconds to fill to 255 slots. With a
    // dense capture stride the entire budget was spent inside that ramp: a
    // 10-second demo where every frame showed 11 objects and 29% coverage,
    // while the same build 40 seconds later was doing 80 objects and 100%.
    //
    // This is the third time in this project the shutter fired at the wrong
    // moment (see DEBUGGING.md 7.7 and the note above). The pattern is always
    // the same -- a trigger condition that is technically satisfied long before
    // the thing it is meant to detect is actually happening.
    constexpr uint32_t kMinIdsToStart = 24;

    // ...and the ids in the buffer must be OURS.
    //
    // Fourth time, and the worst of the four, because this one produced output
    // that looked right. On inZOI the marker never resolved -- UE5's property
    // chains were not populated when it looked -- so segcap marked exactly zero
    // primitives. The elected target still had a stencil plane, because the
    // engine uses stencil for its own purposes, and this gate happily counted
    // 154 distinct values in it and started recording. The result was
    // 2560x1600 masks with a clean-looking 0..153 id range that were entirely
    // the game's own stencil bits and contained no object identity whatsoever.
    //
    // Every downstream tool would have accepted them. The sidecar would have
    // been empty or wrong, which is the only reason it was caught.
    //
    // "The buffer has content" and "the content is content we produced" are
    // different claims, and only the second one justifies writing a mask.
    const size_t ourMarks = GetMarker().markedCount();

    // ...and FIFTH time, worse again, because this one survived a PASS from the
    // validator. On inZOI the buffer was full of plausible-looking ids and every
    // check above was satisfied: 255 distinct values, 187 primitives marked. The
    // pixels were a 2-channel fp16 velocity buffer read through an 8-bit stencil
    // footprint -- another render target's memory entirely.
    //
    // "We marked something" and "these pixel values are the ones we handed out"
    // are STILL different claims. The sidecar knows exactly which slots were
    // leased, so the buffer can be checked against it directly: a value we never
    // leased cannot be ours, whatever it looks like.
    //
    // Stray measures 0% unleased. inZOI measured 5-23%, swinging frame to frame.
    // A tolerance is needed rather than zero because a primitive that hands its
    // slot back keeps drawing for a frame or two until its proxy is rebuilt
    // (see SlotBinding::released), so a small unleased fraction is legitimate.
    uint32_t unleasedIds = 0;
    uint64_t unleasedPixels = 0;
    uint64_t labelledPixels = 0;
    bool leaseCheckPossible = false;

    // Check against the table that was live WHEN THIS FRAME WAS SUBMITTED, not
    // the one live now. The readback ring is three deep and marking runs
    // continuously, so the current table can be several passes ahead -- and this
    // check compares individual pixel values against it, which makes it maximally
    // sensitive to that drift. Measured: 62.8% "unleased" against the current
    // table on a frame that was fine against its own.
    //
    // The dump below already does exactly this lookup for exactly this reason,
    // with the measured cost of getting it wrong recorded beside it. I wrote the
    // check without reusing it.
    std::shared_ptr<const FrameSidecar> atSubmission;
    for (const auto& entry : sidecarHistory_) {
        if (entry.first == frame.frameIndex && entry.second) {
            atSubmission = entry.second;
            break;
        }
    }
    if (!atSubmission) atSubmission = GetMarker().publishedSidecar();
    if (auto sc = atSubmission) {
        bool leased[256] = {};
        for (const auto& b : sc->bindings) {
            if (b.slot > 0 && b.slot < 256) leased[b.slot] = true;
        }
        leaseCheckPossible = !sc->bindings.empty();
        for (int i = 1; i < 256; ++i) {
            if (seen[i] && !leased[i]) ++unleasedIds;
        }
        for (uint32_t y = 0; y < mf.height; ++y) {
            const uint8_t* row = mf.data + static_cast<size_t>(y) * mf.rowPitch;
            for (uint32_t x = 0; x < mf.width; ++x) {
                const uint8_t v = row[x];
                if (!v) continue;
                ++labelledPixels;
                if (!leased[v]) ++unleasedPixels;
            }
        }
    }
    const double unleasedFrac =
        labelledPixels ? static_cast<double>(unleasedPixels) / static_cast<double>(labelledPixels)
                       : 0.0;
    constexpr double kMaxUnleasedFraction = 0.02;
    const bool idsAreOurs = !leaseCheckPossible || unleasedFrac <= kMaxUnleasedFraction;

    const bool hasContent = distinctIds >= kMinIdsToStart && ourMarks > 0 && idsAreOurs;

    // ---- ground truth by intervention --------------------------------------
    //
    // Change exactly one thing and require exactly one consequence. Everything
    // else this file does can be satisfied by a mask that is coherently wrong;
    // this cannot. If "slot S means that object" is true, clearing S's flag
    // removes S's pixels and moves nothing else. If the mask is a different
    // buffer, or the slot table is a fiction, the pixels sit there unchanged.
    if (interveneState_ != Intervention::Off && hasContent) {
        // Per-slot pixel counts for this frame -- the measurement itself.
        uint64_t perSlot[256] = {};
        for (uint32_t y = 0; y < mf.height; ++y) {
            const uint8_t* r = mf.data + static_cast<size_t>(y) * mf.rowPitch;
            for (uint32_t x = 0; x < mf.width; ++x) ++perSlot[r[x]];
        }
        uint64_t labelled = 0;
        for (int i = 1; i < 256; ++i) labelled += perSlot[i];

        switch (interveneState_) {
            case Intervention::Waiting: {
                if (++interveneStableFrames_ < kInterveneWarmupFrames) break;
                // Pick the largest non-trivial slot: the bigger its footprint,
                // the less ambiguous "its pixels vanished" is. A slot with 40
                // pixels could disappear through ordinary occlusion.
                uint8_t best = 0;
                uint64_t bestPx = 0;
                for (int i = 1; i < 256; ++i) {
                    if (perSlot[i] > bestPx) { bestPx = perSlot[i]; best = static_cast<uint8_t>(i); }
                }
                if (!best || bestPx < 500) break;   // wait for something worth measuring
                interveneSlot_ = best;
                intervenePixelsBefore_ = bestPx;
                interveneOthersBefore_ = labelled - bestPx;
                interveneState_ = Intervention::Requested;
                LogWarn("groundtruth: selected slot %u with %llu px (%.2f%% of frame); "
                        "everything else holds %llu px. Requesting unmark.",
                        static_cast<unsigned>(best), bestPx,
                        100.0 * static_cast<double>(bestPx) /
                            static_cast<double>(static_cast<uint64_t>(mf.width) * mf.height),
                        interveneOthersBefore_);
                // The unmark must happen on the game thread; it calls the
                // engine's own setter so the render proxy is rebuilt.
                const uint8_t slot = best;
                ue4::GetProcessEventHook().RunOnGameThread([slot](ue4::Engine& e) {
                    GetMarker().UnmarkSlotForGroundTruth(e, slot);
                });
                interveneFiredFrame_ = mf.frameIndex;
                interveneState_ = Intervention::Settling;
                interveneStableFrames_ = 0;
                break;
            }
            case Intervention::Settling: {
                // Give the renderer time to rebuild the scene proxy. Judging on
                // the very next frame would fail even when the claim is true.
                if (++interveneStableFrames_ < kInterveneSettleFrames) break;
                const uint64_t after = perSlot[interveneSlot_];
                const uint64_t othersAfter = labelled - after;
                const double gone =
                    intervenePixelsBefore_
                        ? 1.0 - static_cast<double>(after) /
                                    static_cast<double>(intervenePixelsBefore_)
                        : 0.0;
                // Others are allowed to move: the camera and the world keep
                // running. A wide band, because this asks "did everything else
                // stay roughly put", not "is the scene identical".
                const double othersDelta =
                    interveneOthersBefore_
                        ? std::abs(static_cast<double>(othersAfter) -
                                   static_cast<double>(interveneOthersBefore_)) /
                              static_cast<double>(interveneOthersBefore_)
                        : 0.0;
                const bool targetGone = gone >= 0.95;
                const bool othersHeld = othersDelta <= 0.50;
                LogWarn("groundtruth RESULT: slot %u went %llu -> %llu px (%.1f%% removed); "
                        "all other slots %llu -> %llu px (%.1f%% change). VERDICT: %s",
                        static_cast<unsigned>(interveneSlot_), intervenePixelsBefore_, after,
                        100.0 * gone, interveneOthersBefore_, othersAfter, 100.0 * othersDelta,
                        (targetGone && othersHeld)
                            ? "PASS -- exactly the unmarked object's pixels disappeared"
                            : (!targetGone ? "FAIL -- the unmarked object's pixels are STILL "
                                             "THERE, so the slot does not mean what the sidecar "
                                             "says"
                                           : "INCONCLUSIVE -- the rest of the scene moved too "
                                             "much to attribute the change"));
                interveneState_ = Intervention::Done;
                break;
            }
            default:
                break;
        }
    }

    if (leaseCheckPossible && !idsAreOurs && !warnedUnleasedIds_) {
        warnedUnleasedIds_ = true;
        LogError("capture: REFUSING to record -- %.1f%% of labelled pixels carry stencil "
                 "values we never leased (%u distinct unleased ids of %u present). Whatever "
                 "this buffer is, it is not our CustomDepth output. Elected target %p.",
                 100.0 * unleasedFrac, unleasedIds, distinctIds,
                 static_cast<void*>(electedTarget_));
    }

    if (distinctIds >= kMinIdsToStart && ourMarks == 0 && !warnedForeignStencil_) {
        warnedForeignStencil_ = true;
        LogWarn("capture: elected target holds %u distinct stencil values but WE HAVE "
                "MARKED NOTHING -- this is the engine's own stencil, not object ids. "
                "Not recording. (Marker resolved? see 'customdepth:' lines.)",
                distinctIds);
    }

    // ---- recording gate ----------------------------------------------------
    //
    // Only record while actually in gameplay. Menus, loading screens and fades
    // contain no labelled objects, so capturing them would pad the dataset with
    // frames that teach nothing -- and for a dataset pipeline that is worse than
    // capturing less.
    //
    // The gate keys on mask content rather than trying to infer "is this a menu"
    // from engine state, because content is exactly the property that makes a
    // frame worth keeping. Hysteresis both ways: a camera can briefly face an
    // unmarked wall without ending a session, and a stray marked object visible
    // behind a menu should not start one.
    constexpr uint32_t kFramesToStart = 3;
    constexpr uint32_t kFramesToStop = 30;

    if (hasContent) {
        // The elected target has now demonstrably produced a mask, which is
        // what entitles it to incumbency protection in future elections. This
        // is the only place that flag is ever set, so an unproductive target
        // can never acquire it.
        electedProducedContent_ = true;
        ++consecutiveContent_;
        consecutiveEmpty_ = 0;
        if (!recording_ && consecutiveContent_ >= kFramesToStart) {
            recording_ = true;
            recordingStartedFrame_ = frame.frameIndex;
            LogInfo("RECORDING STARTED at frame %llu (%u distinct ids in the mask)",
                    frame.frameIndex, distinctIds);
        }
    } else {
        ++consecutiveEmpty_;
        consecutiveContent_ = 0;
        if (recording_ && consecutiveEmpty_ >= kFramesToStop) {
            recording_ = false;
            LogInfo("RECORDING STOPPED at frame %llu (%llu frames captured over %llu)",
                    frame.frameIndex, recordedFrames_,
                    frame.frameIndex - recordingStartedFrame_);
        }
    }

    if (!recording_) {
        ++skippedFrames_;
        // One baseline empty capture is still useful for the task-11 A/B diff.
        if (emptyMasksDumped_ >= 1) return;
        ++emptyMasksDumped_;
    } else {
        ++recordedFrames_;
        if (masksDumped_ >= maxCaptures_) return;
        // Capture every Nth frame rather than N consecutive ones. At ~30fps a
        // run of consecutive frames is under two seconds of game time and shows
        // almost no motion, which makes a useless demo. Striding spreads the
        // same budget across ~20 seconds so objects actually move, enter, and
        // leave -- which is also what makes the identity/slot behaviour visible.
        if ((recordedFrames_ % captureStride_) != 1) return;
        ++masksDumped_;
        maskKept_.insert(frame.frameIndex);
    }

    // Absolute path next to the DLL. A relative filename resolves against the
    // GAME's working directory, which put three 4MB dumps inside Stray's install
    // folder -- writing our output into someone else's game directory.
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_mask_%llu.pgm", dir.c_str(),
                 frame.frameIndex);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;

    std::fprintf(f, "P5\n%u %u\n255\n", frame.width, frame.height);
    // Row by row using rowPitch, not width: the readback pitch is padded to 256
    // bytes, so treating it as tightly packed would shear the image.
    for (uint32_t y = 0; y < mf.height; ++y) {
        std::fwrite(mf.data + static_cast<size_t>(y) * mf.rowPitch, 1, mf.width, f);
    }
    std::fclose(f);

    // Report the distinct values present. For the fixture these must be exactly
    // {0} plus 1..16, which is the whole point of having known ground truth.
    char values[512] = {};
    size_t used = 0;
    for (int v = 0; v < 256 && used < sizeof(values) - 8; ++v) {
        if (!seen[v]) continue;
        used += static_cast<size_t>(
            _snprintf_s(values + used, sizeof(values) - used, _TRUNCATE, "%d ", v));
    }
    LogInfo("mask frame %llu dumped (%ux%u pitch=%u); distinct stencil values: %s",
            frame.frameIndex, frame.width, frame.height, frame.rowPitch, values);

    // The sidecar is what makes the mask decodable. Written for every dumped
    // mask, never separately, so a mask file cannot exist without the table
    // that says what its slot numbers mean.
    //
    // NOTE: masksDumped_ is incremented by the recording gate above, not here.
    // It used to be incremented in both places, which silently halved every
    // capture budget -- asking for 150 produced 75 masks and asking for 200
    // produced 100. It looked like a plausible number every single time, and
    // was only caught by requesting a round 200 and getting exactly half.
    WriteSidecar(frame);
}
// Writes the colour backbuffer as a binary PPM (P6).
//
// PPM rather than PNG because the DLL has no image encoder and adding one to
// code that runs inside someone else's process is not worth it. The validator
// converts. Frames are named by the same monotonic index as the masks, which is
// what lets a mask and its frame be paired by filename alone.
void Hooks::OnColourReady(const MaskFrame& frame) {
    if (colourDumped_ >= maxCaptures_) return;

    // A/B mode captures colour UNCONDITIONALLY on a stride.
    //
    // The normal rule -- keep a colour frame only if its mask was kept -- is
    // right for the dataset and useless for the A/B test, because the whole
    // point of the "before" side is that there is no mask yet. Without this the
    // baseline condition produces zero frames and there is nothing to compare.
    if (abTest_) {
        if ((frame.frameIndex % kAbCaptureStride) != 0) return;
        ++colourDumped_;
    } else {
        if (!recording_) return;
        // Only keep colour frames whose mask was also kept. An unpaired frame is
        // dead weight -- make_demo can only use indices that have both.
        if (!maskKept_.count(frame.frameIndex)) return;
        ++colourDumped_;
    }

    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_frame_%llu.ppm", dir.c_str(),
                 frame.frameIndex);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);

    std::vector<uint8_t> row(static_cast<size_t>(frame.width) * 3);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint32_t px = *reinterpret_cast<const uint32_t*>(src + x * 4);
            if (frame.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                // 10 bits per channel packed into 32; shift down to 8. Stray
                // renders to an HDR10 backbuffer, so a naive byte read would
                // give channel-swapped garbage.
                row[x * 3 + 0] = static_cast<uint8_t>(((px >> 0) & 0x3FF) >> 2);
                row[x * 3 + 1] = static_cast<uint8_t>(((px >> 10) & 0x3FF) >> 2);
                row[x * 3 + 2] = static_cast<uint8_t>(((px >> 20) & 0x3FF) >> 2);
            } else {
                // Assume 8-bit BGRA/RGBA.
                row[x * 3 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF);
                row[x * 3 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
                row[x * 3 + 2] = static_cast<uint8_t>((px >> 0) & 0xFF);
            }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LogInfo("colour frame %llu dumped (%ux%u fmt=%u)", frame.frameIndex, frame.width,
            frame.height, frame.format);
}
// Writes the slot -> object table for one frame, as JSON next to the mask.
//
// A mask alone is ambiguous: the stencil channel is 8 bits, so slot 42 in one
// frame and slot 42 in another can be different objects. The sidecar is what
// resolves it, and it is emitted with EVERY mask so the two cannot become
// separated. A mask file without its sidecar is undecodable, and the format
// should make that impossible to forget rather than merely documented.
void Hooks::WriteSidecar(const MaskFrame& frame) const {
    // Use the table as it stood WHEN THE COPY WAS SUBMITTED, not as it stands
    // now.
    //
    // Readback is asynchronous by design -- the copy is queued on the game's own
    // command list and collected several frames later, which is what keeps the
    // render thread off the GPU's critical path. Meanwhile the marking thread is
    // continuously releasing slots for primitives that left the screen and
    // leasing them to new ones. So by the time a mask lands, the live slot table
    // can already describe a different set of objects than the one that wrote
    // that mask's pixels.
    //
    // The overlay is what exposed this: a gameplay mask contained ids 154, 156,
    // 242 and 255 that had no binding at all in the sidecar written beside it --
    // 5.4% of the frame's pixels labelled with an id whose meaning had already
    // been recycled. Reading the table at dump time is exactly the kind of
    // "close enough" that produces a dataset with quietly wrong labels.
    FrameSidecar sc;
    bool fromHistory = false;
    for (const auto& entry : sidecarHistory_) {
        if (entry.first == frame.frameIndex && entry.second) {
            sc = *entry.second;
            fromHistory = true;
            break;
        }
    }
    if (!fromHistory) {
        // Fall back to the live table rather than emitting nothing, but say so:
        // a sidecar that might not correspond to its mask is worth knowing about
        // rather than silently trusting.
        sc = GetMarker().SnapshotSidecar(frame.frameIndex, frame.width, frame.height);
        LogWarn("sidecar for frame %llu not found in history; using the live table, "
                "which may describe a different moment", frame.frameIndex);
    }
    sc.frameIndex = frame.frameIndex;
    sc.width = frame.width;
    sc.height = frame.height;

    long long stamp = 0;
    for (const auto& e : frameStamps_) {
        if (e.first == frame.frameIndex) { stamp = e.second; break; }
    }

    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_mask_%llu.json", dir.c_str(),
                 frame.frameIndex);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"frameIndex\": %llu,\n", sc.frameIndex);
    // Wall clock at Present, on the same clock vpad.exe stamps its input log
    // with. This is the join key between what was rendered and what was pressed.
    std::fprintf(f, "  \"timestampMs\": %lld,\n", stamp);
    std::fprintf(f, "  \"width\": %u,\n", sc.width);
    std::fprintf(f, "  \"height\": %u,\n", sc.height);
    std::fprintf(f, "  \"bindings\": [\n");
    for (size_t i = 0; i < sc.bindings.size(); ++i) {
        const SlotBinding& b = sc.bindings[i];
        // "released" marks a trailing binding: the object handed its slot back
        // but its render proxy may not have caught up, so these pixels can
        // still appear for a frame or two. A consumer that wants only current
        // labels can filter on it; without it those pixels would simply be
        // undecodable.
        std::fprintf(f,
                     "    {\"slot\": %u, \"stableId\": %llu, \"className\": \"%s\", "
                     "\"objectName\": \"%s\", \"serial\": %d, \"released\": %s}%s\n",
                     b.slot, b.stableId, b.className.c_str(), b.objectName.c_str(),
                     b.serialNumber, b.released ? "true" : "false",
                     (i + 1 < sc.bindings.size()) ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

}  // namespace segcap
