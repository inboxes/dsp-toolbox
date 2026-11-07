// Tests for LoudnessMeter and LevelMeter.
// All signals are generated in-test with the shared formulas in
// support/signals.hpp, as the issue requires.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/loudness.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <ebur128.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using dsptb::dsp::LevelMeter;
using dsptb::dsp::LoudnessMeter;
using dsptb::test::AllocGuard;
using namespace dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr double kMinusInf = -std::numeric_limits<double>::infinity();

// Peak amplitude of a sine whose level is `db` dBFS. For a 1 kHz stereo
// sine this is also its loudness in LUFS: BS.1770 sums the two channels'
// mean squares (A^2/2 + A^2/2 = A^2) and the -0.691 offset cancels the
// K-weighting gain at 1 kHz, so I reads 20*log10(A).
double dbToAmp(double db) { return std::pow(10.0, db / 20.0); }

bool near(double value, double expected, double tolerance) { return std::abs(value - expected) <= tolerance; }

bool isMinusInf(double value) { return std::isinf(value) && value < 0.0; }

// Feed a stereo sine segment through a LoudnessMeter, keeping the global
// sample index `n` continuous across calls and segments.
void feedSine(LoudnessMeter& meter, std::size_t& n, double fs, double freqHz, double amp, double seconds,
              std::size_t chunkFrames) {
    const std::size_t total = static_cast<std::size_t>(std::llround(seconds * fs));
    std::vector<float> left(chunkFrames);
    std::vector<float> right(chunkFrames);
    std::size_t done = 0;
    while (done < total) {
        const std::size_t chunk = std::min(chunkFrames, total - done);
        for (std::size_t i = 0; i < chunk; ++i) {
            const float sample = sine(n + i, fs, freqHz, amp, 0.0);
            left[i] = sample;
            right[i] = sample;
        }
        const float* planar[2] = {left.data(), right.data()};
        meter.process(planar, chunk);
        n += chunk;
        done += chunk;
    }
}

void feedSilence(LoudnessMeter& meter, double fs, double seconds) {
    const std::size_t total = static_cast<std::size_t>(std::llround(seconds * fs));
    std::vector<float> zeros(4096, 0.0f);
    const float* planar[2] = {zeros.data(), zeros.data()};
    std::size_t done = 0;
    while (done < total) {
        const std::size_t chunk = std::min<std::size_t>(4096, total - done);
        meter.process(planar, chunk);
        done += chunk;
    }
}

// Feed prebuilt stereo buffers through a meter in fixed-size chunks.
void feedBuffers(LoudnessMeter& meter, const std::vector<float>& left, const std::vector<float>& right,
                 std::size_t chunkFrames) {
    std::size_t done = 0;
    while (done < left.size()) {
        const std::size_t chunk = std::min(chunkFrames, left.size() - done);
        const float* planar[2] = {left.data() + done, right.data() + done};
        meter.process(planar, chunk);
        done += chunk;
    }
}

} // namespace

TEST_CASE("loudness: stereo 1 kHz sine at -23 dBFS reads -23 LUFS (48 kHz)") {
    LoudnessMeter meter(kFs);
    std::size_t n = 0;
    const double amp = dbToAmp(-23.0);
    feedSine(meter, n, kFs, 1000.0, amp, 3.0, 1024);
    CAPTURE(meter.momentaryLufs());
    CAPTURE(meter.shortTermLufs());
    CHECK(near(meter.momentaryLufs(), -23.0, 0.1));
    CHECK(near(meter.shortTermLufs(), -23.0, 0.1));
    feedSine(meter, n, kFs, 1000.0, amp, 17.0, 1024);
    CAPTURE(meter.integratedLufs());
    CHECK(near(meter.integratedLufs(), -23.0, 0.1));
    CHECK(near(meter.momentaryLufs(), -23.0, 0.1));
    CHECK(near(meter.shortTermLufs(), -23.0, 0.1));
}

TEST_CASE("loudness: -23 LUFS case passes at 44.1 and 96 kHz") {
    for (const double fs : {44100.0, 96000.0}) {
        LoudnessMeter meter(fs);
        std::size_t n = 0;
        feedSine(meter, n, fs, 1000.0, dbToAmp(-23.0), 20.0, 1024);
        CAPTURE(fs);
        CAPTURE(meter.integratedLufs());
        CHECK(near(meter.integratedLufs(), -23.0, 0.1));
        CHECK(near(meter.momentaryLufs(), -23.0, 0.1));
        CHECK(near(meter.shortTermLufs(), -23.0, 0.1));
    }
}

TEST_CASE("loudness: stereo 1 kHz sine at -33 dBFS reads -33 LUFS integrated") {
    LoudnessMeter meter(kFs);
    std::size_t n = 0;
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-33.0), 20.0, 1024);
    CAPTURE(meter.integratedLufs());
    CHECK(near(meter.integratedLufs(), -33.0, 0.1));
}

TEST_CASE("loudness: gating keeps quiet head/tail out of the integrated value") {
    LoudnessMeter meter(kFs);
    std::size_t n = 0;
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-36.0), 10.0, 1024);
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-23.0), 60.0, 1024);
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-36.0), 10.0, 1024);
    CAPTURE(meter.integratedLufs());
    CHECK(near(meter.integratedLufs(), -23.0, 0.1));
}

TEST_CASE("loudness: true peak recovers the inter-sample peak at fs/4") {
    // Every sample is +/-0.707 (sample peak -3.01 dBFS) but the underlying
    // sine peaks at 1.0 between samples (true peak 0 dBTP).
    LoudnessMeter meter(kFs);
    const std::size_t total = static_cast<std::size_t>(kFs); // 1 s
    std::vector<float> left(total);
    std::vector<float> right(total);
    for (std::size_t i = 0; i < total; ++i) {
        left[i] = sine(i, kFs, kFs / 4.0, 1.0, 45.0);
        right[i] = left[i];
    }
    const float* planar[2] = {left.data(), right.data()};
    meter.process(planar, total);
    for (const unsigned ch : {0u, 1u}) {
        CAPTURE(ch);
        CAPTURE(meter.samplePeakDbfs(ch));
        CAPTURE(meter.truePeakDbtp(ch));
        CHECK(near(meter.samplePeakDbfs(ch), -3.01, 0.05));
        CHECK(near(meter.truePeakDbtp(ch), 0.0, 0.3));
    }
}

TEST_CASE("loudness: LRA of a -20 / -30 two-level programme is 10 LU") {
    LoudnessMeter meter(kFs);
    std::size_t n = 0;
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-20.0), 20.0, 1024);
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-30.0), 20.0, 1024);
    CAPTURE(meter.loudnessRangeLu());
    CHECK(near(meter.loudnessRangeLu(), 10.0, 1.0));
}

TEST_CASE("loudness: silence reads -inf for M, S and I, never NaN") {
    LoudnessMeter meter(kFs);
    // Before anything is processed the getters already report -inf.
    CHECK(isMinusInf(meter.momentaryLufs()));
    CHECK(isMinusInf(meter.shortTermLufs()));
    CHECK(isMinusInf(meter.integratedLufs()));
    feedSilence(meter, kFs, 5.0);
    CHECK(isMinusInf(meter.momentaryLufs()));
    CHECK(isMinusInf(meter.shortTermLufs()));
    CHECK(isMinusInf(meter.integratedLufs()));
    CHECK(!std::isnan(meter.momentaryLufs()));
    CHECK(!std::isnan(meter.shortTermLufs()));
    CHECK(!std::isnan(meter.integratedLufs()));
    CHECK(!std::isnan(meter.loudnessRangeLu()));
    CHECK(isMinusInf(meter.truePeakDbtp(0)));
    CHECK(isMinusInf(meter.samplePeakDbfs(0)));
    // Out-of-range channels report -inf instead of reading garbage.
    CHECK(isMinusInf(meter.truePeakDbtp(7)));
    CHECK(isMinusInf(meter.samplePeakDbfs(7)));
}

TEST_CASE("loudness: chunk size does not change the results") {
    const std::size_t total = static_cast<std::size_t>(4.0 * kFs);
    std::vector<float> left(total);
    std::vector<float> right(total);
    for (std::size_t i = 0; i < total; ++i) {
        left[i] = sine(i, kFs, 1000.0, dbToAmp(-23.0), 0.0);
        right[i] = left[i];
    }
    struct Results {
        double m, s, i, tp, sp;
    };
    const auto run = [&](std::size_t chunkFrames, std::size_t maxBlock) {
        LoudnessMeter meter(kFs, 2, maxBlock);
        feedBuffers(meter, left, right, chunkFrames);
        return Results{meter.momentaryLufs(), meter.shortTermLufs(), meter.integratedLufs(),
                       meter.truePeakDbtp(0), meter.samplePeakDbfs(0)};
    };
    const Results reference = run(total, 4096); // one single call
    // Chunk sizes 1..10000 (spec) plus a tiny maxBlock to exercise the
    // internal chunking loop with a large single call.
    for (const auto& [chunk, maxBlock] :
         {std::pair{std::size_t{1}, std::size_t{4096}}, std::pair{std::size_t{13}, std::size_t{4096}},
          std::pair{std::size_t{256}, std::size_t{4096}}, std::pair{std::size_t{4096}, std::size_t{4096}},
          std::pair{std::size_t{10000}, std::size_t{4096}}, std::pair{total, std::size_t{128}}}) {
        const Results r = run(chunk, maxBlock);
        CAPTURE(chunk);
        CAPTURE(maxBlock);
        CHECK(near(r.m, reference.m, 1e-6));
        CHECK(near(r.s, reference.s, 1e-6));
        CHECK(near(r.i, reference.i, 1e-6));
        CHECK(near(r.tp, reference.tp, 1e-6));
        CHECK(near(r.sp, reference.sp, 1e-6));
    }
}

TEST_CASE("loudness: reset restarts the measurement") {
    LoudnessMeter meter(kFs);
    std::size_t n = 0;
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-23.0), 3.0, 1024);
    REQUIRE(near(meter.shortTermLufs(), -23.0, 0.1));
    meter.reset();
    CHECK(isMinusInf(meter.integratedLufs()));
    CHECK(isMinusInf(meter.samplePeakDbfs(0)));
    CHECK(isMinusInf(meter.truePeakDbtp(0)));
    n = 0;
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-23.0), 3.0, 1024);
    CHECK(near(meter.shortTermLufs(), -23.0, 0.1));
}

TEST_CASE("loudness: invalid configuration throws") {
    CHECK_THROWS_AS(LoudnessMeter(0.0), std::invalid_argument);
    CHECK_THROWS_AS(LoudnessMeter(-48000.0), std::invalid_argument);
    CHECK_THROWS_AS(LoudnessMeter(kFs, 0), std::invalid_argument);
    CHECK_THROWS_AS(LoudnessMeter(kFs, 2, 0), std::invalid_argument);
}

// Scope of the AllocGuard check below: the guard counts calls to the
// replaced global operator new, so a zero count proves only that the C++
// wrapper code in LoudnessMeter::process does not allocate. libebur128 is
// C and allocates through malloc/calloc, which the guard cannot see; the
// constant-memory claim for HISTOGRAM mode therefore rests on
// libebur128's design (fixed-size histograms instead of a growing block
// list), not on this test.
TEST_CASE("loudness: process does not allocate after a 10 s warm-up") {
    LoudnessMeter meter(kFs);
    std::size_t n = 0;
    feedSine(meter, n, kFs, 1000.0, dbToAmp(-23.0), 10.0, 4096); // warm-up
    std::vector<float> left(256);
    std::vector<float> right(256);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = sine(n + i, kFs, 1000.0, dbToAmp(-23.0), 0.0);
        right[i] = left[i];
    }
    const float* planar[2] = {left.data(), right.data()};
    AllocGuard guard;
    for (int block = 0; block < 200; ++block)
        meter.process(planar, left.size());
    CHECK(guard.count() == 0);
}

// ---------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------

namespace {

// Feed a LevelMeter from per-frame generated stereo samples.
template <class Gen>
void feedLevel(LevelMeter& meter, std::size_t frames, std::size_t chunkFrames, Gen&& gen) {
    std::vector<float> left(chunkFrames);
    std::vector<float> right(chunkFrames);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t chunk = std::min(chunkFrames, frames - done);
        for (std::size_t i = 0; i < chunk; ++i) {
            const auto pair = gen(done + i);
            left[i] = pair[0];
            right[i] = pair[1];
        }
        const float* planar[2] = {left.data(), right.data()};
        meter.process(planar, chunk);
        done += chunk;
    }
}

std::array<float, 2> stereo(float v) { return {v, v}; }

} // namespace

TEST_CASE("level: RMS of a -6 dBFS sine is -9.03 dBFS after one window") {
    LevelMeter meter(kFs, 2); // 300 ms window = 14400 samples at 48 kHz
    const std::size_t window = 14400;
    feedLevel(meter, window, 1024, [](std::size_t n) {
        return stereo(sine(n, kFs, 1000.0, dbToAmp(-6.0), 0.0));
    });
    CAPTURE(meter.rmsDbfs(0));
    CHECK(near(meter.rmsDbfs(0), -9.03, 0.05));
    CHECK(near(meter.rmsDbfs(1), -9.03, 0.05));
    // The peak of a -6 dBFS sine is -6 dBFS.
    CHECK(near(meter.peakDbfs(0), -6.0, 0.05));
}

TEST_CASE("level: channels are independent") {
    LevelMeter meter(kFs, 2);
    feedLevel(meter, 14400, 1024, [](std::size_t n) {
        return std::array<float, 2>{sine(n, kFs, 1000.0, dbToAmp(-6.0), 0.0), 0.0f};
    });
    CHECK(near(meter.rmsDbfs(0), -9.03, 0.05));
    CHECK(isMinusInf(meter.rmsDbfs(1)));
    CHECK(near(meter.peakDbfs(0), -6.0, 0.05));
    CHECK(isMinusInf(static_cast<double>(meter.peakDbfs(1))));
    CHECK(!meter.clipped(0));
    CHECK(!meter.clipped(1));
    CHECK(isMinusInf(static_cast<double>(meter.peakDbfs(9))));
    CHECK(!meter.clipped(9));
}

TEST_CASE("level: peak holds for peakHoldS, then falls at fallDbPerS") {
    LevelMeter meter(kFs, 2, 300.0, 2.0, 20.0);
    // 0.5 s of full-scale 1 kHz sine: the peak reaches exactly 0 dBFS.
    feedLevel(meter, 24000, 4800, [](std::size_t n) { return stereo(sine(n, kFs, 1000.0, 1.0, 0.0)); });
    CHECK(near(meter.peakDbfs(0), 0.0, 0.01));
    const auto feedSilenceSeconds = [&](double seconds) {
        feedLevel(meter, static_cast<std::size_t>(std::llround(seconds * kFs)), 4800,
                  [](std::size_t) { return stereo(0.0f); });
    };
    feedSilenceSeconds(1.0); // still inside the 2 s hold
    CHECK(near(meter.peakDbfs(0), 0.0, 0.05));
    feedSilenceSeconds(2.0); // 3 s of silence: 1 s of fall at 20 dB/s
    const double after3 = meter.peakDbfs(0);
    CAPTURE(after3);
    CHECK(near(after3, -20.0, 1.0));
    feedSilenceSeconds(1.0); // 4 s of silence: 2 s of fall
    const double after4 = meter.peakDbfs(0);
    CAPTURE(after4);
    CHECK(near(after4, -40.0, 2.0));
    // The fall rate itself over that second: 20 dB/s within 5 %.
    CHECK(near(after3 - after4, 20.0, 1.0));
}

TEST_CASE("level: setPeakHoldS(0) makes the peak fall immediately") {
    LevelMeter meter(kFs, 2, 300.0, 2.0, 20.0);
    meter.setPeakHoldS(0.0);
    feedLevel(meter, 4800, 4800, [](std::size_t n) { return stereo(sine(n, kFs, 1000.0, 1.0, 0.0)); });
    REQUIRE(near(meter.peakDbfs(0), 0.0, 0.01));
    feedLevel(meter, 24000, 4800, [](std::size_t) { return stereo(0.0f); }); // 0.5 s silence
    CHECK(near(meter.peakDbfs(0), -10.0, 0.5));
}

TEST_CASE("level: clip flag sets on |x| >= 1.0 and clears on clearClip") {
    LevelMeter meter(kFs, 2);
    feedLevel(meter, 64, 64, [](std::size_t) { return stereo(0.99f); });
    CHECK(!meter.clipped(0));
    feedLevel(meter, 64, 64, [](std::size_t n) { return stereo(n == 0 ? 1.0f : 0.0f); });
    CHECK(meter.clipped(0));
    CHECK(meter.clipped(1));
    meter.clearClip();
    CHECK(!meter.clipped(0));
    CHECK(!meter.clipped(1));
    feedLevel(meter, 64, 64, [](std::size_t n) { return stereo(n == 0 ? -1.0f : 0.0f); });
    CHECK(meter.clipped(0));
    meter.clearClip();
    feedLevel(meter, 64, 64, [](std::size_t) { return stereo(0.5f); });
    CHECK(!meter.clipped(0));
}

TEST_CASE("level: setRmsWindowMs changes the window and restarts RMS history") {
    LevelMeter meter(kFs, 2);
    meter.setRmsWindowMs(100.0); // 4800 samples at 48 kHz
    feedLevel(meter, 4800, 1024, [](std::size_t n) {
        return stereo(sine(n, kFs, 1000.0, dbToAmp(-6.0), 0.0));
    });
    CHECK(near(meter.rmsDbfs(0), -9.03, 0.05));
    CHECK_THROWS_AS(meter.setRmsWindowMs(0.0), std::invalid_argument);
    CHECK_THROWS_AS(meter.setRmsWindowMs(-100.0), std::invalid_argument);
}

TEST_CASE("level: running-sum drift stays within 0.01 dB over 10 minutes") {
    LevelMeter meter(kFs, 2);
    constexpr std::size_t kWindow = 14400; // 300 ms at 48 kHz
    std::vector<float> window(kWindow, 0.0f);
    std::size_t pos = 0;
    LcgNoise noise(12345u, 0.5);
    const std::size_t total = static_cast<std::size_t>(600.0 * kFs); // 10 minutes
    constexpr std::size_t kChunk = 4096;
    std::vector<float> left(kChunk);
    std::vector<float> right(kChunk);
    std::size_t done = 0;
    while (done < total) {
        const std::size_t chunk = std::min(kChunk, total - done);
        for (std::size_t i = 0; i < chunk; ++i) {
            const float v = noise.next();
            left[i] = v;
            right[i] = v;
            window[pos] = v;
            pos = (pos + 1 == kWindow) ? 0 : pos + 1;
        }
        const float* planar[2] = {left.data(), right.data()};
        meter.process(planar, chunk);
        done += chunk;
    }
    // Exact recompute over the last window of generated samples.
    long double sum = 0.0L;
    for (const float v : window) {
        const long double d = static_cast<long double>(v);
        sum += d * d;
    }
    const double exactDb = 10.0 * std::log10(static_cast<double>(sum / static_cast<long double>(kWindow)));
    CAPTURE(meter.rmsDbfs(0));
    CAPTURE(exactDb);
    CHECK(near(meter.rmsDbfs(0), exactDb, 0.01));
    CHECK(near(meter.rmsDbfs(1), exactDb, 0.01));
}

TEST_CASE("level: process does not allocate after a 10 s warm-up") {
    LevelMeter meter(kFs, 2);
    std::size_t n = 0;
    feedLevel(meter, static_cast<std::size_t>(10.0 * kFs), 4096, [&n](std::size_t) {
        return stereo(sine(n++, kFs, 1000.0, dbToAmp(-6.0), 0.0));
    });
    std::vector<float> block(4096);
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = sine(n + i, kFs, 1000.0, dbToAmp(-6.0), 0.0);
    const float* planar[2] = {block.data(), block.data()};
    AllocGuard guard;
    // 200k frames crosses the 2^16-sample exact-recompute boundary, which
    // must not allocate either.
    for (int iter = 0; iter < 50; ++iter)
        meter.process(planar, block.size());
    CHECK(guard.count() == 0);
}

TEST_CASE("level: invalid configuration throws") {
    CHECK_THROWS_AS(LevelMeter(0.0, 2), std::invalid_argument);
    CHECK_THROWS_AS(LevelMeter(kFs, 0), std::invalid_argument);
    CHECK_THROWS_AS(LevelMeter(kFs, 2, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(LevelMeter(kFs, 2, 300.0, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(LevelMeter(kFs, 2, 300.0, 2.0, -20.0), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Micro-benchmark (spec §6, report only: numbers are printed, not asserted,
// because CI runner timing is not a correctness signal)
// ---------------------------------------------------------------------------

TEST_CASE("loudness: micro-benchmark, both true-peak modes (report only)") {
    constexpr std::size_t kBlockFrames = 256;
    constexpr int kWarmup = 2000;
    constexpr int kBatchIterations = 2000;
    constexpr int kBatches = 10;

    // Interleaved stereo block of a -23 dBFS 1 kHz sine.
    std::vector<float> interleaved(kBlockFrames * 2);
    std::vector<float> left(kBlockFrames);
    std::vector<float> right(kBlockFrames);
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        const float v = sine(i, kFs, 1000.0, dbToAmp(-23.0), 0.0);
        interleaved[2 * i] = v;
        interleaved[2 * i + 1] = v;
        left[i] = v;
        right[i] = v;
    }
    const float* planar[2] = {left.data(), right.data()};

    // Shared CI runners are noisy neighbours: a single long average mixes
    // in preemption, so time in batches and report the best batch average,
    // which is the standard estimate of uncontended per-block cost.
    const auto benchMinUsPerBlock = [](auto&& call) {
        for (int i = 0; i < kWarmup; ++i)
            call();
        double best = std::numeric_limits<double>::max();
        for (int batch = 0; batch < kBatches; ++batch) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kBatchIterations; ++i)
                call();
            const auto stop = std::chrono::steady_clock::now();
            const double perBlock =
                std::chrono::duration<double, std::micro>(stop - start).count() / kBatchIterations;
            best = std::min(best, perBlock);
        }
        return best;
    };

    const auto benchRaw = [&](int mode) {
        ebur128_state* state = ebur128_init(2, 48000, mode);
        CHECK(state != nullptr);
        if (state == nullptr)
            return 0.0;
        const double us = benchMinUsPerBlock(
            [&] { (void)ebur128_add_frames_float(state, interleaved.data(), kBlockFrames); });
        ebur128_destroy(&state);
        return us;
    };

    constexpr int kBaseMode = EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA |
                              EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_HISTOGRAM;
    const double withoutTp = benchRaw(kBaseMode);
    const double withTp = benchRaw(kBaseMode | EBUR128_MODE_TRUE_PEAK);

    LoudnessMeter meter(kFs);
    const double wrapper = benchMinUsPerBlock([&] { meter.process(planar, kBlockFrames); });

    LevelMeter level(kFs, 2);
    const double levelUs = benchMinUsPerBlock([&] { level.process(planar, kBlockFrames); });

    std::printf("[bench] LoudnessMeter budget (spec S6): <= 5 us per 256-frame stereo block without "
                "true-peak, <= 25 us with true-peak\n");
    std::printf("[bench] libebur128 raw, no true-peak : %.3f us/block\n", withoutTp);
    std::printf("[bench] libebur128 raw, true-peak    : %.3f us/block\n", withTp);
    std::printf("[bench] LoudnessMeter::process       : %.3f us/block (includes true-peak + interleave)\n",
                wrapper);
    std::printf("[bench] LevelMeter::process          : %.3f us/block\n", levelUs);
    CHECK(wrapper > 0.0); // the benchmark actually ran
}
