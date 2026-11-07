// SpectralFluxOnset tests.
//
// Detection quality is measured against ground-truth event times with a
// one-to-one greedy matcher (each click takes its nearest still-unmatched
// detection inside the tolerance window); the measured rates are printed
// as evidence alongside the CHECKs.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/onset.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

using dsptb::dsp::OnsetConfig;
using dsptb::dsp::OnsetEvent;
using dsptb::dsp::SpectralFluxOnset;
using dsptb::test::signals::LcgNoise;

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kFft = 1024;
constexpr std::size_t kHop = 256;

struct RunResult {
    std::vector<OnsetEvent> events;
    std::vector<float> novelty;
    std::vector<std::uint64_t> noveltyFrames;
};

RunResult runWhole(SpectralFluxOnset& det, const std::vector<float>& signal) {
    RunResult out;
    det.push(signal.data(), signal.size(),
             [&out](float value, std::uint64_t frame) {
                 out.novelty.push_back(value);
                 out.noveltyFrames.push_back(frame);
             },
             [&out](const OnsetEvent& event) { out.events.push_back(event); });
    return out;
}

RunResult runChunked(SpectralFluxOnset& det, const std::vector<float>& signal, std::size_t chunk) {
    RunResult out;
    auto onNovelty = [&out](float value, std::uint64_t frame) {
        out.novelty.push_back(value);
        out.noveltyFrames.push_back(frame);
    };
    auto onOnset = [&out](const OnsetEvent& event) { out.events.push_back(event); };
    std::size_t pos = 0;
    while (pos < signal.size()) {
        const std::size_t take = std::min(chunk, signal.size() - pos);
        det.push(signal.data() + pos, take, onNovelty, onOnset);
        pos += take;
    }
    return out;
}

// clickTrain(bpm, amp) for `seconds` seconds, built sample by sample from
// the spec §3 helper. The sample rate is a parameter so the same criterion
// can be exercised at 44.1 kHz (the frame-centre ruling on PR #26).
std::vector<float> clickSignal(double bpm, double seconds, double amp, double fs = kFs) {
    const std::size_t count = static_cast<std::size_t>(seconds * fs);
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n)
        out[n] = dsptb::test::signals::clickTrain(n, fs, bpm, amp);
    return out;
}

// Ground-truth click positions: n = round(k * 60/bpm * fs), as in §3.
std::vector<std::size_t> clickTimes(double bpm, double seconds, double fs = kFs) {
    std::vector<std::size_t> times;
    const double period = 60.0 / bpm * fs;
    const std::size_t total = static_cast<std::size_t>(seconds * fs);
    for (std::size_t k = 0;; ++k) {
        const std::size_t t =
            static_cast<std::size_t>(std::llround(static_cast<double>(k) * period));
        if (t >= total)
            break;
        times.push_back(t);
    }
    return times;
}

// The noisy acceptance signal: 120 bpm clicks plus lcgNoise at -30 dBFS.
std::vector<float> noisyClickSignal() {
    std::vector<float> signal = clickSignal(120.0, 30.0, 1.0);
    LcgNoise noise(1234u, std::pow(10.0, -30.0 / 20.0));
    for (float& sample : signal)
        sample = static_cast<float>(static_cast<double>(sample) + static_cast<double>(noise.next()));
    return signal;
}

struct MatchStats {
    std::size_t matched = 0;        // ground-truth events with a detection in the window
    std::size_t doubles = 0;        // ground-truth events with more than one detection in the window
    std::size_t falsePositives = 0; // detections not matched to any ground-truth event
    double maxAbsErrorMs = 0.0;     // worst |detection - truth| over the matched pairs
};

// One-to-one matching: each ground-truth event, in time order, takes its
// nearest still-unmatched detection within +/- toleranceMs.
MatchStats matchEvents(const std::vector<OnsetEvent>& events, const std::vector<std::size_t>& truthSamples,
                       double toleranceMs, double fs = kFs) {
    MatchStats stats;
    const double tolSamples = toleranceMs / 1000.0 * fs;
    std::vector<bool> used(events.size(), false);
    for (const std::size_t truth : truthSamples) {
        std::size_t inWindow = 0;
        std::size_t best = events.size();
        double bestErr = 0.0;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const double detSamples = events[i].timeSeconds * fs;
            const double err = std::abs(detSamples - static_cast<double>(truth));
            if (err > tolSamples)
                continue;
            ++inWindow;
            if (!used[i] && (best == events.size() || err < bestErr)) {
                best = i;
                bestErr = err;
            }
        }
        if (inWindow > 1)
            ++stats.doubles;
        if (best != events.size()) {
            used[best] = true;
            ++stats.matched;
            stats.maxAbsErrorMs = std::max(stats.maxAbsErrorMs, bestErr / fs * 1000.0);
        }
    }
    for (const bool u : used)
        if (!u)
            ++stats.falsePositives;
    return stats;
}

} // namespace

TEST_CASE("onset: invalid configurations are rejected") {
    const OnsetConfig good;
    CHECK_NOTHROW(SpectralFluxOnset(kFs, good));
    CHECK_THROWS_AS(SpectralFluxOnset(0.0, good), std::invalid_argument);
    CHECK_THROWS_AS(SpectralFluxOnset(-48000.0, good), std::invalid_argument);
    OnsetConfig bad = good;
    bad.hop = 0;
    CHECK_THROWS_AS(SpectralFluxOnset(kFs, bad), std::invalid_argument);
    bad = good;
    bad.medianFrames = 0;
    CHECK_THROWS_AS(SpectralFluxOnset(kFs, bad), std::invalid_argument);
    bad = good;
    bad.medianFrames = 64; // spec §4.7: M <= 63
    CHECK_THROWS_AS(SpectralFluxOnset(kFs, bad), std::invalid_argument);
    bad = good;
    bad.compression = 0.0f;
    CHECK_THROWS_AS(SpectralFluxOnset(kFs, bad), std::invalid_argument);
    bad = good;
    bad.fftSize = 1000; // not a power of two (rejected by RealFft)
    CHECK_THROWS_AS(SpectralFluxOnset(kFs, bad), std::invalid_argument);
}

TEST_CASE("onset: silence produces zero novelty and no onsets") {
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const std::vector<float> silence(48000, 0.0f);
    const RunResult r = runWhole(det, silence);
    // One novelty value per completed frame: (48000 - 1024) / 256 + 1 = 184.
    REQUIRE(r.novelty.size() == 184u);
    for (std::size_t i = 0; i < r.novelty.size(); ++i) {
        CHECK(r.noveltyFrames[i] == i);
        CHECK(r.novelty[i] == 0.0f);
    }
    CHECK(r.events.empty());
}

TEST_CASE("onset: clickTrain(120 bpm), 30 s, >= 95% detected within +/-20 ms, <= 1 onset per click") {
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const std::vector<float> signal = clickSignal(120.0, 30.0, 1.0);
    const std::vector<std::size_t> truth = clickTimes(120.0, 30.0);
    REQUIRE(truth.size() == 60u);
    const RunResult r = runWhole(det, signal);
    const MatchStats m = matchEvents(r.events, truth, 20.0);
    const double matchedPct = 100.0 * static_cast<double>(m.matched) / static_cast<double>(truth.size());
    std::printf("[onset] clean clicks: %zu/%zu detected (%.1f%%), doubles %zu, false positives %zu, "
                "worst timing error %.2f ms\n",
                m.matched, truth.size(), matchedPct, m.doubles, m.falsePositives, m.maxAbsErrorMs);
    CHECK(m.matched * 20 >= truth.size() * 19); // >= 95%
    CHECK(m.doubles == 0u);
    CHECK(m.falsePositives == 0u);
}

TEST_CASE("onset: timeSeconds reports the frame centre exactly") {
    // Ruling on PR #26 (2026-11-05): OnsetEvent::timeSeconds is
    // (frameIndex*hop + fftSize/2)/fs — the frame centre, not the frame
    // start. Asserted exactly (same expression as the implementation) on
    // every event of a click train.
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const std::vector<float> signal = clickSignal(120.0, 30.0, 1.0);
    const RunResult r = runWhole(det, signal);
    REQUIRE(!r.events.empty());
    const OnsetConfig cfg;
    for (const OnsetEvent& event : r.events) {
        const double expected =
            (static_cast<double>(event.frameIndex) * static_cast<double>(cfg.hop) +
             static_cast<double>(cfg.fftSize) / 2.0) /
            kFs;
        CHECK(event.timeSeconds == expected);
    }
}

TEST_CASE("onset: clickTrain(120 bpm) at 44.1 kHz, >= 95% detected within +/-20 ms") {
    // With the frame-centre rule: the same
    // criterion at 44.1 kHz, where the old frame-start convention would
    // have failed (worst offset ~20.3 ms > 20 ms). With the centre the
    // bias is roughly halved at any sample rate.
    constexpr double fs441 = 44100.0;
    SpectralFluxOnset det(fs441, OnsetConfig{});
    const std::vector<float> signal = clickSignal(120.0, 30.0, 1.0, fs441);
    const std::vector<std::size_t> truth = clickTimes(120.0, 30.0, fs441);
    REQUIRE(truth.size() == 60u);
    const RunResult r = runWhole(det, signal);
    const MatchStats m = matchEvents(r.events, truth, 20.0, fs441);
    const double matchedPct = 100.0 * static_cast<double>(m.matched) / static_cast<double>(truth.size());
    std::printf("[onset] clean clicks at 44.1 kHz: %zu/%zu detected (%.1f%%), doubles %zu, false "
                "positives %zu, worst timing error %.2f ms\n",
                m.matched, truth.size(), matchedPct, m.doubles, m.falsePositives, m.maxAbsErrorMs);
    CHECK(m.matched * 20 >= truth.size() * 19); // >= 95%
    CHECK(m.doubles == 0u);
    CHECK(m.falsePositives == 0u);
}

TEST_CASE("onset: clicks + lcgNoise at -30 dBFS, >= 90% detected, <= 2% false positives") {
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const std::vector<float> signal = noisyClickSignal();
    const std::vector<std::size_t> truth = clickTimes(120.0, 30.0);
    const RunResult r = runWhole(det, signal);
    const MatchStats m = matchEvents(r.events, truth, 20.0);
    const double fpRate =
        r.events.empty() ? 0.0
                         : static_cast<double>(m.falsePositives) / static_cast<double>(r.events.size());
    const double matchedPct = 100.0 * static_cast<double>(m.matched) / static_cast<double>(truth.size());
    std::printf("[onset] noisy clicks: %zu/%zu detected (%.1f%%), %zu detections, false positives %zu "
                "(%.2f%%), worst timing error %.2f ms\n",
                m.matched, truth.size(), matchedPct, r.events.size(), m.falsePositives, 100.0 * fpRate,
                m.maxAbsErrorMs);
    CHECK(m.matched * 10 >= truth.size() * 9); // >= 90%
    CHECK(fpRate <= 0.02);
}

TEST_CASE("onset: a steady 440 Hz sine produces no onsets after the first 100 ms") {
    SpectralFluxOnset det(kFs, OnsetConfig{});
    std::vector<float> signal(480000);
    for (std::size_t n = 0; n < signal.size(); ++n)
        signal[n] = dsptb::test::signals::sine(n, kFs, 440.0, 0.5, 0.0);
    const RunResult r = runWhole(det, signal);
    std::printf("[onset] steady sine: %zu onsets total, all within the first 100 ms: %s\n", r.events.size(),
                r.events.empty() || r.events.back().timeSeconds < 0.1 ? "yes" : "NO");
    // (480000 - 1024) / 256 + 1 = 1872 novelty values over the 10 s.
    REQUIRE(r.novelty.size() == 1872u);
    for (const OnsetEvent& event : r.events)
        CHECK(event.timeSeconds < 0.1);
}

TEST_CASE("onset: amplitude steps -40 -> -10 dBFS are detected within +/-20 ms") {
    // A 440 Hz sine at -40 dBFS jumping to -10 dBFS at t = 2, 4, 6, 8 s
    // (each loud section lasts 1 s, -40 dBFS in between; the first frame of
    // the signal is at -40 dBFS, so all four steps are up-steps).
    const double lowAmp = std::pow(10.0, -40.0 / 20.0);
    const double highAmp = std::pow(10.0, -10.0 / 20.0);
    std::vector<float> signal(480000);
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const double t = static_cast<double>(n) / kFs;
        const bool high = (t >= 2.0 && t < 3.0) || (t >= 4.0 && t < 5.0) || (t >= 6.0 && t < 7.0) || t >= 8.0;
        signal[n] = dsptb::test::signals::sine(n, kFs, 440.0, high ? highAmp : lowAmp, 0.0);
    }
    const std::vector<std::size_t> truth = {96000, 192000, 288000, 384000};
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const RunResult r = runWhole(det, signal);
    const MatchStats m = matchEvents(r.events, truth, 20.0);
    std::printf("[onset] amplitude steps: %zu/%zu detected, worst timing error %.2f ms\n", m.matched,
                truth.size(), m.maxAbsErrorMs);
    CHECK(m.matched == truth.size());
}

TEST_CASE("onset: minIntervalMs is respected") {
    // Clicks every 100 ms (600 bpm) with a 250 ms minimum interval: the
    // detector must space its onsets at least 250 ms apart, and must report
    // fewer onsets than with the default 50 ms interval.
    const std::vector<float> signal = clickSignal(600.0, 5.0, 1.0);
    OnsetConfig config;
    config.minIntervalMs = 250.0;
    SpectralFluxOnset det(kFs, config);
    const RunResult r = runWhole(det, signal);
    REQUIRE(r.events.size() >= 5u);
    for (std::size_t i = 1; i < r.events.size(); ++i)
        CHECK((r.events[i].timeSeconds - r.events[i - 1].timeSeconds) * 1000.0 >= 250.0 - 1e-6);

    SpectralFluxOnset detDefault(kFs, OnsetConfig{});
    const RunResult rDefault = runWhole(detDefault, signal);
    std::printf("[onset] minInterval: %zu onsets at 250 ms minimum vs %zu at the 50 ms default\n",
                r.events.size(), rDefault.events.size());
    CHECK(rDefault.events.size() > r.events.size());
}

TEST_CASE("onset: raising lambda reduces detections monotonically on the noisy test") {
    const std::vector<float> signal = noisyClickSignal();
    const float lambdas[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
    std::size_t counts[5] = {};
    for (std::size_t i = 0; i < 5; ++i) {
        OnsetConfig config;
        config.lambda = lambdas[i];
        SpectralFluxOnset det(kFs, config);
        counts[i] = runWhole(det, signal).events.size();
    }
    std::printf("[onset] lambda sweep on noisy clicks: %zu, %zu, %zu, %zu, %zu detections\n", counts[0],
                counts[1], counts[2], counts[3], counts[4]);
    for (std::size_t i = 1; i < 5; ++i)
        CHECK(counts[i] <= counts[i - 1]);
}

TEST_CASE("onset: setSensitivity with an extreme lambda suppresses the clicks") {
    const std::vector<float> signal = noisyClickSignal();
    SpectralFluxOnset det(kFs, OnsetConfig{});
    det.setSensitivity(50.0f, 0.02f);
    const RunResult r = runWhole(det, signal);
    // Every click is suppressed: once the median history holds real novelty
    // values the threshold is ~50x the noise median. The single event that
    // remains is the frame-0 stream-start transient — the causal median is
    // still empty there, so theta_0 = delta whatever lambda is (the same
    // reason the spec's sine criterion tolerates onsets in the first
    // 100 ms).
    CHECK(r.events.size() <= 1u);
    if (!r.events.empty())
        CHECK(r.events[0].frameIndex == 0u);
}

TEST_CASE("onset: latencyFrames() equals the measured detection delay (within one hop)") {
    // Two isolated clicks on silence. The samples are pushed in hop-sized
    // chunks and `consumed` counts the samples pushed when each callback
    // fires: the measured delay of an event is how many samples after its
    // reported frame start the detector could possibly report it. With the
    // declared one frame of look-ahead this must be exactly fftSize + hop.
    const std::vector<std::size_t> clickSamples = {48000, 96000};
    std::vector<float> signal(144000, 0.0f);
    for (const std::size_t c : clickSamples)
        signal[c] = 1.0f;

    SpectralFluxOnset det(kFs, OnsetConfig{});
    CHECK(det.latencyFrames() == kFft + kHop);

    std::vector<double> delays;
    std::vector<std::uint64_t> eventFrames;
    std::size_t consumed = 0;
    auto onNovelty = [](float, std::uint64_t) {};
    auto onOnset = [&delays, &eventFrames, &consumed](const OnsetEvent& event) {
        delays.push_back(static_cast<double>(consumed) -
                         static_cast<double>(event.frameIndex) * static_cast<double>(kHop));
        eventFrames.push_back(event.frameIndex);
    };
    std::size_t pos = 0;
    while (pos < signal.size()) {
        const std::size_t take = std::min(kHop, signal.size() - pos);
        consumed += take;
        det.push(signal.data() + pos, take, onNovelty, onOnset);
        pos += take;
    }
    REQUIRE(delays.size() == 2u);
    for (std::size_t i = 0; i < delays.size(); ++i) {
        CHECK(delays[i] == static_cast<double>(det.latencyFrames()));
        // The reported frame is the first frame whose novelty peaks for this
        // click: its start precedes the click by less than one frame.
        const double frameStart = static_cast<double>(eventFrames[i]) * static_cast<double>(kHop);
        CHECK(static_cast<double>(clickSamples[i]) - frameStart >= 0.0);
        CHECK(static_cast<double>(clickSamples[i]) - frameStart < static_cast<double>(kFft));
    }
    std::printf("[onset] latency: measured delays %.0f and %.0f samples, latencyFrames() = %zu\n", delays[0],
                delays[1], det.latencyFrames());

    // With an awkward chunk size the frame still completes mid-push, so the
    // measured delay may overshoot latencyFrames() by less than one chunk —
    // here 100 samples, inside the +/- 1 hop tolerance.
    SpectralFluxOnset det2(kFs, OnsetConfig{});
    std::vector<double> delays2;
    std::size_t consumed2 = 0;
    auto onOnset2 = [&delays2, &consumed2](const OnsetEvent& event) {
        delays2.push_back(static_cast<double>(consumed2) -
                          static_cast<double>(event.frameIndex) * static_cast<double>(kHop));
    };
    pos = 0;
    while (pos < signal.size()) {
        const std::size_t take = std::min(std::size_t{100}, signal.size() - pos);
        consumed2 += take;
        det2.push(signal.data() + pos, take, onNovelty, onOnset2);
        pos += take;
    }
    REQUIRE(delays2.size() == 2u);
    for (const double d : delays2) {
        CHECK(d >= static_cast<double>(det2.latencyFrames()));
        CHECK(d <= static_cast<double>(det2.latencyFrames() + kHop));
    }
}

TEST_CASE("onset: results are identical for any push chunk size") {
    std::vector<float> signal = clickSignal(120.0, 5.0, 1.0);
    LcgNoise noise(7u, std::pow(10.0, -30.0 / 20.0));
    for (float& sample : signal)
        sample = static_cast<float>(static_cast<double>(sample) + static_cast<double>(noise.next()));

    SpectralFluxOnset detRef(kFs, OnsetConfig{});
    const RunResult ref = runWhole(detRef, signal);
    REQUIRE(!ref.events.empty());

    const std::size_t chunkSizes[] = {1, 7, 100, 1023, 5000};
    for (const std::size_t chunk : chunkSizes) {
        SpectralFluxOnset det(kFs, OnsetConfig{});
        const RunResult r = runChunked(det, signal, chunk);
        REQUIRE(r.novelty.size() == ref.novelty.size());
        REQUIRE(r.events.size() == ref.events.size());
        for (std::size_t i = 0; i < ref.novelty.size(); ++i) {
            CHECK(r.novelty[i] == ref.novelty[i]); // bit-identical: same operations, same order
            CHECK(r.noveltyFrames[i] == ref.noveltyFrames[i]);
        }
        for (std::size_t i = 0; i < ref.events.size(); ++i) {
            CHECK(r.events[i].frameIndex == ref.events[i].frameIndex);
            CHECK(r.events[i].timeSeconds == ref.events[i].timeSeconds);
            CHECK(r.events[i].strength == ref.events[i].strength);
        }
    }
}

TEST_CASE("onset: push does not allocate") {
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const std::vector<float> warm = clickSignal(120.0, 1.0, 1.0);
    const std::vector<float> signal = clickSignal(120.0, 2.0, 1.0);
    auto noopNovelty = [](float, std::uint64_t) {};
    auto noopOnset = [](const OnsetEvent&) {};
    det.push(warm.data(), warm.size(), noopNovelty, noopOnset); // warm-up, unguarded

    // Fixed-size sinks: the callbacks must not allocate either.
    float noveltySink[1024];
    OnsetEvent eventSink[64];
    std::size_t noveltyCount = 0;
    std::size_t eventCount = 0;
    auto onNovelty = [&noveltySink, &noveltyCount](float value, std::uint64_t) {
        if (noveltyCount < 1024)
            noveltySink[noveltyCount] = value;
        ++noveltyCount;
    };
    auto onOnset = [&eventSink, &eventCount](const OnsetEvent& event) {
        if (eventCount < 64)
            eventSink[eventCount] = event;
        ++eventCount;
    };
    std::size_t allocs = 0;
    {
        dsptb::test::AllocGuard guard;
        std::size_t pos = 0;
        while (pos < signal.size()) {
            const std::size_t take = std::min(std::size_t{512}, signal.size() - pos);
            det.push(signal.data() + pos, take, onNovelty, onOnset);
            pos += take;
        }
        allocs = guard.count();
    }
    CHECK(allocs == 0u);
    CHECK(noveltyCount > 0u);
    CHECK(eventCount > 0u);
    CHECK(std::isfinite(noveltySink[0]));
    CHECK(std::isfinite(eventSink[0].strength));
}

TEST_CASE("onset: reset restarts the stream cleanly") {
    const std::vector<float> signal = clickSignal(120.0, 5.0, 1.0);
    SpectralFluxOnset det(kFs, OnsetConfig{});
    const RunResult first = runWhole(det, signal);
    det.reset();
    const RunResult second = runWhole(det, signal);
    REQUIRE(second.events.size() == first.events.size());
    for (std::size_t i = 0; i < first.events.size(); ++i) {
        CHECK(second.events[i].frameIndex == first.events[i].frameIndex);
        CHECK(second.events[i].timeSeconds == first.events[i].timeSeconds);
        CHECK(second.events[i].strength == first.events[i].strength);
    }
}
