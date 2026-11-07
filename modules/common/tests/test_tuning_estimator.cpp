// TuningEstimator on key progressions at known tunings.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/stft.hpp>
#include <dsptb/dsp_common/tuning_estimator.hpp>

#include "support/alloc_counter.hpp"
#include "support/music.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;
namespace music = dsptb::test::music;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;

double trueOffset(double a4) {
    return 1200.0 * std::log2(a4 / 440.0);
}

// Feeds the signal through the same chain as estimateTuning, in chunks.
TuningEstimate estimateInChunks(const std::vector<float>& mono, std::size_t chunk) {
    const StftConfig config{8192, 4096, Window::Hann};
    StftAnalyzer stft(config);
    PitchSpectrum spectrum(kFs, config.fftSize);
    TuningEstimator estimator(kFs, config.hop);
    for (std::size_t i = 0; i < mono.size(); i += chunk) {
        stft.pushPower(mono.data() + i, std::min(chunk, mono.size() - i),
                       [&](const float* power, std::size_t, std::uint64_t) noexcept {
                           spectrum.analyze(power);
                           estimator.addPeaks(spectrum.peaks(), spectrum.peakCount());
                           estimator.addFrames(1);
                       });
    }
    return estimator.result();
}

} // namespace

TEST_CASE("the estimate finds the tuning of a progression") {
    const int roots[] = {48, 51, 54, 57}; // C3, D#3, F#3, A3
    for (const double a4 : {428.0, 432.0, 440.0, 446.0, 452.0}) {
        for (const int harmonics : {1, 6}) {
            for (const int root : roots) {
                for (const bool minor : {false, true}) {
                    const auto signal = music::progression(kFs, root, minor, a4, harmonics);
                    const TuningEstimate e = estimateTuning(signal.data(), signal.size(), kFs);
                    CAPTURE(a4);
                    CAPTURE(harmonics);
                    CAPTURE(root);
                    CAPTURE(minor);
                    CHECK(e.valid);
                    CHECK(e.confident);
                    CHECK(std::abs(static_cast<double>(e.offsetCents) - trueOffset(a4)) <= 2.0);
                }
            }
        }
    }
}

TEST_CASE("white noise has no tuning") {
    sig::LcgNoise noise(1, 0.25);
    std::vector<float> signal(static_cast<std::size_t>(10.0 * kFs));
    for (float& v : signal)
        v = noise.next();
    const TuningEstimate e = estimateTuning(signal.data(), signal.size(), kFs);
    CHECK(e.confidence < 0.2f);
    CHECK_FALSE(e.confident);
}

TEST_CASE("under 2 seconds isn't valid") {
    auto signal = music::progression(kFs, 57, false);
    signal.resize(static_cast<std::size_t>(1.5 * kFs));
    const TuningEstimate e = estimateTuning(signal.data(), signal.size(), kFs);
    CHECK_FALSE(e.valid);
    CHECK_FALSE(e.confident);
    CHECK(e.offsetCents == 0.0f);
    CHECK(e.a4Hz == 440.0);
}

TEST_CASE("silence isn't valid") {
    const std::vector<float> silence(static_cast<std::size_t>(5.0 * kFs), 0.0f);
    const TuningEstimate e = estimateTuning(silence.data(), silence.size(), kFs);
    CHECK_FALSE(e.valid);
}

TEST_CASE("the estimate doesn't depend on chunk sizes") {
    const auto signal = music::progression(kFs, 48, false, 446.0);
    const TuningEstimate whole = estimateInChunks(signal, signal.size());
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{256}, std::size_t{10000}}) {
        const TuningEstimate part = estimateInChunks(signal, chunk);
        CHECK(std::abs(part.offsetCents - whole.offsetCents) <= 0.01f);
    }
}

TEST_CASE("estimateTuning on an A = 432 progression") {
    const auto signal = music::progression(kFs, 57, true, 432.0);
    const TuningEstimate e = estimateTuning(signal.data(), signal.size(), kFs);
    CHECK(std::abs(e.offsetCents + 31.8f) <= 2.0f);
    CHECK(e.a4Hz == doctest::Approx(432.0).epsilon(0.002));
}

TEST_CASE("reset, addPeaks and result don't allocate") {
    TuningEstimator estimator;
    const SpectralPeak peaks[2] = {{440.0f, 1.0f, 69.0f}, {660.0f, 0.5f, 76.02f}};
    AllocGuard guard;
    estimator.addPeaks(peaks, 2);
    estimator.addFrames(30);
    const TuningEstimate e = estimator.result();
    estimator.reset();
    CHECK(guard.count() == 0);
    CHECK(e.valid);
    CHECK_FALSE(estimator.result().valid);
}
