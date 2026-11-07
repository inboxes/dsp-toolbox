#include <doctest/doctest.h>

#include <dsptb/dsp_common/fft.hpp>
#include <dsptb/dsp_common/spectral_features.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <dsptb/dsp/window.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kFftSize = 2048;
constexpr std::size_t kHop = 512;
constexpr double kBinHz = kFs / static_cast<double>(kFftSize); // 23.4375 Hz

// Minimal extractor for the flat number arrays in the golden JSON:
// finds "key", then reads every number inside the following [ ... ].
// (Same approach as test_freq_map.cpp.)
std::vector<double> goldenArray(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    REQUIRE(keyPos != std::string::npos);
    const std::size_t open = text.find('[', keyPos);
    const std::size_t close = text.find(']', open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    std::vector<double> values;
    const char* cursor = text.c_str() + open + 1;
    const char* const end = text.c_str() + close;
    while (cursor < end) {
        char* next = nullptr;
        const double value = std::strtod(cursor, &next);
        if (next == cursor) {
            ++cursor; // separator (comma / whitespace)
            continue;
        }
        values.push_back(value);
        cursor = next;
    }
    return values;
}

std::string goldenText() {
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "golden" / "spectral_features.json";
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot open golden file: ", path.string());
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

// Runs the whole signal through `extractor` and collects the features.
std::vector<SpectralFeatures> runAll(SpectralFeatureExtractor& extractor,
                                     const std::vector<float>& signal) {
    std::vector<SpectralFeatures> out;
    extractor.push(signal.data(), signal.size(),
                   [&](const SpectralFeatures& features, std::uint64_t frame) {
                       REQUIRE(frame == out.size());
                       out.push_back(features);
                   });
    return out;
}

std::vector<float> sineSignal(double freq, double amp, std::size_t count) {
    std::vector<float> out;
    out.reserve(count);
    for (std::size_t n = 0; n < count; ++n)
        out.push_back(sig::sine(n, kFs, freq, amp, 0.0));
    return out;
}

double medianOf(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

TEST_CASE("spectral_features: config validation") {
    const auto make = [](double fs, std::size_t fft, std::size_t hop, float pct) {
        return SpectralFeatureExtractor(fs, fft, hop, pct);
    };
    CHECK_NOTHROW(make(kFs, kFftSize, kHop, 0.85f));
    CHECK_NOTHROW(make(kFs, kFftSize, kHop, 1.0f));
    CHECK_THROWS_AS(make(0.0, kFftSize, kHop, 0.85f), std::invalid_argument);
    CHECK_THROWS_AS(make(-48000.0, kFftSize, kHop, 0.85f), std::invalid_argument);
    CHECK_THROWS_AS(make(kFs, 1000, kHop, 0.85f), std::invalid_argument); // not a power of two
    CHECK_THROWS_AS(make(kFs, 16, kHop, 0.85f), std::invalid_argument);   // below RealFft's range
    CHECK_THROWS_AS(make(kFs, kFftSize, 0, 0.85f), std::invalid_argument);
    CHECK_THROWS_AS(make(kFs, kFftSize, kHop, 0.0f), std::invalid_argument);
    CHECK_THROWS_AS(make(kFs, kFftSize, kHop, 1.5f), std::invalid_argument);
}

TEST_CASE("spectral_features: golden signal matches librosa within 1e-3*max(1,|golden|)") {
    const std::string text = goldenText();
    const std::vector<double> centroid = goldenArray(text, "centroid");
    const std::vector<double> spread = goldenArray(text, "spread");
    const std::vector<double> rolloff = goldenArray(text, "rolloff");
    const std::vector<double> flatness = goldenArray(text, "flatness");
    const std::vector<double> zcr = goldenArray(text, "zcr");
    const std::size_t frames = centroid.size();
    REQUIRE(frames == 90);
    REQUIRE(spread.size() == frames);
    REQUIRE(rolloff.size() == frames);
    REQUIRE(flatness.size() == frames);
    REQUIRE(zcr.size() == frames);

    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    const std::vector<float> signal = sig::goldenSignal(kFs);
    const std::vector<SpectralFeatures> results = runAll(extractor, signal);
    REQUIRE(results.size() == frames);

    double worstCentroid = 0.0, worstSpread = 0.0, worstRolloff = 0.0, worstFlatness = 0.0,
           worstZcr = 0.0;
    for (std::size_t t = 0; t < frames; ++t) {
        const SpectralFeatures& f = results[t];
        const auto track = [](double actual, double golden, double& worst) {
            const double tol = 1e-3 * std::max(1.0, std::abs(golden));
            const double diff = std::abs(actual - golden);
            worst = std::max(worst, diff);
            CHECK(diff <= tol);
        };
        track(f.centroidHz, centroid[t], worstCentroid);
        track(f.spreadHz, spread[t], worstSpread);
        track(f.rolloffHz, rolloff[t], worstRolloff);
        track(f.flatness, flatness[t], worstFlatness);
        track(f.zcr, zcr[t], worstZcr);
        CHECK(std::isfinite(f.flux));
        CHECK(std::isfinite(f.rms));
    }
    MESSAGE("worst |delta| vs golden: centroid ", worstCentroid, " spread ", worstSpread,
            " rolloff ", worstRolloff, " flatness ", worstFlatness, " zcr ", worstZcr);
}

TEST_CASE("spectral_features: analyzeFrame agrees with push frame by frame") {
    // Feeding the same frames through analyzeFrame with an explicitly
    // computed previous magnitude must reproduce the streamed results
    // bit-for-bit: push and analyzeFrame share one computation.
    const std::vector<float> signal = sig::goldenSignal(kFs);
    SpectralFeatureExtractor streamed(kFs, kFftSize, kHop);
    const std::vector<SpectralFeatures> expected = runAll(streamed, signal);

    RealFft fft(kFftSize);
    std::vector<float> window(kFftSize);
    dsptb::hannWindow(window.data(), kFftSize);
    std::vector<float> windowed(kFftSize);
    std::vector<float> mag(kFftSize / 2 + 1);

    SpectralFeatureExtractor direct(kFs, kFftSize, kHop);
    const float* prevMag = nullptr;
    std::vector<float> prev;
    for (std::size_t t = 0; t < expected.size(); ++t) {
        const float* frame = signal.data() + t * kHop;
        const SpectralFeatures f = direct.analyzeFrame(frame, prevMag);
        CHECK(f.centroidHz == expected[t].centroidHz);
        CHECK(f.spreadHz == expected[t].spreadHz);
        CHECK(f.rolloffHz == expected[t].rolloffHz);
        CHECK(f.flatness == expected[t].flatness);
        CHECK(f.flux == expected[t].flux);
        CHECK(f.zcr == expected[t].zcr);
        CHECK(f.rms == expected[t].rms);
        for (std::size_t i = 0; i < kFftSize; ++i)
            windowed[i] = frame[i] * window[i];
        fft.magnitudeSpectrum(windowed.data(), mag.data());
        prev = mag;
        prevMag = prev.data();
    }
    // The first frame's flux is 0 in both paths (no previous magnitude).
    CHECK(expected[0].flux == 0.0f);
}

TEST_CASE("spectral_features: 1 kHz sine centroid, rolloff, flatness and ZCR") {
    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    const std::vector<float> signal = sineSignal(1000.0, 1.0, static_cast<std::size_t>(kFs));
    const std::vector<SpectralFeatures> results = runAll(extractor, signal);
    REQUIRE(results.size() == 90);
    std::vector<double> centroids, rolloffs;
    for (const SpectralFeatures& f : results) {
        centroids.push_back(f.centroidHz);
        rolloffs.push_back(f.rolloffHz);
        CHECK(f.flatness < 0.01f);
        // 2000 crossings per second -> 2000/48000 per sample, +-1 crossing
        // per frame: |zcr * frameSize - 2000/48000 * frameSize| <= 1.
        const double crossings = static_cast<double>(f.zcr) * static_cast<double>(kFftSize);
        CHECK(std::abs(crossings - 2000.0 / kFs * static_cast<double>(kFftSize)) <= 1.0);
    }
    // Median over frames (the phase rotates through the frames, so the
    // per-frame values straddle the bin): centroid within 1 bin of
    // 1000 Hz, rolloff within 1 bin of 1000 Hz.
    CHECK(std::abs(medianOf(centroids) - 1000.0) <= kBinHz);
    CHECK(std::abs(medianOf(rolloffs) - 1000.0) <= kBinHz);
    MESSAGE("1 kHz sine: median centroid ", medianOf(centroids), " median rolloff ",
            medianOf(rolloffs));
}

TEST_CASE("spectral_features: lcgNoise flatness > 0.5 (corrected threshold)") {
    // The issue's original "> 0.8" was a spec bug (maintainer ruling on
    // issue #14): under section 4.9's power=2 definition, white-noise
    // flatness concentrates at exp(-gamma) ~= 0.5615. The corrected
    // threshold is 0.5; observed range across seeds/amplitudes is
    // 0.52-0.60.
    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    sig::LcgNoise noise(7u, 0.5);
    std::vector<float> signal;
    signal.reserve(static_cast<std::size_t>(kFs));
    for (std::size_t n = 0; n < static_cast<std::size_t>(kFs); ++n)
        signal.push_back(noise.next());
    const std::vector<SpectralFeatures> results = runAll(extractor, signal);
    REQUIRE(results.size() == 90);
    float minFlatness = 1.0f, maxFlatness = 0.0f;
    std::vector<double> flatness;
    for (const SpectralFeatures& f : results) {
        minFlatness = std::min(minFlatness, f.flatness);
        maxFlatness = std::max(maxFlatness, f.flatness);
        flatness.push_back(f.flatness);
        CHECK(f.flatness > 0.5f);
    }
    MESSAGE("lcgNoise flatness: min ", minFlatness, " median ", medianOf(flatness), " max ",
            maxFlatness);
}

TEST_CASE("spectral_features: RMS of a -6 dBFS sine frame is -9.03 dBFS") {
    // -6 dBFS peak amplitude; a sine's RMS is peak/sqrt(2), i.e. -9.03 dBFS.
    const double amp = std::pow(10.0, -6.0 / 20.0);
    std::vector<float> frame(kFftSize);
    for (std::size_t n = 0; n < kFftSize; ++n)
        frame[n] = sig::sine(n, kFs, 1000.0, amp, 0.0);
    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    const SpectralFeatures f = extractor.analyzeFrame(frame.data(), nullptr);
    const double rmsDb = 20.0 * std::log10(static_cast<double>(f.rms));
    MESSAGE("RMS of -6 dBFS sine frame: ", rmsDb, " dBFS");
    CHECK(std::abs(rmsDb - (-9.03)) <= 0.05);
}

TEST_CASE("spectral_features: silence gives the section 4.9 readings with no NaN") {
    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    const std::vector<float> zeros(kFftSize, 0.0f);
    const SpectralFeatures f = extractor.analyzeFrame(zeros.data(), nullptr);
    CHECK(f.centroidHz == 0.0f);
    CHECK(f.spreadHz == 0.0f);
    CHECK(f.rolloffHz == 0.0f);
    CHECK(f.flatness == 1.0f);
    CHECK(f.flux == 0.0f);
    CHECK(f.zcr == 0.0f);
    CHECK(f.rms == 0.0f);

    // Streamed silence: every frame reads the same, flux stays 0.
    const std::vector<float> silence(static_cast<std::size_t>(kFs), 0.0f);
    const std::vector<SpectralFeatures> results = runAll(extractor, silence);
    REQUIRE(results.size() == 90);
    for (const SpectralFeatures& r : results) {
        CHECK(r.centroidHz == 0.0f);
        CHECK(r.spreadHz == 0.0f);
        CHECK(r.rolloffHz == 0.0f);
        CHECK(r.flatness == 1.0f);
        CHECK(r.flux == 0.0f);
        CHECK(std::isfinite(r.centroidHz));
        CHECK(std::isfinite(r.spreadHz));
        CHECK(std::isfinite(r.rolloffHz));
        CHECK(std::isfinite(r.flatness));
        CHECK(std::isfinite(r.zcr));
        CHECK(std::isfinite(r.rms));
    }
}

TEST_CASE("spectral_features: flux is 0 on identical frames and > 0 at an amplitude step") {
    // Identical frames: analyzeFrame with the frame's own magnitude as
    // the previous magnitude gives exactly 0.
    const std::vector<float> frame = sineSignal(440.0, 0.5, kFftSize);
    RealFft fft(kFftSize);
    std::vector<float> window(kFftSize);
    dsptb::hannWindow(window.data(), kFftSize);
    std::vector<float> windowed(kFftSize);
    std::vector<float> mag(kFftSize / 2 + 1);
    for (std::size_t i = 0; i < kFftSize; ++i)
        windowed[i] = frame[i] * window[i];
    fft.magnitudeSpectrum(windowed.data(), mag.data());

    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    CHECK(extractor.analyzeFrame(frame.data(), nullptr).flux == 0.0f);
    CHECK(extractor.analyzeFrame(frame.data(), mag.data()).flux == 0.0f);

    // Streamed amplitude step 0.25 -> 0.5 at 0.5 s: flux is 0 on the
    // first frame and positive on the frames straddling the step.
    std::vector<float> stepped;
    stepped.reserve(static_cast<std::size_t>(kFs));
    for (std::size_t n = 0; n < static_cast<std::size_t>(kFs); ++n)
        stepped.push_back(sig::sine(n, kFs, 440.0, n < 24000 ? 0.25 : 0.5, 0.0));
    SpectralFeatureExtractor streamed(kFs, kFftSize, kHop);
    const std::vector<SpectralFeatures> results = runAll(streamed, stepped);
    REQUIRE(results.size() == 90);
    CHECK(results[0].flux == 0.0f);
    float maxFlux = 0.0f;
    for (const SpectralFeatures& r : results)
        maxFlux = std::max(maxFlux, r.flux);
    CHECK(maxFlux > 0.0f);
}

TEST_CASE("spectral_features: push is chunk-size invariant and reset restarts the stream") {
    const std::vector<float> signal = sig::goldenSignal(kFs);
    SpectralFeatureExtractor oneShot(kFs, kFftSize, kHop);
    const std::vector<SpectralFeatures> expected = runAll(oneShot, signal);

    SpectralFeatureExtractor chunked(kFs, kFftSize, kHop);
    std::vector<SpectralFeatures> actual;
    const std::size_t chunks[] = {1, 7, 100, 1023, 5000, 3};
    std::size_t pos = 0, ci = 0;
    while (pos < signal.size()) {
        const std::size_t take = std::min(chunks[ci++ % 6], signal.size() - pos);
        chunked.push(signal.data() + pos, take,
                     [&](const SpectralFeatures& features, std::uint64_t) {
                         actual.push_back(features);
                     });
        pos += take;
    }
    REQUIRE(actual.size() == expected.size());
    for (std::size_t t = 0; t < expected.size(); ++t) {
        CHECK(actual[t].centroidHz == expected[t].centroidHz);
        CHECK(actual[t].flux == expected[t].flux);
        CHECK(actual[t].rms == expected[t].rms);
    }

    // After reset the stream restarts: same input, same results, and the
    // first frame's flux is 0 again.
    chunked.reset();
    const std::vector<SpectralFeatures> again = runAll(chunked, signal);
    REQUIRE(again.size() == expected.size());
    CHECK(again[0].flux == 0.0f);
    CHECK(again[0].centroidHz == expected[0].centroidHz);
}

TEST_CASE("spectral_features: no allocation in push or analyzeFrame") {
    SpectralFeatureExtractor extractor(kFs, kFftSize, kHop);
    const std::vector<float> signal = sig::goldenSignal(kFs);
    // Warm up (first push fills the stream), then measure.
    extractor.push(signal.data(), signal.size(),
                   [](const SpectralFeatures&, std::uint64_t) {});
    extractor.reset();
    const std::vector<float> frame(signal.begin(), signal.begin() + kFftSize);
    std::size_t callbacks = 0;
    {
        AllocGuard guard;
        extractor.push(signal.data(), signal.size(),
                       [&](const SpectralFeatures&, std::uint64_t) { ++callbacks; });
        (void)extractor.analyzeFrame(frame.data(), nullptr);
        CHECK(guard.count() == 0);
    }
    CHECK(callbacks == 90);
}
