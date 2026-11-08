#include <doctest/doctest.h>

#include <dsptb/dsp_common/voice_stats.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kC = 35000.0; // speed of sound, cm/s (spec default)

// Single-tube formants Fi = (2i-1)*c/(4L), i = 1..4.
std::vector<double> tubeFormants(double lCm) {
    std::vector<double> out;
    for (int i = 1; i <= 4; ++i) {
        out.push_back((2.0 * i - 1.0) * kC / (4.0 * lCm));
    }
    return out;
}

// Synthetic vowel (acceptance criteria): an impulse train at f0
// through four two-pole resonators at the single-tube formants, bandwidths
// 80, 90, 120 and 150 Hz. When tilted, the impulse train first passes
// through two cascaded one-pole low-passes (fc = 500 Hz), giving a source
// tilted to about -12 dB/oct net above 500 Hz (the tilted-source regression
// test, maintainer ruling). Built in double, returned as float,
// normalised to peak 1. A 0.5 s warm-up is discarded so the resonators are
// in steady state.
std::vector<float> makeVowel(double fs, double lCm, double f0, std::size_t nFrames,
                             std::size_t frameSize, bool tilted = false) {
    const std::size_t warmup = static_cast<std::size_t>(0.5 * fs);
    const std::size_t total = warmup + nFrames * frameSize;
    std::vector<double> x(total, 0.0);
    double phase = 0.0;
    for (std::size_t n = 0; n < total; ++n) {
        phase += f0 / fs;
        if (phase >= 1.0) {
            phase -= 1.0;
            x[n] = 1.0;
        }
    }
    if (tilted) {
        const double g = std::exp(-2.0 * 3.14159265358979323846 * 500.0 / fs);
        for (int stage = 0; stage < 2; ++stage) {
            std::vector<double> y(total, 0.0);
            double acc = 0.0;
            for (std::size_t n = 0; n < total; ++n) {
                acc = (1.0 - g) * x[n] + g * acc;
                y[n] = acc;
            }
            x = std::move(y);
        }
    }
    const std::vector<double> tube = tubeFormants(lCm);
    const double bandwidths[4] = {80.0, 90.0, 120.0, 150.0};
    for (int r = 0; r < 4; ++r) {
        const double poleR = std::exp(-3.14159265358979323846 * bandwidths[r] / fs);
        const double theta = 2.0 * 3.14159265358979323846 * tube[static_cast<std::size_t>(r)] / fs;
        const double b1 = 2.0 * poleR * std::cos(theta);
        const double b2 = -poleR * poleR;
        std::vector<double> y(total, 0.0);
        double y1 = 0.0, y2 = 0.0;
        for (std::size_t n = 0; n < total; ++n) {
            const double v = x[n] + b1 * y1 + b2 * y2;
            y2 = y1;
            y1 = v;
            y[n] = v;
        }
        x = std::move(y);
    }
    double peak = 0.0;
    for (std::size_t n = warmup; n < total; ++n) peak = std::max(peak, std::abs(x[n]));
    std::vector<float> out;
    out.reserve(nFrames * frameSize);
    for (std::size_t n = warmup; n < total; ++n) {
        out.push_back(static_cast<float>(x[n] / peak));
    }
    return out;
}

// Pushes nFrames consecutive frames of `signal` (frameSize samples each)
// and returns the FormantFrames. yinConfidence is passed through.
std::vector<FormantFrame> runFrames(VoiceStats& stats, const std::vector<float>& signal,
                                    std::size_t nFrames, float yinConfidence = 0.99f) {
    std::vector<FormantFrame> out;
    out.reserve(nFrames);
    const std::size_t frameSize = stats.frameSize();
    for (std::size_t i = 0; i < nFrames; ++i) {
        out.push_back(stats.pushFrame(signal.data() + i * frameSize, yinConfidence));
    }
    return out;
}

// Worst relative formant error over the frames (against the tube formants),
// and the worst F1 error. Frames with fewer than 3 formants count as
// infinite error so a lost formant fails loudly.
void worstErrors(const std::vector<FormantFrame>& frames, const std::vector<double>& tube,
                 double& worst, double& worstF1) {
    worst = 0.0;
    worstF1 = 0.0;
    for (const FormantFrame& frame : frames) {
        if (frame.count < 3) {
            worst = 1.0;
            worstF1 = 1.0;
            continue;
        }
        for (int j = 0; j < frame.count; ++j) {
            const double err = std::abs(static_cast<double>(frame.hz[j]) - tube[static_cast<std::size_t>(j)]) /
                               tube[static_cast<std::size_t>(j)];
            worst = std::max(worst, err);
            if (j == 0) worstF1 = std::max(worstF1, err);
        }
    }
}

} // namespace

TEST_CASE("voice_stats: vtlFromFormants on exact tube formants") {
    const std::vector<double> tube = tubeFormants(17.0);
    float hz[4];
    for (int i = 0; i < 4; ++i) hz[i] = static_cast<float>(tube[static_cast<std::size_t>(i)]);
    CHECK(VoiceStats::vtlFromFormants(hz, 4, kC) == doctest::Approx(17.0f).epsilon(0.0005));
    // With 3 formants: same.
    CHECK(VoiceStats::vtlFromFormants(hz, 3, kC) == doctest::Approx(17.0f).epsilon(0.0005));
    // Exact tube values: 514.7, 1544.1, 2573.5, 3602.9 Hz.
    const float exact[4] = {514.7f, 1544.1f, 2573.5f, 3602.9f};
    CHECK(VoiceStats::vtlFromFormants(exact, 4, kC) == doctest::Approx(17.0f).epsilon(0.0005));
    CHECK(VoiceStats::vtlFromFormants(exact, 3, kC) == doctest::Approx(17.0f).epsilon(0.0005));
    CHECK(VoiceStats::vtlFromFormants(nullptr, 0, kC) == 0.0f);
    // Non-finite input returns 0 instead of propagating NaN (M1).
    const float nanHz[4] = {std::numeric_limits<float>::quiet_NaN(), 1544.1f, 2573.5f, 3602.9f};
    CHECK(VoiceStats::vtlFromFormants(nanHz, 4, kC) == 0.0f);
    const float infHz[4] = {std::numeric_limits<float>::infinity(), 1544.1f, 2573.5f, 3602.9f};
    CHECK(VoiceStats::vtlFromFormants(infHz, 4, kC) == 0.0f);
    CHECK(VoiceStats::vtlFromFormants(exact, 4, std::numeric_limits<double>::quiet_NaN()) == 0.0f);
}

TEST_CASE("voice_stats: ltasBandCentres") {
    float centres[32];
    VoiceStats::ltasBandCentres(centres);
    CHECK(centres[0] == doctest::Approx(100.0f));
    CHECK(centres[31] == doctest::Approx(12800.0f));
    CHECK(centres[15] == doctest::Approx(100.0f * std::pow(128.0f, 15.0f / 31.0f)).epsilon(1e-6));
    for (int i = 1; i < 32; ++i) CHECK(centres[i] > centres[i - 1]);
}

TEST_CASE("voice_stats: synthetic vowel 17 cm / 110 Hz at 48 kHz") {
    constexpr double kFs = 48000.0;
    constexpr std::size_t kFrames = 24;
    VoiceStats stats(kFs);
    CHECK(stats.frameSize() == 2048);
    const std::vector<float> signal = makeVowel(kFs, 17.0, 110.0, kFrames, stats.frameSize());
    const std::vector<FormantFrame> frames = runFrames(stats, signal, kFrames);
    const std::vector<double> tube = tubeFormants(17.0);
    for (const FormantFrame& frame : frames) {
        CHECK(frame.count >= 3);
        CHECK(frame.valid);
        CHECK(frame.vtlCm == doctest::Approx(17.0f).epsilon(0.05));
        for (int j = 0; j < frame.count; ++j) {
            CHECK(frame.hz[j] == doctest::Approx(static_cast<float>(tube[static_cast<std::size_t>(j)]))
                                     .epsilon(0.05));
        }
    }
    double worst = 0.0, worstF1 = 0.0;
    worstErrors(frames, tube, worst, worstF1);
    std::printf("[voice_stats] 48k 17cm/110Hz: worst formant error %.2f%% (F1 %.2f%%), "
                "median VTL %.3f cm\n",
                worst * 100.0, worstF1 * 100.0, stats.summary().vtlCmMedian);
    CHECK(stats.summary().vtlCmMedian == doctest::Approx(17.0f).epsilon(0.05));
}

TEST_CASE("voice_stats: synthetic vowel 17 cm / 110 Hz at 44.1 and 96 kHz") {
    for (const double fs : {44100.0, 96000.0}) {
        VoiceStats stats(fs);
        CHECK(stats.frameSize() == static_cast<std::size_t>(fs <= 50000.0 ? 2048 : 4096));
        constexpr std::size_t kFrames = 24;
        const std::vector<float> signal = makeVowel(fs, 17.0, 110.0, kFrames, stats.frameSize());
        const std::vector<FormantFrame> frames = runFrames(stats, signal, kFrames);
        const std::vector<double> tube = tubeFormants(17.0);
        for (const FormantFrame& frame : frames) {
            INFO("fs=", fs);
            CHECK(frame.count >= 3);
            CHECK(frame.valid);
            CHECK(frame.vtlCm == doctest::Approx(17.0f).epsilon(0.05));
            for (int j = 0; j < frame.count; ++j) {
                CHECK(frame.hz[j] == doctest::Approx(static_cast<float>(tube[static_cast<std::size_t>(j)]))
                                         .epsilon(0.05));
            }
        }
        double worst = 0.0, worstF1 = 0.0;
        worstErrors(frames, tube, worst, worstF1);
        std::printf("[voice_stats] %.0f Hz 17cm/110Hz: worst formant error %.2f%% (F1 %.2f%%), "
                    "median VTL %.3f cm\n",
                    fs, worst * 100.0, worstF1 * 100.0, stats.summary().vtlCmMedian);
        CHECK(stats.summary().vtlCmMedian == doctest::Approx(17.0f).epsilon(0.05));
    }
}

TEST_CASE("voice_stats: synthetic vowel 14 cm / 210 Hz median VTL over 200 frames") {
    constexpr double kFs = 48000.0;
    constexpr std::size_t kFrames = 200;
    VoiceStats stats(kFs);
    const std::vector<float> signal = makeVowel(kFs, 14.0, 210.0, kFrames, stats.frameSize());
    const std::vector<FormantFrame> frames = runFrames(stats, signal, kFrames);
    const std::vector<double> tube = tubeFormants(14.0);
    double worst = 0.0, worstF1 = 0.0;
    worstErrors(frames, tube, worst, worstF1);
    std::printf("[voice_stats] 48k 14cm/210Hz: worst formant error %.2f%% (F1 %.2f%%), "
                "median VTL %.3f cm over %zu frames\n",
                worst * 100.0, worstF1 * 100.0, stats.summary().vtlCmMedian, kFrames);
    CHECK(stats.summary().vtlCmMedian == doctest::Approx(14.0f).epsilon(0.05));
    CHECK(stats.summary().vtlFrames == kFrames);
}

// Tilted-source regression test (maintainer ruling, condition 2): a source
// tilted to about -12 dB/oct net must still give >= 3 formants in every
// frame and VTL within +/-5%. Without pre-emphasis this regresses (with
// the coefficient at 0 the tilted signal yields no valid VTL frames),
// which is why pre-emphasis is kept at 0.7.
TEST_CASE("voice_stats: tilted source (-12 dB/oct) keeps formants and VTL") {
    constexpr double kFs = 48000.0;
    constexpr std::size_t kFrames = 24;
    VoiceStats stats(kFs);
    const std::vector<float> signal = makeVowel(kFs, 17.0, 110.0, kFrames, stats.frameSize(), true);
    const std::vector<FormantFrame> frames = runFrames(stats, signal, kFrames);
    for (const FormantFrame& frame : frames) {
        CHECK(frame.count >= 3);
        CHECK(frame.valid);
    }
    double worst = 0.0, worstF1 = 0.0;
    worstErrors(frames, tubeFormants(17.0), worst, worstF1);
    std::printf("[voice_stats] 48k tilted 17cm/110Hz: worst formant error %.2f%% (F1 %.2f%%), "
                "median VTL %.3f cm\n",
                worst * 100.0, worstF1 * 100.0, stats.summary().vtlCmMedian);
    CHECK(stats.summary().vtlCmMedian == doctest::Approx(17.0f).epsilon(0.05));
    CHECK(stats.summary().vtlFrames == kFrames);
}

TEST_CASE("voice_stats: LTAS white noise is flat (48 kHz)") {
    constexpr double kFs = 48000.0;
    constexpr std::size_t kFrames = 64;
    VoiceStats stats(kFs);
    sig::LcgNoise noise(1u, 1.0);
    std::vector<float> frame(stats.frameSize());
    for (std::size_t f = 0; f < kFrames; ++f) {
        for (float& s : frame) s = noise.next();
        stats.pushFrame(frame.data(), 0.99f);
    }
    const VoiceSummary summary = stats.summary();
    float worst = 0.0f;
    for (int b = 0; b <= 30; ++b) worst = std::max(worst, std::abs(summary.ltasDb[b]));
    std::printf("[voice_stats] 48k white noise: max |ltasDb[0..30]| = %.3f dB\n", worst);
    for (int b = 0; b <= 30; ++b) CHECK(std::abs(summary.ltasDb[b]) <= 1.5f);
}

// Ruling condition 3: at 44.1 kHz with a 2048-point frame, band 1
// (~108-126 Hz) contains no whole FFT bin, so a plain per-band mean is
// 0/0. Fractional bin-overlap weights must keep every band defined and
// white noise flat.
TEST_CASE("voice_stats: LTAS white noise is flat (44.1 kHz, empty-band check)") {
    constexpr double kFs = 44100.0;
    constexpr std::size_t kFrames = 64;
    VoiceStats stats(kFs);
    sig::LcgNoise noise(1u, 1.0);
    std::vector<float> frame(stats.frameSize());
    for (std::size_t f = 0; f < kFrames; ++f) {
        for (float& s : frame) s = noise.next();
        stats.pushFrame(frame.data(), 0.99f);
    }
    const VoiceSummary summary = stats.summary();
    float worst = 0.0f;
    for (int b = 0; b <= 30; ++b) {
        CHECK(std::isfinite(summary.ltasDb[b]));
        worst = std::max(worst, std::abs(summary.ltasDb[b]));
    }
    std::printf("[voice_stats] 44.1k white noise: max |ltasDb[0..30]| = %.3f dB\n", worst);
    for (int b = 0; b <= 30; ++b) CHECK(std::abs(summary.ltasDb[b]) <= 1.5f);
}

TEST_CASE("voice_stats: LTAS 1 kHz sine peaks in the nearest band") {
    constexpr double kFs = 48000.0;
    VoiceStats stats(kFs);
    std::vector<float> frame(stats.frameSize());
    for (std::size_t n = 0; n < frame.size(); ++n) {
        frame[n] = sig::sine(n, kFs, 1000.0, 1.0, 0.0);
    }
    stats.pushFrame(frame.data(), 0.99f);
    const VoiceSummary summary = stats.summary();
    int peak = 0;
    for (int b = 1; b < 32; ++b) {
        if (summary.ltasDb[b] > summary.ltasDb[peak]) peak = b;
    }
    float centres[32];
    VoiceStats::ltasBandCentres(centres);
    std::printf("[voice_stats] 1 kHz sine: peak band %d (centre %.0f Hz), "
                "band-2 delta %.1f dB, band+2 delta %.1f dB\n",
                peak, centres[peak], summary.ltasDb[peak] - summary.ltasDb[peak - 2],
                summary.ltasDb[peak] - summary.ltasDb[peak + 2]);
    // Band nearest 1 kHz: centres are 100*128^(i/31); band 15 is 1046 Hz.
    CHECK(peak == 15);
    CHECK(summary.ltasDb[peak] - summary.ltasDb[peak - 2] >= 10.0f);
    CHECK(summary.ltasDb[peak] - summary.ltasDb[peak + 2] >= 10.0f);
}

TEST_CASE("voice_stats: HNR from YIN confidence") {
    VoiceStats stats(48000.0);
    std::vector<float> frame(stats.frameSize(), 0.0f);
    stats.pushFrame(frame.data(), 0.99f);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(19.96f).epsilon(0.001));
    stats.reset();
    stats.pushFrame(frame.data(), 0.5f);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(0.0f).epsilon(0.001));
    stats.reset();
    stats.pushFrame(frame.data(), 0.001f);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(-10.0f).epsilon(0.001));
    // Median over mixed values: 0.99, 0.5, 0.001 -> median is the 0.5 frame.
    stats.reset();
    stats.pushFrame(frame.data(), 0.99f);
    stats.pushFrame(frame.data(), 0.5f);
    stats.pushFrame(frame.data(), 0.001f);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(stats.summary().frames == 3u);
    // A NaN confidence is treated as unvoiced (-10 dB) instead of
    // poisoning the median (I2): 0.99, NaN, 0.99 -> median 19.96 dB.
    stats.reset();
    stats.pushFrame(frame.data(), std::numeric_limits<float>::quiet_NaN());
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(-10.0f).epsilon(0.001));
    stats.reset();
    stats.pushFrame(frame.data(), 0.99f);
    stats.pushFrame(frame.data(), std::numeric_limits<float>::quiet_NaN());
    stats.pushFrame(frame.data(), 0.99f);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(19.96f).epsilon(0.001));
}

TEST_CASE("voice_stats: frames beyond maxFrames are counted, not stored") {
    VoiceStatsConfig config;
    config.maxFrames = 4;
    VoiceStats stats(48000.0, config);
    std::vector<float> frame(stats.frameSize(), 0.0f);
    {
        AllocGuard guard;
        for (int i = 0; i < 10; ++i) stats.pushFrame(frame.data(), 0.5f);
        CHECK(guard.count() == 0);
    }
    CHECK(stats.summary().frames == 10u);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(0.0f));
}

TEST_CASE("voice_stats: reset clears all statistics") {
    VoiceStats stats(48000.0);
    std::vector<float> frame(stats.frameSize(), 0.0f);
    for (int i = 0; i < 5; ++i) stats.pushFrame(frame.data(), 0.99f);
    CHECK(stats.summary().frames == 5u);
    CHECK(stats.summary().hnrDbMedian == doctest::Approx(19.96f).epsilon(0.001));
    {
        AllocGuard guard;
        stats.reset();
        CHECK(guard.count() == 0);
    }
    const VoiceSummary summary = stats.summary();
    CHECK(summary.frames == 0u);
    CHECK(summary.vtlFrames == 0u);
    CHECK(summary.vtlCmMedian == 0.0f);
    CHECK(summary.hnrDbMedian == 0.0f);
    for (int b = 0; b < 32; ++b) CHECK(summary.ltasDb[b] == 0.0f);
}

TEST_CASE("voice_stats: bad config throws invalid_argument") {
    VoiceStatsConfig bad;
    bad.frameSize = 1000; // not a power of two
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    bad.frameSize = 256; // power of two but < 512
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    bad = VoiceStatsConfig{};
    bad.maxFrames = 0;
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    bad = VoiceStatsConfig{};
    bad.minVtlCm = 24.0f;
    bad.maxVtlCm = 8.0f;
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    CHECK_THROWS_AS(VoiceStats(0.0), std::invalid_argument);
    CHECK_THROWS_AS(VoiceStats(-48000.0), std::invalid_argument);
    // Non-finite sample rate (M2).
    CHECK_THROWS_AS(VoiceStats(std::numeric_limits<double>::quiet_NaN(), VoiceStatsConfig{}), std::invalid_argument);
    CHECK_THROWS_AS(VoiceStats(std::numeric_limits<double>::infinity(), VoiceStatsConfig{}), std::invalid_argument);
    // Non-positive / non-finite speed of sound (M2).
    bad = VoiceStatsConfig{};
    bad.speedOfSoundCmS = 0.0;
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    bad.speedOfSoundCmS = -35000.0;
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    bad.speedOfSoundCmS = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    bad.speedOfSoundCmS = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(VoiceStats(48000.0, bad), std::invalid_argument);
    // Sample rate too low for the LTAS band table: Nyquist below the top
    // band's lower edge (~11.84 kHz) leaves a band with no bin overlap
    // (0/0 = NaN) (C1). 24 kHz is above the ~23.7 kHz floor and builds.
    CHECK_THROWS_AS(VoiceStats(16000.0), std::invalid_argument);
    CHECK_THROWS_AS(VoiceStats(22050.0), std::invalid_argument);
    CHECK_NOTHROW(VoiceStats(24000.0));
    // Too few decimated samples for the order-12 LPC (M2): 512-sample
    // frames at 480 kHz decimate by 43, leaving 11 samples.
    bad = VoiceStatsConfig{};
    bad.frameSize = 512;
    CHECK_THROWS_AS(VoiceStats(480000.0, bad), std::invalid_argument);
}

TEST_CASE("voice_stats: no allocation in pushFrame, summary or reset") {
    VoiceStats stats(48000.0);
    const std::vector<float> signal = makeVowel(48000.0, 17.0, 110.0, 2, stats.frameSize());
    {
        AllocGuard guard;
        stats.pushFrame(signal.data(), 0.99f);
        stats.pushFrame(signal.data() + stats.frameSize(), 0.99f);
        const VoiceSummary summary = stats.summary();
        (void)summary;
        stats.reset();
        CHECK(guard.count() == 0);
    }
}

TEST_CASE("voice_stats: micro-benchmark pushFrame (report only, budget 60 us)") {
    constexpr double kFs = 48000.0;
    VoiceStats stats(kFs);
    const std::vector<float> signal = makeVowel(kFs, 17.0, 110.0, 1, stats.frameSize());
    // Warm up.
    for (int i = 0; i < 10; ++i) stats.pushFrame(signal.data(), 0.99f);
    constexpr int kIterations = 200;
    const auto start = std::chrono::steady_clock::now();
    double check = 0.0;
    for (int i = 0; i < kIterations; ++i) {
        const FormantFrame frame = stats.pushFrame(signal.data(), 0.99f);
        check += frame.hz[0];
    }
    const auto stop = std::chrono::steady_clock::now();
    const double usPerFrame =
        std::chrono::duration<double, std::micro>(stop - start).count() / kIterations;
    std::printf("[bench] VoiceStats::pushFrame 48 kHz: %.2f us per frame over %d calls "
                "(spec budget: 60 us)\n",
                usPerFrame, kIterations);
    CHECK(check > 0.0); // the benchmark actually ran
}
