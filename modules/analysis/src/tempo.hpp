#pragma once

// An approximate tempo from onset times. The gap between neighbouring onsets follows the fastest notes, such as a
// hi-hat, not the beat. So this looks for the period that the gaps between all recent onsets, near and far, keep
// falling on. A tempo and its double fit the same onsets, so a preference for normal tempos picks between them,
// and the answer can still be half or double what a listener would tap.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace dsptb::analysis {

// Onset needs `double t` in seconds and `float strength`. A strong pair counts for more, so kicks and snares outweigh
// hats. Returns 0 until there's enough to go on.
template <class Onsets> double estimateTempoBpm(const Onsets& onsets) {
    constexpr double kWindowSeconds = 30.0;
    constexpr size_t kMinOnsets = 8;
    constexpr double kMinBpm = 60.0, kMaxBpm = 200.0, kBpmStep = 0.5;
    constexpr int kBeatsCompared = 4;
    constexpr double kBinSeconds = 0.005;
    constexpr double kMaxGapSeconds = kBeatsCompared * 60.0 / kMinBpm;
    constexpr size_t kBins = static_cast<size_t>(kMaxGapSeconds / kBinSeconds) + 4;
    constexpr double kPreferredBpm = 120.0, kPreferenceOctaves = 0.9;
    constexpr double kMinBeatShare = 0.25;

    const size_t count = onsets.size();
    if (count < kMinOnsets)
        return 0.0;
    size_t first = count;
    while (first > 0 && onsets[count - 1].t - onsets[first - 1].t <= kWindowSeconds)
        --first;
    if (count - first < kMinOnsets)
        return 0.0;

    std::array<double, kBins> gaps{};
    for (size_t a = first; a < count; ++a)
        for (size_t b = a + 1; b < count; ++b) {
            const double gap = onsets[b].t - onsets[a].t;
            if (gap > kMaxGapSeconds)
                break;
            const auto bin = static_cast<size_t>(std::lround(gap / kBinSeconds));
            gaps[bin] += std::sqrt(static_cast<double>(std::max(onsets[a].strength, 0.0f)) *
                                   static_cast<double>(std::max(onsets[b].strength, 0.0f)));
        }

    // Spreads each gap over neighbouring bins, because onset times carry some jitter.
    std::array<double, kBins> smooth{};
    for (size_t i = 2; i + 2 < kBins; ++i)
        smooth[i] = gaps[i - 2] + 2.0 * gaps[i - 1] + 3.0 * gaps[i] + 2.0 * gaps[i + 1] + gaps[i + 2];

    const auto binOf = [&](double seconds) { return static_cast<size_t>(std::lround(seconds / kBinSeconds)); };
    double strongestBeatGap = 0.0;
    for (size_t i = binOf(60.0 / kMaxBpm); i <= binOf(60.0 / kMinBpm); ++i)
        strongestBeatGap = std::max(strongestBeatGap, smooth[i]);

    double bestBpm = 0.0, bestScore = 0.0;
    for (double bpm = kMinBpm; bpm <= kMaxBpm; bpm += kBpmStep) {
        // A beat must be a gap that really occurs. Clicks at 60 BPM fit 120 too, with nothing on the half-beats.
        if (smooth[binOf(60.0 / bpm)] < kMinBeatShare * strongestBeatGap)
            continue;
        double score = 0.0;
        for (int beat = 1; beat <= kBeatsCompared; ++beat)
            score += smooth[binOf(beat * 60.0 / bpm)];
        const double octaves = std::log2(bpm / kPreferredBpm) / kPreferenceOctaves;
        score *= std::exp(-0.5 * octaves * octaves);
        if (score > bestScore) {
            bestScore = score;
            bestBpm = bpm;
        }
    }
    return bestBpm;
}

} // namespace dsptb::analysis
