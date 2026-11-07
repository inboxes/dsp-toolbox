// YinPitch tests.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/pitch.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using dsptb::dsp::PitchConfig;
using dsptb::dsp::PitchResult;
using dsptb::dsp::YinPitch;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr double kAmpMinus12Dbfs = 0.251188643150958; // 10^(-12/20)

std::vector<float> sineSignal(double fs, double hz, double amp, double seconds) {
    const std::size_t count = static_cast<std::size_t>(fs * seconds);
    std::vector<float> out;
    out.reserve(count);
    for (std::size_t n = 0; n < count; ++n)
        out.push_back(sig::sine(n, fs, hz, amp, 0.0));
    return out;
}

std::vector<PitchResult> runAll(YinPitch& pitch, const std::vector<float>& signal) {
    std::vector<PitchResult> results;
    pitch.push(signal.data(), signal.size(),
               [&results](const PitchResult& r) { results.push_back(r); });
    return results;
}

// Frames expected from `samples` pushed samples: one per hop once the
// first full frame has arrived.
std::size_t expectedFrames(std::size_t samples, std::size_t frameSize, std::size_t hop) {
    return samples < frameSize ? 0 : (samples - frameSize) / hop + 1;
}

} // namespace

TEST_CASE("pitch: fs/minHz > frameSize/2 throws std::invalid_argument") {
    // Default config at 96 kHz: 96000/50 = 1920 > 2048/2 = 1024.
    CHECK_THROWS_AS((YinPitch(96000.0, PitchConfig{})), std::invalid_argument);
    // minHz too low for the frame at 48 kHz: 48000/40 = 1200 > 1024.
    PitchConfig lowMin;
    lowMin.minHz = 40.0;
    CHECK_THROWS_AS((YinPitch(kFs, lowMin)), std::invalid_argument);
    // Exactly at the limit is fine: 48000/46.875 = 1024 = frameSize/2.
    PitchConfig edge;
    edge.minHz = 46.875;
    CHECK_NOTHROW((YinPitch(kFs, edge)));
    // Other invalid configs.
    PitchConfig badHop;
    badHop.hop = 0;
    CHECK_THROWS_AS((YinPitch(kFs, badHop)), std::invalid_argument);
    PitchConfig badMin;
    badMin.minHz = 0.0;
    CHECK_THROWS_AS((YinPitch(kFs, badMin)), std::invalid_argument);
    CHECK_THROWS_AS((YinPitch(0.0, PitchConfig{})), std::invalid_argument);
}

TEST_CASE("pitch: sine 220 Hz at -12 dBFS, fs 48 kHz") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::vector<float> signal = sineSignal(kFs, 220.0, kAmpMinus12Dbfs, 1.0);
    const std::vector<PitchResult> results = runAll(pitch, signal);
    CHECK(results.size() == expectedFrames(signal.size(), 2048, 256));
    REQUIRE(results.size() > 2);
    INFO("frame 0: hz = ", results[0].hz, ", confidence = ", results[0].confidence);
    // Acceptance: every voiced frame after the first full frame is within
    // +/-0.5 Hz with confidence >= 0.9. A pure sine must in fact be voiced
    // on all of them.
    for (std::size_t i = 1; i < results.size(); ++i) {
        CHECK(results[i].frameIndex == i);
        CHECK(results[i].voiced);
        CHECK(std::abs(static_cast<double>(results[i].hz) - 220.0) <= 0.5);
        CHECK(results[i].confidence >= 0.9f);
    }
}

TEST_CASE("pitch: sine 220 Hz passes at 44.1 kHz and 96 kHz") {
    {
        YinPitch pitch(44100.0, PitchConfig{});
        const std::vector<float> signal = sineSignal(44100.0, 220.0, kAmpMinus12Dbfs, 1.0);
        const std::vector<PitchResult> results = runAll(pitch, signal);
        REQUIRE(results.size() > 2);
        for (std::size_t i = 1; i < results.size(); ++i) {
            CHECK(results[i].voiced);
            CHECK(std::abs(static_cast<double>(results[i].hz) - 220.0) <= 0.5);
            CHECK(results[i].confidence >= 0.9f);
        }
    }
    {
        // At 96 kHz the default frame is too short for minHz = 50
        // (96000/50 = 1920 > 2048/2), so the frame grows to 4096
        // (W = 2048 >= 1920).
        PitchConfig config;
        config.frameSize = 4096;
        YinPitch pitch(96000.0, config);
        const std::vector<float> signal = sineSignal(96000.0, 220.0, kAmpMinus12Dbfs, 1.0);
        const std::vector<PitchResult> results = runAll(pitch, signal);
        REQUIRE(results.size() > 2);
        for (std::size_t i = 1; i < results.size(); ++i) {
            CHECK(results[i].voiced);
            CHECK(std::abs(static_cast<double>(results[i].hz) - 220.0) <= 0.5);
            CHECK(results[i].confidence >= 0.9f);
        }
    }
}

TEST_CASE("pitch: sine 220 Hz at -60 dBFS stays within +/-0.5 Hz") {
    // Maintainer review nit on PR #25: the CMND is a ratio of energies,
    // so it should be scale-invariant in practice — but the correlation
    // goes through a float FFT while the energy terms are double, so
    // d(τ) near a true period is a difference of two large similar
    // numbers. A 48 dB quieter sine exercises that at a much smaller
    // absolute level than the -12 dBFS acceptance test.
    YinPitch pitch(kFs, PitchConfig{});
    const std::vector<float> signal = sineSignal(kFs, 220.0, 0.001, 1.0); // -60 dBFS
    const std::vector<PitchResult> results = runAll(pitch, signal);
    REQUIRE(results.size() > 2);
    for (std::size_t i = 1; i < results.size(); ++i) {
        CHECK(results[i].voiced);
        CHECK(std::abs(static_cast<double>(results[i].hz) - 220.0) <= 0.5);
        CHECK(results[i].confidence >= 0.9f);
    }
}

TEST_CASE("pitch: naive saw at 110 Hz") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::size_t count = static_cast<std::size_t>(kFs);
    std::vector<float> signal;
    signal.reserve(count);
    for (std::size_t n = 0; n < count; ++n)
        signal.push_back(sig::saw(n, kFs, 110.0, 0.5));
    const std::vector<PitchResult> results = runAll(pitch, signal);
    REQUIRE(results.size() > 2);
    for (std::size_t i = 1; i < results.size(); ++i) {
        CHECK(results[i].voiced);
        CHECK(std::abs(static_cast<double>(results[i].hz) - 110.0) <= 1.0);
    }
}

TEST_CASE("pitch: two harmonics, f0 150 Hz with 2nd harmonic at 2x amplitude") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::size_t count = static_cast<std::size_t>(kFs);
    std::vector<float> signal;
    signal.reserve(count);
    for (std::size_t n = 0; n < count; ++n) {
        const double v = static_cast<double>(sig::sine(n, kFs, 150.0, 0.3, 0.0)) +
                         static_cast<double>(sig::sine(n, kFs, 300.0, 0.6, 0.0));
        signal.push_back(static_cast<float>(v));
    }
    const std::vector<PitchResult> results = runAll(pitch, signal);
    REQUIRE(results.size() > 2);
    // No octave error: the estimate must sit at 150 Hz, not 300 Hz.
    for (std::size_t i = 1; i < results.size(); ++i) {
        CHECK(results[i].voiced);
        CHECK(std::abs(static_cast<double>(results[i].hz) - 150.0) <= 1.0);
    }
}

TEST_CASE("pitch: lcgNoise is unvoiced with confidence < 0.3") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::size_t count = static_cast<std::size_t>(kFs) * 2;
    std::vector<float> signal;
    signal.reserve(count);
    sig::LcgNoise noise(7u, 0.5);
    for (std::size_t n = 0; n < count; ++n)
        signal.push_back(noise.next());
    const std::vector<PitchResult> results = runAll(pitch, signal);
    REQUIRE(results.size() > 100);
    std::size_t ok = 0;
    float worstConfidence = 0.0f;
    for (const PitchResult& r : results) {
        worstConfidence = std::max(worstConfidence, r.confidence);
        if (!r.voiced && r.confidence < 0.3f)
            ++ok;
    }
    INFO("frames = ", results.size(), ", ok = ", ok, ", worst confidence = ", worstConfidence);
    CHECK(static_cast<double>(ok) >= 0.95 * static_cast<double>(results.size()));
}

TEST_CASE("pitch: digital silence is unvoiced with zero confidence") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::vector<float> frame(2048, 0.0f);
    const PitchResult r = pitch.analyzeFrame(frame.data());
    CHECK(!r.voiced);
    CHECK(r.confidence == 0.0f);
    CHECK(std::isfinite(r.hz));
}

TEST_CASE("pitch: sweepLog 80 to 800 Hz over 5 s tracks within 2%") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::size_t count = static_cast<std::size_t>(kFs) * 5;
    std::vector<float> signal;
    signal.reserve(count);
    for (std::size_t n = 0; n < count; ++n)
        signal.push_back(sig::sweepLog(n, kFs, 80.0, 800.0, 5.0, 0.5));
    const std::vector<PitchResult> results = runAll(pitch, signal);
    CHECK(results.size() == expectedFrames(count, 2048, 256));
    REQUIRE(!results.empty());
    // Instantaneous frequency of the log sweep at the centre sample of
    // frame i: f(t) = 80 * exp(ln(800/80) * t / 5).
    const double logRatio = std::log(800.0 / 80.0);
    std::size_t within = 0;
    double worstRel = 0.0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const double centre = static_cast<double>(i * 256 + 1024) / kFs;
        const double fInst = 80.0 * std::exp(logRatio * centre / 5.0);
        const double rel = std::abs(static_cast<double>(results[i].hz) - fInst) / fInst;
        worstRel = std::max(worstRel, rel);
        if (rel <= 0.02)
            ++within;
    }
    INFO("frames = ", results.size(), ", within 2% = ", within, ", worst relative error = ", worstRel);
    CHECK(static_cast<double>(within) >= 0.95 * static_cast<double>(results.size()));
}

TEST_CASE("pitch: random chunk sizes give the same results as one push") {
    const std::vector<float> signal = sig::goldenSignal(kFs);
    YinPitch oneShot(kFs, PitchConfig{});
    const std::vector<PitchResult> whole = runAll(oneShot, signal);
    CHECK(whole.size() == expectedFrames(signal.size(), 2048, 256));

    YinPitch chunked(kFs, PitchConfig{});
    std::vector<PitchResult> parts;
    sig::LcgNoise chunkNoise(99u, 1.0);
    std::size_t pos = 0;
    while (pos < signal.size()) {
        // Pseudo-random chunk sizes in [1, 997] from the spec's LCG.
        const double unit = (static_cast<double>(chunkNoise.next()) + 1.0) * 0.5;
        std::size_t chunk = 1 + static_cast<std::size_t>(unit * 996.0);
        chunk = std::min(chunk, signal.size() - pos);
        chunked.push(signal.data() + pos, chunk,
                     [&parts](const PitchResult& r) { parts.push_back(r); });
        pos += chunk;
    }
    REQUIRE(parts.size() == whole.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(parts[i].frameIndex == whole[i].frameIndex);
        CHECK(parts[i].voiced == whole[i].voiced);
        CHECK(parts[i].hz == whole[i].hz);
        CHECK(parts[i].confidence == whole[i].confidence);
    }
}

TEST_CASE("pitch: push and analyzeFrame do not allocate") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::vector<float> signal = sineSignal(kFs, 220.0, 0.5, 1.0);
    std::size_t results = 0;
    std::size_t allocs = 0;
    PitchResult direct{};
    {
        dsptb::test::AllocGuard guard;
        pitch.push(signal.data(), signal.size(), [&results](const PitchResult&) { ++results; });
        direct = pitch.analyzeFrame(signal.data());
        allocs = guard.count();
    }
    CHECK(direct.voiced);
    CHECK(results == expectedFrames(signal.size(), 2048, 256));
    CHECK(allocs == 0);
}

TEST_CASE("pitch: analyzeFrame direct use and reset restart the frame index") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::vector<float> signal = sineSignal(kFs, 220.0, kAmpMinus12Dbfs, 1.0);
    const PitchResult first = pitch.analyzeFrame(signal.data());
    CHECK(first.frameIndex == 0);
    CHECK(first.voiced);
    CHECK(std::abs(static_cast<double>(first.hz) - 220.0) <= 0.5);
    const PitchResult second = pitch.analyzeFrame(signal.data() + 256);
    CHECK(second.frameIndex == 1);
    pitch.reset();
    const PitchResult afterReset = pitch.analyzeFrame(signal.data());
    CHECK(afterReset.frameIndex == 0);
    CHECK(afterReset.hz == first.hz);
    CHECK(afterReset.confidence == first.confidence);
}

TEST_CASE("pitch: micro-benchmark 2048/256 (report only, budget 80 us per hop)") {
    YinPitch pitch(kFs, PitchConfig{});
    const std::vector<float> frame = sineSignal(kFs, 220.0, 0.5, 1.0);
    // Warm-up, then time repeated single-frame analyses; one analysis is
    // exactly the work done per hop at 2048/256.
    for (int i = 0; i < 20; ++i)
        pitch.analyzeFrame(frame.data());
    constexpr int kCalls = 2000;
    const auto start = std::chrono::steady_clock::now();
    double hzSum = 0.0;
    for (int i = 0; i < kCalls; ++i)
        hzSum += static_cast<double>(pitch.analyzeFrame(frame.data()).hz);
    const auto stop = std::chrono::steady_clock::now();
    const double totalUs = std::chrono::duration<double, std::micro>(stop - start).count();
    const double usPerHop = totalUs / static_cast<double>(kCalls);
    std::printf("[bench] YinPitch 2048/256: %.2f us per hop over %d calls (spec budget: 80 us)\n", usPerHop,
                kCalls);
    CHECK(hzSum > 0.0);          // the benchmark actually ran
    CHECK(usPerHop > 0.0);
    CHECK(usPerHop < 800.0);     // loose sanity bound only; the budget itself is report-only
}
