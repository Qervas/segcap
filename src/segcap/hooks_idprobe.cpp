#include "hooks.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "customdepth.h"
#include "log.h"

// Linker-provided base of this module; avoids needing the HMODULE passed around
// just to find where our own DLL lives.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace segcap {
// Reports what is actually inside a scene-resolution integer render target.
//
// The question this answers is not "does a buffer exist" -- the census already
// said yes -- but "does it contain per-object identity". Three signals separate
// the plausible answers, and none of them require knowing UE's internals:
//
//   how many distinct values      a handful  -> material or shading class
//                                 hundreds+  -> per-object or per-cluster ids
//   how the top values cluster    contiguous regions -> spatial objects
//                                 scattered          -> not per-object
//   whether 0 dominates           a mostly-empty buffer is a mask of something
//                                 specific, not a general id buffer
//
// A PGM is written alongside, colouring by value hash, because a histogram
// cannot show whether the regions line up with objects and a picture can.
void Hooks::OnIdBufferReady(const MaskFrame& frame) {
    // 1, 2 and 4 bytes per pixel are all legitimate id-buffer widths and this
    // used to reject everything but 4. inZOI presents an R8_UINT at exactly
    // scene resolution, bound once per frame -- an 8-bit single-channel integer
    // target whose range is precisely our 1..255 slot range, i.e. the single
    // most promising candidate in the whole census -- and the width check would
    // have discarded it with a one-line warning.
    if (frame.bytesPerPixel != 1 && frame.bytesPerPixel != 2 && frame.bytesPerPixel != 4) {
        LogWarn("idbuf: unsupported %u bytes/pixel", frame.bytesPerPixel);
        return;
    }
    ++idDumped_;

    // ---- does THIS candidate hold OUR ids? ---------------------------------
    //
    // The whole reason to enumerate candidates is to answer this by measurement.
    // A 4-byte integer texel is read three ways -- as R16 (low half), as G16
    // (high half), and as the whole 32-bit value -- because which channel
    // carries the stencil depends on the format and on the platform's channel
    // order. UE's Nanite export puts the custom stencil in .g on D3D.
    //
    // The test is the same one that guards the mask path: a value we never
    // leased cannot be ours. A channel where most labelled texels ARE leased
    // slots is the id channel, and that is a conclusion, not a guess.
    if (auto sc = GetMarker().publishedSidecar()) {
        // Bit-packed (8 KB) rather than a 64 KB bool array: this runs on the
        // render thread, and 64 KB of stack per frame is not worth the risk.
        std::vector<bool> leased(65536, false);
        size_t leasedCount = 0;
        for (const auto& b : sc->bindings) {
            if (b.slot > 0 && b.slot < 65536) {
                if (!leased[b.slot]) ++leasedCount;
                leased[b.slot] = true;
            }
        }
        if (leasedCount > 0) {
            uint64_t nonZero[3] = {0, 0, 0};
            uint64_t hit[3] = {0, 0, 0};
            // Per-channel histogram over LEASED values only, so the acceptance
            // test can ask "how many of our slots actually appear, and is any one
            // of them carrying the whole match".
            std::vector<std::unordered_map<uint32_t, uint64_t>> leasedHist(3);
            const uint32_t bpp = frame.bytesPerPixel;
            for (uint32_t y = 0; y < frame.height; ++y) {
                const uint8_t* row = frame.data + static_cast<size_t>(y) * frame.rowPitch;
                for (uint32_t x = 0; x < frame.width; ++x) {
                    const uint8_t* p = row + static_cast<size_t>(x) * bpp;
                    // Decompose the texel every way an id could plausibly be
                    // stored at this width. Which channel actually carries the
                    // stencil depends on format and platform channel order --
                    // UE's Nanite export puts it in .g on D3D -- so all of them
                    // are measured and the data picks the winner.
                    uint32_t chan[3] = {0, 0, 0};
                    if (bpp == 1) {
                        chan[0] = p[0];
                    } else if (bpp == 2) {
                        chan[0] = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
                        chan[1] = p[0];
                        chan[2] = p[1];
                    } else {
                        const uint32_t v = static_cast<uint32_t>(p[0]) |
                                           (static_cast<uint32_t>(p[1]) << 8) |
                                           (static_cast<uint32_t>(p[2]) << 16) |
                                           (static_cast<uint32_t>(p[3]) << 24);
                        chan[0] = v & 0xFFFFu;
                        chan[1] = (v >> 16) & 0xFFFFu;
                        chan[2] = v;
                    }
                    for (int c = 0; c < 3; ++c) {
                        if (!chan[c]) continue;
                        ++nonZero[c];
                        if (chan[c] < 65536 && leased[chan[c]]) {
                            ++hit[c];
                            ++leasedHist[c][chan[c]];
                        }
                    }
                }
            }
            const char* names1[3] = {"R8", "-", "-"};
            const char* names2[3] = {"R16", "byte0", "byte1"};
            const char* names4[3] = {"R16(low)", "G16(high)", "R32(whole)"};
            const char* const* names = bpp == 1 ? names1 : (bpp == 2 ? names2 : names4);
            uint32_t distinctLeasedSeen[3] = {0, 0, 0};
            uint64_t topLeasedValueCount[3] = {0, 0, 0};
            for (int c = 0; c < 3; ++c) {
                distinctLeasedSeen[c] = static_cast<uint32_t>(leasedHist[c].size());
                for (const auto& kv : leasedHist[c]) {
                    if (kv.second > topLeasedValueCount[c]) topLeasedValueCount[c] = kv.second;
                }
            }
            int bestChan = -1;
            double bestFrac = 0.0;
            for (int c = 0; c < 3; ++c) {
                const double frac =
                    nonZero[c] ? static_cast<double>(hit[c]) / static_cast<double>(nonZero[c])
                               : 0.0;
                LogInfo("idbuf   channel %-10s: %llu non-zero texels, %.1f%% carry a slot we "
                        "leased",
                        names[c], nonZero[c], 100.0 * frac);
                if (frac > bestFrac) { bestFrac = frac; bestChan = c; }
            }
            // A high match fraction is NOT sufficient on its own, and believing
            // it was cost a full misdiagnosis. With few slots leased, and
            // especially when the leased set is a low contiguous range, a buffer
            // of ordinary small integers matches trivially: the measured "100.0%"
            // was carried by values 1, 2 and 3 covering 58.7% of the screen,
            // while pixels genuinely from our marks were 0.41%.
            //
            // Slots are now issued in a scattered order (IdentityRegistry::
            // LeaseSlot), so an honest match must ALSO reproduce the scatter.
            // Three independent conditions, all cheap:
            //   - enough distinct leased values actually present, so one dominant
            //     value cannot carry the verdict
            //   - no single value dominating the "match", for the same reason
            //   - a decent share of the leased set observed, not just its head
            constexpr double kIdChannelThreshold = 0.90;
            constexpr uint32_t kMinDistinctLeasedSeen = 6;
            constexpr double kMaxSingleValueShare = 0.60;
            const bool enoughDistinct = bestChan >= 0 && distinctLeasedSeen[bestChan] >= kMinDistinctLeasedSeen;
            const double dominance =
                (bestChan >= 0 && hit[bestChan])
                    ? static_cast<double>(topLeasedValueCount[bestChan]) /
                          static_cast<double>(hit[bestChan])
                    : 1.0;
            const bool notDominated = dominance <= kMaxSingleValueShare;
            if (bestChan >= 0 && bestFrac >= kIdChannelThreshold && !enoughDistinct) {
                LogInfo("idbuf: %p channel %s matched %.1f%% but only %u distinct leased "
                        "values appear -- too few to distinguish real ids from ordinary "
                        "small integers; not accepting",
                        static_cast<void*>(idTarget_), names[bestChan], 100.0 * bestFrac,
                        distinctLeasedSeen[bestChan]);
            } else if (bestChan >= 0 && bestFrac >= kIdChannelThreshold && !notDominated) {
                LogInfo("idbuf: %p channel %s matched %.1f%% but %.0f%% of the matching "
                        "texels are a single value -- that is one big region, not a set of "
                        "object ids; not accepting",
                        static_cast<void*>(idTarget_), names[bestChan], 100.0 * bestFrac,
                        100.0 * dominance);
            } else if (bestChan >= 0 && bestFrac >= kIdChannelThreshold && enoughDistinct &&
                       notDominated) {
                LogWarn("idbuf: FOUND OUR IDS in %p channel %s (%.1f%% of non-zero texels are "
                        "leased slots, %zu slots leased). This is the per-object id buffer.",
                        static_cast<void*>(idTarget_), names[bestChan], 100.0 * bestFrac,
                        leasedCount);
                idChannelFound_ = true;
                // Switch the MASK pipeline onto this buffer. On a Nanite title
                // the depth-stencil route cannot work at all: Nanite exports
                // depth to CombinedCustomDepth (whose stencil plane is never
                // written) and the stencil VALUE to this separate colour target,
                // which the election was hard-rejecting as "no stencil plane".
                maskSource_ = idTarget_;
                maskChannel_ = bestChan;
                maskSourceBpp_ = bpp;
                // Pin it, for the same reason the elected depth-stencil is
                // pinned: D3D12 recycles addresses and UE5's transient allocator
                // reissues heap ranges within a frame, and this is now the one
                // resource every captured mask comes from.
                maskSource_->AddRef();
                LogWarn("capture: mask source switched to the id buffer %p (channel %s). "
                        "The CustomDepth stencil-plane route does not carry ids on this "
                        "title.",
                        static_cast<void*>(maskSource_), names[bestChan]);
            } else if (idDumped_ >= kIdProbeAttemptsPerCandidate) {
                // Abandon this candidate permanently and pick another next
                // frame. Recorded in idRejected_ rather than by index, because
                // the candidate list is rebuilt each time and an index would
                // point at a different resource once new targets appear.
                LogInfo("idbuf: candidate %p holds none of our slots (best %.1f%% over %zu "
                        "leased); rejected, will try another",
                        static_cast<void*>(idTarget_), 100.0 * bestFrac, leasedCount);
                idRejected_.insert(idTarget_);
                idTarget_ = nullptr;
                idDumped_ = 0;
            }
        }
    }

    std::unordered_map<uint32_t, uint32_t> hist;
    hist.reserve(4096);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.data + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * frame.bytesPerPixel;
            uint32_t v = 0;
            for (uint32_t b = 0; b < frame.bytesPerPixel; ++b) {
                v |= static_cast<uint32_t>(p[b]) << (8 * b);
            }
            ++hist[v];
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> top(hist.begin(), hist.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    const size_t total = static_cast<size_t>(frame.width) * frame.height;
    LogInfo("idbuf frame %llu: %ux%u fmt=%u -- %zu DISTINCT VALUES over %zu pixels",
            frame.frameIndex, frame.width, frame.height, frame.format, hist.size(), total);
    for (size_t i = 0; i < top.size() && i < 12; ++i) {
        LogInfo("idbuf   0x%08X  %8u px  %5.2f%%", top[i].first, top[i].second,
                100.0 * top[i].second / static_cast<double>(total));
    }

    // Visualise by hashing the value into a byte. Distinct ids become distinct
    // greys; a buffer with four values looks like four flat regions and a
    // per-object buffer looks like the scene.
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\segcap_idbuf_%llu.pgm", dir.c_str(), frame.frameIndex);
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    std::fprintf(f, "P5\n%u %u\n255\n", frame.width, frame.height);
    std::vector<uint8_t> row(frame.width);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const auto* src =
            reinterpret_cast<const uint32_t*>(frame.data + static_cast<size_t>(y) * frame.rowPitch);
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint32_t v = src[x];
            // Cheap avalanche so neighbouring ids get far-apart greys.
            uint32_t h = v * 2654435761u;
            h ^= h >> 16;
            row[x] = v == 0 ? 0 : static_cast<uint8_t>(16 + (h % 240));
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LogInfo("idbuf: wrote %ls", path);
}

}  // namespace segcap
