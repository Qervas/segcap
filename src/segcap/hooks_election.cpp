#include "hooks.h"

#include <algorithm>
#include <cstdio>

#include "hooks_format.h"
#include "log.h"

namespace segcap {
namespace {

// How much the currently elected target is favoured. Large enough that a
// marginally-better challenger cannot displace it, small enough that a target
// which stops being viable (goes to zero or negative) still loses.
constexpr int kIncumbencyBonus = 40;

}  // namespace

// Score candidates for "this is the CustomDepth target".
//
// Only runtime-observable signals are used. Debug names are excluded on
// purpose: they are stripped in shipping builds, so an election that leans on
// them is a technique that only works where it is not needed.
//
// The discriminator that matters is bind count. In Stray's captured frame the
// scene depth target took 1365 draws while the CustomDepth candidate took none
// -- three orders of magnitude apart. Once primitives opt in, CustomDepth still
// takes only a handful. So "full-res, has stencil, few binds, gets cleared" is
// the signature, and "most-bound target in the frame" is the thing to exclude.
std::vector<ElectionScore> Hooks::ScoreTargets(
    const std::vector<TargetFingerprint>& targets) const {
    uint32_t maxBinds = 0;
    // std::max, not max: NOMINMAX is defined project-wide so the Windows macro
    // does not exist here.
    for (const TargetFingerprint& t : targets) maxBinds = std::max(maxBinds, t.bindCount);

    // Establish the SCENE resolution, which is not necessarily the backbuffer
    // resolution.
    //
    // An earlier version hard-required t.width/height == backbuffer, on the
    // reasoning that anything smaller is a shadow map or a downsample. That
    // rejected every real depth target in a run where the game was configured
    // with ScreenPercentage=50: the backbuffer was 1280x720 but the entire 3D
    // scene -- including scene depth and CustomDepth -- rendered at 640x360.
    // The census then showed exactly one depth target, a 1x1 dummy, which reads
    // as "this game has no depth buffer" rather than "my filter ate them".
    //
    // Screen percentage, dynamic resolution and upsampling are all common, so
    // "equals the backbuffer" was never the right test. What actually
    // identifies the scene cohort is aspect ratio plus being the largest of
    // that shape: the scene targets share the backbuffer's aspect, while
    // shadow atlases are square and downsample chains are a fraction of it.
    uint64_t sceneW = 0;
    uint32_t sceneH = 0;
    if (backbufferWidth_ != 0 && backbufferHeight_ != 0) {
        // "Largest aspect-matching target" was wrong, and it failed exactly the
        // way a max-of-set rule always fails: ONE outlier redefines the answer
        // for everything else. inZOI produces an occasional 2560x1600
        // depth-stencil (a full-res pass over the upscaled image); the instant
        // it appeared, sceneW became 2560 and every real 1280x800 candidate was
        // hard-rejected as "1280x800 != scene 2560x1600". The election then held
        // an empty target for fifteen straight rounds, logged as "incumbent but
        // no content yet".
        //
        // The scene resolution is better defined as THE RESOLUTION THE GAME DOES
        // MOST OF ITS DEPTH RENDERING AT. Scene depth is bound far more than
        // anything else, so summing bind counts per resolution and taking the
        // maximum identifies it robustly -- and a rare full-res pass bound once
        // or twice cannot outvote the main depth buffer bound 15 times a frame.
        struct ResVotes {
            uint64_t width = 0;
            uint32_t height = 0;
            uint64_t binds = 0;
            uint32_t seen = 0;
        };
        std::vector<ResVotes> votes;
        for (const TargetFingerprint& t : targets) {
            if (!HasStencilPlane(t.format) || t.width == 0 || t.height == 0) continue;
            // Aspect match within 2%, done in integer arithmetic to avoid
            // float comparison entirely: w/h ~= bbW/bbH  <=>  w*bbH ~= h*bbW.
            const uint64_t lhs = t.width * static_cast<uint64_t>(backbufferHeight_);
            const uint64_t rhs = static_cast<uint64_t>(t.height) * backbufferWidth_;
            const uint64_t diff = lhs > rhs ? lhs - rhs : rhs - lhs;
            if (rhs == 0 || diff * 50 > rhs) continue;      // > 2% off: not the scene
            bool merged = false;
            for (ResVotes& v : votes) {
                if (v.width == t.width && v.height == t.height) {
                    v.binds += t.bindCount;
                    ++v.seen;
                    merged = true;
                    break;
                }
            }
            if (!merged) votes.push_back({t.width, t.height, t.bindCount, 1});
        }
        const ResVotes* best = nullptr;
        for (const ResVotes& v : votes) {
            // Most depth work wins; ties go to the resolution more targets share,
            // then to the larger one so the rule stays deterministic.
            if (!best || v.binds > best->binds ||
                (v.binds == best->binds &&
                 (v.seen > best->seen || (v.seen == best->seen && v.width > best->width)))) {
                best = &v;
            }
        }
        if (best) { sceneW = best->width; sceneH = best->height; }
    }

    std::vector<ElectionScore> out;
    for (const TargetFingerprint& t : targets) {
        ElectionScore s;
        s.resource = t.resource;

        // Hard requirements. Without a stencil plane there is nothing to read,
        // and a target below the scene resolution is a shadow map or a
        // downsample rather than a candidate.
        if (!HasStencilPlane(t.format)) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: no stencil plane (%s)",
                        FormatName(t.format));
            out.push_back(s);
            continue;
        }
        if (sceneW == 0) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: scene resolution not established yet");
            out.push_back(s);
            continue;
        }
        if (t.width != sceneW || t.height != sceneH) {
            _snprintf_s(s.reason, _TRUNCATE, "rejected: %llux%u != scene %llux%u",
                        t.width, t.height, sceneW, sceneH);
            out.push_back(s);
            continue;
        }

        int score = 100;
        char detail[192] = "scene-res + stencil";

        if (t.sampleCount == 1) {
            score += 20;
        } else {
            score -= 50;  // MSAA depth cannot be read back per-pixel meaningfully
            _snprintf_s(detail, _TRUNCATE, "%s; MSAA x%u penalised", detail, t.sampleCount);
        }

        // Cleared every frame is characteristic: UE clears CustomDepth even
        // when nothing renders into it, which is exactly how the candidate was
        // spotted in the RenderDoc capture.
        //
        // But WHICH plane was cleared is the discriminating part. Every depth
        // target gets its depth cleared, so a flag-blind clear counter is nearly
        // free information -- and on inZOI it was deciding a three-way tie
        // between identically-shaped 1280x800 targets, because it was the only
        // term separating them. Clearing STENCIL is the signal that someone
        // intends to write stencil, so it is scored far above a bare clear.
        if (t.stencilClearCount > 0) {
            score += 80;
            _snprintf_s(detail, _TRUNCATE, "%s; STENCIL cleared %ux", detail,
                        t.stencilClearCount);
        } else if (t.clearCount > 0) {
            score += 10;
            _snprintf_s(detail, _TRUNCATE, "%s; depth-only clear", detail);
        }

        // The most-bound target in the frame is the scene depth buffer, whose
        // stencil UE4 fully owns -- sandbox bit, lighting channels, receive-decal
        // bit, all confirmed by reading actual stencil write masks out of a
        // RenderDoc capture of this game.
        //
        // This is a HARD rejection, not a penalty. An earlier version subtracted
        // 200, which the persistence bonus then partly cancelled, leaving scene
        // depth at a positive score -- so if the real CustomDepth target ever
        // disappeared we would elect it and emit masks made of lighting-channel
        // bits. "No viable candidate" is the correct answer in that situation;
        // a confidently wrong mask is worse than none.
        if (maxBinds > 0 && t.bindCount == maxBinds && maxBinds > 8) {
            _snprintf_s(s.reason, _TRUNCATE,
                        "rejected: MOST-BOUND (%u binds) => scene depth, stencil owned by UE4",
                        t.bindCount);
            out.push_back(s);
            continue;
        }
        if (t.bindCount == 0 && t.clearCount > 0) {
            score += 40;
            _snprintf_s(detail, _TRUNCATE,
                        "%s; cleared but never bound => CustomDepth with no opt-ins",
                        detail);
        } else if (t.bindCount > 0 && t.bindCount <= 8) {
            score += 50;
            _snprintf_s(detail, _TRUNCATE, "%s; few binds (%u) => CustomDepth pass",
                        detail, t.bindCount);
        }

        // Persistence. Against Stray, the genuine CustomDepth target appeared in
        // 1096 of 1099 census blocks spanning the whole 118s session, while two
        // transient loading-screen depth buffers lived ~25s and scored
        // identically on every per-frame signal. Without this term the election
        // flipped between those two on essentially every frame.
        if (t.framesSeen >= 600) {
            score += 60;
            _snprintf_s(detail, _TRUNCATE, "%s; persistent (%u frames)", detail, t.framesSeen);
        } else if (t.framesSeen >= 120) {
            score += 30;
            _snprintf_s(detail, _TRUNCATE, "%s; seen %u frames", detail, t.framesSeen);
        } else {
            _snprintf_s(detail, _TRUNCATE, "%s; new (%u frames)", detail, t.framesSeen);
        }

        // Incumbency, but EARNED rather than granted on arrival.
        //
        // Switching targets mid-capture splits the mask stream across two
        // buffers, so a challenger must be clearly better -- that is why the
        // bonus exists, and it did stop the thrashing it was written for.
        //
        // What it also did, unintentionally, was make the first guess
        // permanent. On one run the very first election happened at frame 2,
        // when only one scene-res candidate had been observed at all. It scored
        // 200 and became the incumbent. Two frames later the real CustomDepth
        // target appeared and scored 250 on its own merits -- but 230+40 beat
        // it every frame thereafter, so the wrong target stayed elected for the
        // entire session and every mask came back empty.
        //
        // The fix is to make incumbency conditional on the incumbent actually
        // having produced a non-empty mask. A target that has never yielded a
        // single non-zero stencil pixel has no stream worth protecting, so it
        // gets no protection and the election stays free to correct itself.
        // Once a target is genuinely producing data, the bonus applies and
        // thrash protection works as designed.
        if (t.resource == electedTarget_) {
            if (electedProducedContent_) {
                score += kIncumbencyBonus;
                _snprintf_s(detail, _TRUNCATE, "%s; INCUMBENT (producing)", detail);
            } else {
                _snprintf_s(detail, _TRUNCATE, "%s; incumbent but no content yet", detail);
            }
        }

        s.score = score;
        _snprintf_s(s.reason, _TRUNCATE, "%s", detail);
        out.push_back(s);
    }

    // Ties broken by resource address, not left to sort order. Two candidates
    // scoring equally must still produce the same winner every frame -- an
    // arbitrary tie-break is a coin flip re-tossed 30 times a second.
    std::sort(out.begin(), out.end(), [](const ElectionScore& a, const ElectionScore& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.resource < b.resource;
    });
    return out;
}
// Folds this frame's observations into the accumulated evidence and returns the
// evidence, not the frame.
//
// Electing from a single frame was the cause of residual flapping even after
// persistence and incumbency were added: a candidate not bound or cleared in a
// given frame is simply absent from that frame's map, so it cannot be elected
// no matter how good it is. Two candidates alternating their absence trade the
// election back and forth. Accumulated evidence removes the whole failure mode.
std::vector<TargetFingerprint> Hooks::SnapshotTargets() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& kv : targets_) {
        const TargetFingerprint& fresh = kv.second;
        TargetFingerprint& acc = evidence_[kv.first];
        // Has this ADDRESS changed identity since we last saw it?
        //
        // `fresh` was built from a live GetDesc in NoteBind during this frame,
        // so it is always the truth about whatever occupies the address now.
        // `acc` is what we have been scoring. When they disagree on the
        // immutable properties of a resource -- format and dimensions cannot
        // change without destroying and recreating it -- the previous occupant
        // is gone and the accumulated evidence belongs to a dead resource.
        //
        // This is the whole inZOI failure. The merge below deliberately carries
        // only the per-frame counters forward, which means format/width/height
        // were never refreshed after the first sighting. UE5's pooled allocator
        // recycles render-target addresses within a frame or two, so a dead
        // depth-stencil's format stayed attached to its address permanently
        // while the new occupant's binds were merged on top of it. The election
        // then chose a "1280x800 depth-stencil" that was really a 512x512
        // R8G8B8A8_TYPELESS, and the readback failed to size a stencil plane
        // that did not exist.
        //
        // The staleness prune below cannot catch this: the address keeps being
        // bound by its NEW owner, so lastSeenFrame keeps refreshing and the
        // entry never goes quiet. Identity has to be checked, not just liveness.
        const bool identityChanged =
            acc.resource != nullptr &&
            (acc.format != fresh.format || acc.width != fresh.width ||
             acc.height != fresh.height || acc.sampleCount != fresh.sampleCount);
        if (identityChanged) {
            ++addressRecycles_;
            if (addressRecycles_ <= 8 || (addressRecycles_ % 100) == 0) {
                LogWarn("address %p changed identity (%llux%u %s -> %llux%u %s); "
                        "discarding %llu frames of evidence (recycle #%llu)",
                        static_cast<void*>(kv.first), acc.width, acc.height,
                        FormatName(acc.format), fresh.width, fresh.height,
                        FormatName(fresh.format), acc.framesSeen, addressRecycles_);
            }
            if (kv.first == electedTarget_) {
                ++electedRecycles_;
                LogWarn("the ELECTED target %p is the one that changed identity; "
                        "election voided", static_cast<void*>(kv.first));
                electedTarget_ = nullptr;
                electedProducedContent_ = false;
            }
            resourceState_.erase(kv.first);
            barriersSeen_.erase(kv.first);
            textureLayout_.erase(kv.first);
            acc = TargetFingerprint{};
        }

        if (acc.resource == nullptr) {
            acc = fresh;
            acc.framesSeen = 0;
        } else {
            // Carry the latest per-frame counts forward; the scoring thresholds
            // are expressed per frame, so accumulating them would misclassify a
            // long-lived CustomDepth pass as the most-bound target.
            acc.bindCount = fresh.bindCount;
            acc.clearCount = fresh.clearCount;
            acc.stencilClearCount = fresh.stencilClearCount;
            acc.firstBindOrdinal = fresh.firstBindOrdinal;
            acc.everBoundAsDepth = acc.everBoundAsDepth || fresh.everBoundAsDepth;
        }
        ++acc.framesSeen;
        acc.lastSeenFrame = frameIndex_;
    }

    // Prune resources that have gone quiet. Without this, a destroyed
    // loading-screen buffer would keep winning on accumulated persistence long
    // after it stopped existing -- and its pointer could be reused by an
    // unrelated allocation.
    constexpr uint64_t kStaleAfterFrames = 600;
    for (auto it = evidence_.begin(); it != evidence_.end();) {
        if (frameIndex_ > it->second.lastSeenFrame + kStaleAfterFrames) {
            it = evidence_.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<TargetFingerprint> out;
    out.reserve(evidence_.size());
    for (const auto& kv : evidence_) out.push_back(kv.second);
    return out;
}

}  // namespace segcap
