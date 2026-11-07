// PitchSpectrum: peak picking, semitone bins and pitch-class folding.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/pitch_spectrum.hpp>
#include <dsptb/dsp_common/stft.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;
namespace sig = dsptb::test::signals;

static_assert(noexcept(std::declval<PitchSpectrum&>().analyze(nullptr)));
static_assert(noexcept(std::declval<const PitchSpectrum&>().foldToPitchClasses(nullptr)));

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kFft = 8192;

struct Tone {
    double hz;
    double amp;
};

// The power spectrum of one full Hann frame of a sum of sines.
std::vector<float> frameOf(const std::vector<Tone>& tones, double fs, std::size_t fft) {
    std::vector<float> samples(fft);
    for (std::size_t n = 0; n < fft; ++n) {
        double v = 0.0;
        for (const Tone& t : tones)
            v += static_cast<double>(sig::sine(n, fs, t.hz, t.amp, 0.0));
        samples[n] = static_cast<float>(v);
    }
    StftAnalyzer stft(StftConfig{fft, fft, Window::Hann});
    std::vector<float> power;
    stft.pushPower(samples.data(), samples.size(), [&](const float* p, std::size_t bins, std::uint64_t) {
        power.assign(p, p + bins);
    });
    return power;
}

std::size_t largestBin(const PitchSpectrum& s) {
    return static_cast<std::size_t>(std::max_element(s.bins(), s.bins() + s.numBins()) - s.bins());
}

} // namespace

TEST_CASE("an in-tune sine lands on its note with balanced neighbours") {
    PitchSpectrum s(kFs, kFft);
    const auto power = frameOf({{440.0, 0.5}}, kFs, kFft);
    s.analyze(power.data());
    const std::size_t top = largestBin(s);
    CHECK(s.binMidi(top) == doctest::Approx(69.0));
    const float low = s.bins()[top - 1];
    const float high = s.bins()[top + 1];
    CHECK(low > 0.0f);
    // The log-parabola on a Hann window has a bias of about 0.16 cents here, which is 1.6 % in this ratio.
    CHECK(std::abs(low / high - 1.0f) < 0.02f);
}

TEST_CASE("an off-note sine splits by its cents") {
    PitchSpectrum s(kFs, kFft);
    const double hz = 440.0 * std::pow(2.0, 33.0 / 1200.0);
    const auto power = frameOf({{hz, 0.5}}, kFs, kFft);
    s.analyze(power.data());
    const std::size_t top = largestBin(s);
    double sum = 0.0;
    double weighted = 0.0;
    for (std::size_t i = top - 1; i <= top + 1; ++i) {
        const double v = static_cast<double>(s.bins()[i]);
        sum += v;
        weighted += v * s.binMidi(i);
    }
    CHECK(std::abs(100.0 * (weighted / sum - 69.0) - 33.0) <= 3.0);
}

TEST_CASE("the tuning moves the note") {
    const Tuning tuning{432.0};
    PitchSpectrum s(kFs, kFft, {}, tuning);
    const auto power = frameOf({{432.0, 0.5}}, kFs, kFft);
    s.analyze(power.data());
    CHECK(s.binMidi(largestBin(s)) == doctest::Approx(69.0));
    REQUIRE(s.peakCount() == 1);
    CHECK(s.peaks()[0].midi == doctest::Approx(69.0).epsilon(0.001));
}

TEST_CASE("close low notes stay apart at 96 kHz with a scaled FFT") {
    const double fs = 96000.0;
    const std::size_t fft = fftSizeForRate(fs, 8192);
    PitchSpectrum s(fs, fft);
    const auto power = frameOf({{130.81, 0.3}, {155.56, 0.3}}, fs, fft);
    s.analyze(power.data());
    REQUIRE(s.peakCount() == 2);
    float a = s.peaks()[0].hz;
    float b = s.peaks()[1].hz;
    if (a > b)
        std::swap(a, b);
    CHECK(std::abs(a - 130.81f) < 2.0f);
    CHECK(std::abs(b - 155.56f) < 2.0f);
}

TEST_CASE("the peak floor drops quiet tones") {
    const auto power = frameOf({{440.0, 0.5}, {880.0, 0.5 * std::pow(10.0, -50.0 / 20.0)}}, kFs, kFft);
    PitchSpectrum standard(kFs, kFft);
    standard.analyze(power.data());
    CHECK(standard.peakCount() == 1);

    PitchSpectrumConfig low;
    low.peakFloor = 0.001f;
    PitchSpectrum sensitive(kFs, kFft, low);
    sensitive.analyze(power.data());
    bool found = false;
    for (std::size_t i = 0; i < sensitive.peakCount(); ++i)
        found = found || std::abs(sensitive.peaks()[i].hz - 880.0f) < 2.0f;
    CHECK(found);
}

TEST_CASE("harmonic summation favours the fundamental's pitch class") {
    const double a3 = 220.0;
    std::vector<Tone> tones;
    for (int h = 1; h <= 6; ++h)
        tones.push_back({a3 * h, 0.3 / h});
    const auto power = frameOf(tones, kFs, kFft);

    // How far class A leads the strongest other class. Its share of the total falls, because subharmonics
    // also land on other notes: what summation adds is a bigger lead for the fundamental.
    const auto leadOfA = [&](int harmonics) {
        PitchSpectrumConfig config;
        config.harmonics = harmonics;
        PitchSpectrum s(kFs, kFft, config);
        s.analyze(power.data());
        std::vector<float> classes(36);
        s.foldToPitchClasses(classes.data());
        double semitones[12] = {};
        for (std::size_t i = 0; i < classes.size(); ++i)
            semitones[(i + 1) / 3 % 12] += static_cast<double>(classes[i]); // each class centred on its note
        double other = 0.0;
        for (int pc = 0; pc < 12; ++pc)
            if (pc != 9)
                other = std::max(other, semitones[pc]);
        return semitones[9] / other;
    };
    const double plain = leadOfA(1);
    const double summed = leadOfA(4);
    CAPTURE(plain);
    CAPTURE(summed);
    CHECK(summed > plain);
}

TEST_CASE("folding puts each note in its class") {
    PitchSpectrum s(kFs, kFft);
    const auto power = frameOf({{261.6256, 0.5}}, kFs, kFft); // C4
    s.analyze(power.data());
    std::vector<float> classes(36);
    s.foldToPitchClasses(classes.data());
    CHECK(std::max_element(classes.begin(), classes.end()) - classes.begin() == 0);
}

TEST_CASE("per-frame calls don't allocate and are deterministic") {
    PitchSpectrumConfig config;
    config.harmonics = 3;
    PitchSpectrum s(kFs, kFft, config);
    const auto power = frameOf({{440.0, 0.3}, {660.0, 0.2}}, kFs, kFft);
    std::vector<float> classes(36);
    std::vector<float> first;
    std::vector<float> second;
    {
        AllocGuard guard;
        s.analyze(power.data());
        (void)s.bins();
        (void)s.peaks();
        s.foldToPitchClasses(classes.data());
        s.setTuning(Tuning{});
        CHECK(guard.count() == 0);
    }
    first.assign(s.bins(), s.bins() + s.numBins());
    s.analyze(power.data());
    second.assign(s.bins(), s.bins() + s.numBins());
    CHECK(first == second);
}

TEST_CASE("invalid configurations throw") {
    PitchSpectrumConfig bad;
    bad.binsPerSemitone = 11;
    CHECK_THROWS_AS(PitchSpectrum(kFs, kFft, bad), std::invalid_argument);
    bad = {};
    bad.minHz = 2000.0;
    CHECK_THROWS_AS(PitchSpectrum(kFs, kFft, bad), std::invalid_argument);
    CHECK_THROWS_AS(PitchSpectrum(0.0, kFft), std::invalid_argument);
    CHECK_THROWS_AS(PitchSpectrum(kFs, 1000), std::invalid_argument);
}
