// LpcVoiceModel tests. Pre-emphasis is part of the signal path, so
// `analyze` filters the pre-emphasised signal and `synthesize` applies the
// inverse one-pole de-emphasis after G/A'(z).
//
// "Vowel" = an impulse train at 120 Hz through three two-pole resonators at
// 700, 1220 and 2600 Hz (bandwidths 80, 90, 120 Hz). "Measure formants" =
// run a second LpcVoiceModel (no warp) on the output and peak-pick
// envelopeDb(..., false) between 200 and 4000 Hz. The residual crosses the
// API as float, so the identity criteria are checked with the float
// residual buffer a caller actually passes.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/fft.hpp>
#include <dsptb/dsp_common/lpc.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using dsptb::dsp::LpcConfig;
using dsptb::dsp::LpcVoiceModel;
using dsptb::dsp::RealFft;
using dsptb::dsp::WarpSettings;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Two-pole resonator: y[n] = x[n] + 2r cos(theta) y[n-1] - r^2 y[n-2],
// r = exp(-pi*BW/fs), theta = 2*pi*f/fs.
std::vector<float> resonator(const std::vector<float>& x, double fs, double f, double bw) {
    const double r = std::exp(-kPi * bw / fs);
    const double theta = 2.0 * kPi * f / fs;
    const double b1 = 2.0 * r * std::cos(theta);
    const double b2 = -r * r;
    std::vector<float> y(x.size(), 0.0f);
    for (std::size_t n = 0; n < x.size(); ++n) {
        double v = static_cast<double>(x[n]);
        if (n >= 1)
            v += b1 * static_cast<double>(y[n - 1]);
        if (n >= 2)
            v += b2 * static_cast<double>(y[n - 2]);
        y[n] = static_cast<float>(v);
    }
    return y;
}

// The test vowel: impulse train at 120 Hz through three two-pole
// resonators, normalised to peak 0.5.
std::vector<float> vowelSignal(double fs, double seconds) {
    const std::size_t count = static_cast<std::size_t>(fs * seconds);
    std::vector<float> imp(count, 0.0f);
    const std::size_t period = static_cast<std::size_t>(std::lround(fs / 120.0));
    for (std::size_t n = 0; n < count; n += period)
        imp[n] = 1.0f;
    std::vector<float> y = resonator(imp, fs, 700.0, 80.0);
    y = resonator(y, fs, 1220.0, 90.0);
    y = resonator(y, fs, 2600.0, 120.0);
    double peak = 0.0;
    for (const float v : y)
        peak = std::max(peak, std::fabs(static_cast<double>(v)));
    for (float& v : y)
        v = static_cast<float>(static_cast<double>(v) / peak * 0.5);
    return y;
}

// The same vowel filter excited by white noise instead of a 120 Hz pulse
// train. A harmonic source samples the envelope only at multiples of f0, so
// an LPC measurement of it is pulled towards the nearest harmonic; noise
// shows the formant filter itself.
std::vector<float> noiseVowelSignal(double fs, double seconds) {
    const std::size_t count = static_cast<std::size_t>(fs * seconds);
    sig::LcgNoise lcg(7u, 0.5);
    std::vector<float> src(count);
    for (float& v : src)
        v = lcg.next();
    std::vector<float> y = resonator(src, fs, 700.0, 80.0);
    y = resonator(y, fs, 1220.0, 90.0);
    y = resonator(y, fs, 2600.0, 120.0);
    double peak = 0.0;
    for (const float v : y)
        peak = std::max(peak, std::fabs(static_cast<double>(v)));
    for (float& v : y)
        v = static_cast<float>(static_cast<double>(v) / peak * 0.5);
    return y;
}

// Saw 150 Hz + lcgNoise at -30 dB (the identity test signal).
std::vector<float> identitySignal(double fs, double seconds) {
    const std::size_t count = static_cast<std::size_t>(fs * seconds);
    std::vector<float> out;
    out.reserve(count);
    sig::LcgNoise noise(1u, std::pow(10.0, -30.0 / 20.0) * 0.5);
    for (std::size_t n = 0; n < count; ++n) {
        const double saw = 0.5 * (2.0 * (std::fmod(150.0 * static_cast<double>(n) / fs, 1.0)) - 1.0);
        out.push_back(static_cast<float>(saw + static_cast<double>(noise.next())));
    }
    return out;
}

double rms(const std::vector<float>& x) {
    double acc = 0.0;
    for (const float v : x)
        acc += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(acc / static_cast<double>(x.size()));
}

double maxAbs(const std::vector<float>& x) {
    double m = 0.0;
    for (const float v : x)
        m = std::max(m, std::fabs(static_cast<double>(v)));
    return m;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return m;
}

// Alternating analyze/synthesize in fixed blocks, as the API contract
// requires (the synthesis coefficient history only spans the last few
// hops). The residual crosses the API as float throughout.
std::vector<float> runPair(LpcVoiceModel& model, const std::vector<float>& x,
                           std::size_t block = 256) {
    std::vector<float> y(x.size(), 0.0f);
    std::vector<float> residual(block, 0.0f);
    for (std::size_t i = 0; i < x.size(); i += block) {
        const std::size_t n = std::min(block, x.size() - i);
        model.analyze(x.data() + i, residual.data(), n);
        model.synthesize(residual.data(), y.data() + i, n);
    }
    return y;
}

// Analyze only, in fixed blocks; returns the residual.
std::vector<float> runAnalyze(LpcVoiceModel& model, const std::vector<float>& x,
                              std::size_t block = 256) {
    std::vector<float> residual(x.size(), 0.0f);
    for (std::size_t i = 0; i < x.size(); i += block) {
        const std::size_t n = std::min(block, x.size() - i);
        model.analyze(x.data() + i, residual.data() + i, n);
    }
    return residual;
}

// Peak-pick a dB envelope on a uniform grid over 0..fs/2: local maxima in
// [loHz, hiHz] with parabolic interpolation on dB, top 3 by magnitude,
// sorted by frequency. Returns the frequencies.
std::vector<double> envelopePeaks(const std::vector<float>& envDb, double fs, double loHz,
                                  double hiHz) {
    const std::size_t points = envDb.size();
    const double df = (fs / 2.0) / static_cast<double>(points - 1);
    struct Peak {
        double hz;
        double db;
    };
    std::vector<Peak> peaks;
    for (std::size_t i = 1; i + 1 < points; ++i) {
        const double f = static_cast<double>(i) * df;
        if (f < loHz || f > hiHz)
            continue;
        const double y0 = static_cast<double>(envDb[i - 1]);
        const double y1 = static_cast<double>(envDb[i]);
        const double y2 = static_cast<double>(envDb[i + 1]);
        if (y1 > y0 && y1 >= y2) {
            const double denom = y0 - 2.0 * y1 + y2;
            const double d = denom != 0.0 ? 0.5 * (y0 - y2) / denom : 0.0;
            peaks.push_back(Peak{f + d * df, y1});
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
    if (peaks.size() > 3)
        peaks.resize(3);
    std::vector<double> out;
    for (const Peak& p : peaks)
        out.push_back(p.hz);
    std::sort(out.begin(), out.end());
    return out;
}

// A formant measurement: a second LpcVoiceModel (no warp) on the
// output, peak-pick envelopeDb(..., false) between 200 and 4000 Hz.
std::vector<double> measureFormants(const std::vector<float>& signal, double fs) {
    LpcVoiceModel meas(fs, LpcConfig{});
    (void)runAnalyze(meas, signal);
    std::vector<float> env(4096, 0.0f);
    meas.envelopeDb(env.data(), env.size(), false);
    return envelopePeaks(env, fs, 200.0, 4000.0);
}

// 1/3-octave band level (dB, mean power in the band) of a signal, averaged
// over Hann-windowed 8192-sample frames stepped by 4096 (Welch-style), via
// RealFft. The same framing is used for input and output, so level
// differences are exact and stable across noise realisations.
double thirdOctaveLevel(const std::vector<float>& signal, double fs, double fc) {
    constexpr std::size_t kFft = 8192;
    constexpr std::size_t kStep = 4096;
    RealFft fft(kFft);
    std::vector<std::complex<float>> spec(fft.bins());
    std::vector<float> frame(kFft, 0.0f);
    const double lo = fc / std::pow(2.0, 1.0 / 6.0);
    const double hi = fc * std::pow(2.0, 1.0 / 6.0);
    double acc = 0.0;
    std::size_t count = 0;
    std::size_t frames = 0;
    for (std::size_t start = 0; start + kFft <= signal.size(); start += kStep) {
        for (std::size_t i = 0; i < kFft; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                    static_cast<double>(kFft - 1));
            frame[i] = static_cast<float>(static_cast<double>(signal[start + i]) * hann);
        }
        fft.forward(frame.data(), spec.data());
        for (std::size_t k = 0; k < fft.bins(); ++k) {
            const double f = static_cast<double>(k) * fs / static_cast<double>(kFft);
            if (f >= lo && f < hi) {
                acc += std::norm(spec[k]);
                ++count;
            }
        }
        ++frames;
    }
    REQUIRE(frames > 0);
    REQUIRE(count > 0);
    return 10.0 * std::log10(acc / static_cast<double>(count));
}

// Harmonic amplitudes (dB) of a segment at multiples of f0, via Goertzel
// with a Hann window. Returns the max-min span over [loHz, hiHz].
double harmonicSpan(const std::vector<float>& seg, double fs, double f0, double loHz,
                    double hiHz) {
    const std::size_t n = seg.size();
    double lo = 1e30;
    double hi = -1e30;
    for (double f = f0 * std::ceil(loHz / f0); f <= hiHz; f += f0) {
        const double w = 2.0 * kPi * f / fs;
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                    static_cast<double>(n - 1));
            const double v = static_cast<double>(seg[i]) * hann;
            re += v * std::cos(w * static_cast<double>(i));
            im -= v * std::sin(w * static_cast<double>(i));
        }
        const double db = 20.0 * std::log10(std::hypot(re, im) / (static_cast<double>(n) / 2.0) + 1e-30);
        lo = std::min(lo, db);
        hi = std::max(hi, db);
    }
    return hi - lo;
}

} // namespace

TEST_CASE("lpc: bad configs throw std::invalid_argument") {
    CHECK_THROWS_AS((LpcVoiceModel(0.0, LpcConfig{})), std::invalid_argument);
    CHECK_THROWS_AS((LpcVoiceModel(-48000.0, LpcConfig{})), std::invalid_argument);
    LpcConfig badHop;
    badHop.hop = 0;
    CHECK_THROWS_AS((LpcVoiceModel(kFs, badHop)), std::invalid_argument);
    LpcConfig badBlock;
    badBlock.maxBlock = 0;
    CHECK_THROWS_AS((LpcVoiceModel(kFs, badBlock)), std::invalid_argument);
    LpcConfig badOrder;
    badOrder.order = 129;
    CHECK_THROWS_AS((LpcVoiceModel(kFs, badOrder)), std::invalid_argument);
    LpcConfig badWindow;
    badWindow.order = 52;
    badWindow.window = 103; // < 2*order
    CHECK_THROWS_AS((LpcVoiceModel(kFs, badWindow)), std::invalid_argument);
    LpcConfig badDelay;
    badDelay.synthDelay = 4 * 256 + 1;
    CHECK_THROWS_AS((LpcVoiceModel(kFs, badDelay)), std::invalid_argument);
    // Edge cases that must NOT throw.
    LpcConfig edgeOrder;
    edgeOrder.order = 128;
    CHECK_NOTHROW((LpcVoiceModel(kFs, edgeOrder)));
    LpcConfig edgeWindow;
    edgeWindow.order = 52;
    edgeWindow.window = 104; // == 2*order
    CHECK_NOTHROW((LpcVoiceModel(kFs, edgeWindow)));
    LpcConfig edgeDelay;
    edgeDelay.synthDelay = 4 * 256; // == 4*hop
    CHECK_NOTHROW((LpcVoiceModel(kFs, edgeDelay)));
}

TEST_CASE("lpc: auto order and window") {
    CHECK(LpcVoiceModel(44100.0, LpcConfig{}).order() == 48); // round(44.1)+4
    CHECK(LpcVoiceModel(48000.0, LpcConfig{}).order() == 52); // round(48)+4
    CHECK(LpcVoiceModel(96000.0, LpcConfig{}).order() == 100); // min(104, 100)
    CHECK(LpcVoiceModel(192000.0, LpcConfig{}).order() == 104); // capped
    CHECK(LpcVoiceModel(48000.0, LpcConfig{}).hop() == 256);
    LpcConfig explicitOrder;
    explicitOrder.order = 12;
    CHECK(LpcVoiceModel(kFs, explicitOrder).order() == 12);
}

TEST_CASE("lpc: eq band centres are 100 * 128^(i/9) Hz") {
    float centres[10];
    LpcVoiceModel::eqBandCentres(centres);
    CHECK(std::fabs(static_cast<double>(centres[0]) - 100.0) < 0.01);
    // Band 7 (1-based) is index 6, ~2540 Hz (the EQ test band).
    CHECK(std::fabs(static_cast<double>(centres[6]) - 2540.0) / 2540.0 < 0.01);
    CHECK(std::fabs(static_cast<double>(centres[9]) - 12800.0) < 1.0);
}

TEST_CASE("lpc: identity, no warp, float residual (ruling condition 1)") {
    const std::vector<float> x = identitySignal(kFs, 5.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    const std::vector<float> y = runPair(model, x);
    const double err = maxAbsDiff(y, x);
    std::printf("[lpc] identity max abs error (float residual): %.3e (bar 1e-5)\n", err);
    CHECK(err <= 1e-5);
}

TEST_CASE("lpc: delayed identity, synthDelay 48, float residual") {
    const std::vector<float> x = identitySignal(kFs, 5.0);
    LpcConfig config;
    config.synthDelay = 48;
    LpcVoiceModel model(kFs, config);
    // The residual is delayed by 48 frames in the test before synthesize;
    // the model must use the coefficients of 48 frames ago to match.
    std::vector<float> y(x.size(), 0.0f);
    std::vector<float> delayLine(48, 0.0f);
    std::vector<float> residual(256, 0.0f);
    std::vector<float> delayed(256, 0.0f);
    for (std::size_t i = 0; i < x.size(); i += 256) {
        const std::size_t n = std::min<std::size_t>(256, x.size() - i);
        model.analyze(x.data() + i, residual.data(), n);
        for (std::size_t j = 0; j < n; ++j) {
            delayed[j] = delayLine[0];
            for (std::size_t k = 0; k + 1 < delayLine.size(); ++k)
                delayLine[k] = delayLine[k + 1];
            delayLine.back() = residual[j];
        }
        model.synthesize(delayed.data(), y.data() + i, n);
    }
    std::vector<float> xDelayed(x.size(), 0.0f);
    for (std::size_t i = 48; i < x.size(); ++i)
        xDelayed[i] = x[i - 48];
    const double err = maxAbsDiff(y, xDelayed);
    std::printf("[lpc] delayed identity max abs error (float residual): %.3e (bar 1e-5)\n", err);
    CHECK(err <= 1e-5);
}

TEST_CASE("lpc: exact bypass with identity warp settings") {
    const std::vector<float> x = vowelSignal(kFs, 2.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    model.setWarp(WarpSettings{}); // phi=1, tilt=0, EQ=0
    (void)runAnalyze(model, x);
    std::vector<float> warped(1024, 0.0f);
    std::vector<float> plain(1024, 0.0f);
    model.envelopeDb(warped.data(), warped.size(), true);
    model.envelopeDb(plain.data(), plain.size(), false);
    // Bit-identical: the warp is skipped, so the synthesis coefficients are
    // copies of the analysis ones, not a recomputed refit.
    CHECK(warped == plain);
}

TEST_CASE("lpc: chunking gives the same output as fixed 256-frame calls; in == out works") {
    const std::vector<float> x = identitySignal(kFs, 2.0);
    LpcVoiceModel ref(kFs, LpcConfig{});
    const std::vector<float> yRef = runPair(ref, x, 256);
    // Random chunk sizes 1..maxBlock (seeded LCG).
    LpcVoiceModel chunked(kFs, LpcConfig{});
    std::vector<float> yChunked(x.size(), 0.0f);
    std::vector<float> residual(4096, 0.0f);
    std::uint32_t state = 12345u;
    std::size_t i = 0;
    while (i < x.size()) {
        state = 1664525u * state + 1013904223u;
        const std::size_t n = std::min<std::size_t>(1 + state % 4096, x.size() - i);
        chunked.analyze(x.data() + i, residual.data(), n);
        chunked.synthesize(residual.data(), yChunked.data() + i, n);
        i += n;
    }
    const double chunkErr = maxAbsDiff(yChunked, yRef);
    std::printf("[lpc] chunking max abs diff: %.3e (bar 1e-6)\n", chunkErr);
    CHECK(chunkErr <= 1e-6);
    // in == out for both calls.
    LpcVoiceModel inPlace(kFs, LpcConfig{});
    std::vector<float> buf = x;
    for (std::size_t j = 0; j < x.size(); j += 256) {
        const std::size_t n = std::min<std::size_t>(256, x.size() - j);
        inPlace.analyze(buf.data() + j, buf.data() + j, n);
        inPlace.synthesize(buf.data() + j, buf.data() + j, n);
    }
    const double inPlaceErr = maxAbsDiff(buf, yRef);
    std::printf("[lpc] in == out max abs diff: %.3e (bar 1e-6)\n", inPlaceErr);
    CHECK(inPlaceErr <= 1e-6);
}

TEST_CASE("lpc: envelope peaks at 700, 1220, 2600 Hz +-5%") {
    const std::vector<float> x = vowelSignal(kFs, 3.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    (void)runAnalyze(model, x);
    std::vector<float> env(4096, 0.0f);
    model.envelopeDb(env.data(), env.size(), false);
    const std::vector<double> peaks = envelopePeaks(env, kFs, 200.0, 4000.0);
    REQUIRE(peaks.size() == 3);
    std::printf("[lpc] envelope peaks: %.1f, %.1f, %.1f Hz (targets 700, 1220, 2600)\n", peaks[0],
                peaks[1], peaks[2]);
    CHECK(std::fabs(peaks[0] - 700.0) / 700.0 <= 0.05);
    CHECK(std::fabs(peaks[1] - 1220.0) / 1220.0 <= 0.05);
    CHECK(std::fabs(peaks[2] - 2600.0) / 2600.0 <= 0.05);
}

TEST_CASE("lpc: flat residual, 12 dB span (input span > 25 dB)") {
    const std::vector<float> x = vowelSignal(kFs, 3.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    const std::vector<float> residual = runAnalyze(model, x);
    const std::size_t start = static_cast<std::size_t>(kFs * 1.0);
    const std::size_t len = static_cast<std::size_t>(kFs * 1.0);
    const std::vector<float> resSeg(residual.begin() + static_cast<std::ptrdiff_t>(start),
                                    residual.begin() + static_cast<std::ptrdiff_t>(start + len));
    const std::vector<float> inSeg(x.begin() + static_cast<std::ptrdiff_t>(start),
                                   x.begin() + static_cast<std::ptrdiff_t>(start + len));
    const double resSpan = harmonicSpan(resSeg, kFs, 120.0, 200.0, 4000.0);
    const double inSpan = harmonicSpan(inSeg, kFs, 120.0, 200.0, 4000.0);
    std::printf("[lpc] residual harmonic span: %.2f dB (bar 12); input span: %.2f dB (must be > 25)\n",
                resSpan, inSpan);
    CHECK(resSpan <= 12.0);
    CHECK(inSpan > 25.0);
}

TEST_CASE("lpc: envelope test at 44.1, 48 and 96 kHz (auto order and window)") {
    for (const double fs : {44100.0, 48000.0, 96000.0}) {
        const std::vector<float> x = vowelSignal(fs, 2.0);
        LpcVoiceModel model(fs, LpcConfig{});
        (void)runAnalyze(model, x);
        std::vector<float> env(4096, 0.0f);
        model.envelopeDb(env.data(), env.size(), false);
        const std::vector<double> peaks = envelopePeaks(env, fs, 200.0, 4000.0);
        REQUIRE(peaks.size() == 3);
        INFO("fs = ", fs);
        std::printf("[lpc] fs %.0f: envelope peaks %.1f, %.1f, %.1f Hz\n", fs, peaks[0], peaks[1],
                    peaks[2]);
        CHECK(std::fabs(peaks[0] - 700.0) / 700.0 <= 0.05);
        CHECK(std::fabs(peaks[1] - 1220.0) / 1220.0 <= 0.05);
        CHECK(std::fabs(peaks[2] - 2600.0) / 2600.0 <= 0.05);
    }
}

// Averaged LPC envelope (dB) of a noisy signal: every hop of the last
// second, so one frame's estimation noise doesn't decide the peaks.
std::vector<float> averagedEnvelope(const std::vector<float>& signal, double fs) {
    LpcVoiceModel meas(fs, LpcConfig{});
    std::vector<float> residual(meas.hop());
    std::vector<float> env(4096, 0.0f);
    std::vector<double> sum(env.size(), 0.0);
    std::size_t hops = 0;
    const std::size_t from = signal.size() - static_cast<std::size_t>(fs);
    for (std::size_t i = 0; i + meas.hop() <= signal.size(); i += meas.hop()) {
        meas.analyze(signal.data() + i, residual.data(), meas.hop());
        if (i < from)
            continue;
        meas.envelopeDb(env.data(), env.size(), false);
        for (std::size_t k = 0; k < env.size(); ++k)
            sum[k] += static_cast<double>(env[k]);
        ++hops;
    }
    for (std::size_t k = 0; k < env.size(); ++k)
        env[k] = static_cast<float>(sum[k] / static_cast<double>(hops));
    return env;
}

// The local envelope maximum nearest to `target` (Hz), or 0 if none.
double nearestPeak(const std::vector<float>& envDb, double fs, double target) {
    const double df = (fs / 2.0) / static_cast<double>(envDb.size() - 1);
    double best = 0.0;
    for (std::size_t i = 1; i + 1 < envDb.size(); ++i) {
        if (envDb[i] > envDb[i - 1] && envDb[i] >= envDb[i + 1]) {
            const double f = static_cast<double>(i) * df;
            if (best == 0.0 || std::fabs(f - target) < std::fabs(best - target))
                best = f;
        }
    }
    return best;
}

double envelopeAt(const std::vector<float>& envDb, double fs, double hz) {
    const double df = (fs / 2.0) / static_cast<double>(envDb.size() - 1);
    return static_cast<double>(envDb[static_cast<std::size_t>(std::lround(hz / df))]);
}

// The formant filter must move by phi.
//  * The warp itself: the warped envelope of the harmonic vowel has its
//    three formants at phi times the input's, +-5 %.
//  * End to end (analyze, warp, synthesize) on the noise-excited vowel:
//    an output peak within +-7 % of each target. Noise is used because a
//    second LPC analysis of the harmonic output is pulled towards the nearest
//    120 Hz harmonic. The bar is wider than the warp's because the residual
//    keeps a 3-4 dB trace of the original formants (the frequency
//    resolution of a 21 ms causal window), which pulls a shifted formant
//    that lands near an original one.
//  * That trace stays small: in the residual of the noise vowel, the level at
//    each original formant is at most 6 dB above the level between them.
TEST_CASE("lpc: formant shift phi 1.25 and 0.8") {
    const std::vector<float> x = vowelSignal(kFs, 3.0);
    const std::vector<float> noisy = noiseVowelSignal(kFs, 3.0);
    for (const auto& [phi, t0, t1, t2] : std::array<std::array<double, 4>, 2>{
             {{{1.25, 875.0, 1525.0, 3250.0}}, {{0.8, 560.0, 976.0, 2080.0}}}}) {
        WarpSettings warp;
        warp.formantFactor = static_cast<float>(phi);
        INFO("phi = ", phi);

        LpcVoiceModel model(kFs, LpcConfig{});
        model.setWarp(warp);
        (void)runPair(model, x);
        std::vector<float> env(4096, 0.0f);
        model.envelopeDb(env.data(), env.size(), true);
        const std::vector<double> warped = envelopePeaks(env, kFs, 200.0, 4000.0);
        REQUIRE(warped.size() == 3);
        CHECK(std::fabs(warped[0] - t0) / t0 <= 0.05);
        CHECK(std::fabs(warped[1] - t1) / t1 <= 0.05);
        CHECK(std::fabs(warped[2] - t2) / t2 <= 0.05);

        LpcVoiceModel chain(kFs, LpcConfig{});
        chain.setWarp(warp);
        const std::vector<float> out = averagedEnvelope(runPair(chain, noisy), kFs);
        const double m0 = nearestPeak(out, kFs, t0);
        const double m1 = nearestPeak(out, kFs, t1);
        const double m2 = nearestPeak(out, kFs, t2);
        std::printf("[lpc] phi %.2f: warped envelope %.1f, %.1f, %.1f Hz; noise-vowel output %.1f, %.1f, %.1f Hz "
                    "(targets %.0f, %.0f, %.0f)\n",
                    phi, warped[0], warped[1], warped[2], m0, m1, m2, t0, t1, t2);
        CHECK(std::fabs(m0 - t0) / t0 <= 0.07);
        CHECK(std::fabs(m1 - t1) / t1 <= 0.07);
        CHECK(std::fabs(m2 - t2) / t2 <= 0.07);
    }

    // The trace of the original formants in the residual.
    LpcVoiceModel model(kFs, LpcConfig{});
    const std::vector<float> residual = runAnalyze(model, noisy);
    // Welch-averaged power at one frequency (Hann frames of 4096, hop 2048,
    // skipping the first second while the model settles).
    const auto powerDb = [&](double f) {
        constexpr std::size_t kFrame = 4096;
        double acc = 0.0;
        std::size_t frames = 0;
        for (std::size_t st = static_cast<std::size_t>(kFs); st + kFrame <= residual.size(); st += kFrame / 2) {
            double re = 0.0;
            double im = 0.0;
            for (std::size_t i = 0; i < kFrame; ++i) {
                const double h = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (kFrame - 1.0));
                const double v = static_cast<double>(residual[st + i]) * h;
                const double w = 2.0 * kPi * f * static_cast<double>(i) / kFs;
                re += v * std::cos(w);
                im -= v * std::sin(w);
            }
            acc += re * re + im * im;
            ++frames;
        }
        return 10.0 * std::log10(acc / static_cast<double>(frames));
    };
    const double between = (powerDb(400.0) + powerDb(950.0) + powerDb(1800.0) + powerDb(3500.0)) / 4.0;
    for (const double f : {700.0, 1220.0, 2600.0}) {
        const double trace = powerDb(f) - between;
        std::printf("[lpc] residual trace at %.0f Hz: %+.1f dB (bar 6)\n", f, trace);
        CHECK(trace <= 6.0);
    }
}

TEST_CASE("lpc: warp level within +-1.5 dB (phi 1.25 and 0.8, vowel and lcgNoise)") {
    const std::vector<float> vowel = vowelSignal(kFs, 3.0);
    std::vector<float> noise;
    {
        sig::LcgNoise lcg(1u, 0.3);
        noise.reserve(static_cast<std::size_t>(kFs * 3.0));
        for (std::size_t n = 0; n < static_cast<std::size_t>(kFs * 3.0); ++n)
            noise.push_back(lcg.next());
    }
    for (const double phi : {1.25, 0.8}) {
        for (const auto& [name, sigIn] : std::array<std::pair<const char*, const std::vector<float>*>, 2>{
                 {{"vowel", &vowel}, {"lcgNoise", &noise}}}) {
            LpcVoiceModel model(kFs, LpcConfig{});
            WarpSettings warp;
            warp.formantFactor = static_cast<float>(phi);
            model.setWarp(warp);
            const std::vector<float> y = runPair(model, *sigIn);
            const double levelDb = 20.0 * std::log10(rms(y) / rms(*sigIn));
            std::printf("[lpc] phi %.2f %s: level %+.2f dB (bar +-1.5)\n", phi, name, levelDb);
            INFO("phi = ", phi, ", signal = ", name);
            CHECK(std::fabs(levelDb) <= 1.5);
        }
    }
}

TEST_CASE("lpc: tilt +4 dB/oct gives +8 dB at 4 kHz vs 1 kHz") {
    std::vector<float> noise;
    {
        sig::LcgNoise lcg(1u, 0.3);
        noise.reserve(static_cast<std::size_t>(kFs * 3.0));
        for (std::size_t n = 0; n < static_cast<std::size_t>(kFs * 3.0); ++n)
            noise.push_back(lcg.next());
    }
    LpcVoiceModel model(kFs, LpcConfig{});
    WarpSettings warp;
    warp.tiltDbPerOct = 4.0f;
    model.setWarp(warp);
    const std::vector<float> y = runPair(model, noise);
    const std::size_t steady = static_cast<std::size_t>(kFs * 1.0);
    const std::vector<float> ySeg(y.begin() + static_cast<std::ptrdiff_t>(steady), y.end());
    const std::vector<float> xSeg(noise.begin() + static_cast<std::ptrdiff_t>(steady), noise.end());
    const double deltaOut = thirdOctaveLevel(ySeg, kFs, 4000.0) - thirdOctaveLevel(ySeg, kFs, 1000.0);
    const double deltaIn = thirdOctaveLevel(xSeg, kFs, 4000.0) - thirdOctaveLevel(xSeg, kFs, 1000.0);
    const double delta = deltaOut - deltaIn;
    std::printf("[lpc] tilt +4: (4k-1k) delta %+.2f dB (bar +8 +-1.5)\n", delta);
    CHECK(std::fabs(delta - 8.0) <= 1.5);
}

TEST_CASE("lpc: EQ band 7 +9 dB gives +9 dB at 2.5 kHz, flat at 500 Hz") {
    std::vector<float> noise;
    {
        sig::LcgNoise lcg(1u, 0.3);
        noise.reserve(static_cast<std::size_t>(kFs * 3.0));
        for (std::size_t n = 0; n < static_cast<std::size_t>(kFs * 3.0); ++n)
            noise.push_back(lcg.next());
    }
    LpcVoiceModel model(kFs, LpcConfig{});
    WarpSettings warp;
    // Bands are numbered 1-based here: band 7 is index 6 (centre ~2540 Hz).
    warp.eqDb[6] = 9.0f;
    model.setWarp(warp);
    const std::vector<float> y = runPair(model, noise);
    const std::size_t steady = static_cast<std::size_t>(kFs * 1.0);
    const std::vector<float> ySeg(y.begin() + static_cast<std::ptrdiff_t>(steady), y.end());
    const std::vector<float> xSeg(noise.begin() + static_cast<std::ptrdiff_t>(steady), noise.end());
    const double at25k = thirdOctaveLevel(ySeg, kFs, 2500.0) - thirdOctaveLevel(xSeg, kFs, 2500.0);
    const double at500 = thirdOctaveLevel(ySeg, kFs, 500.0) - thirdOctaveLevel(xSeg, kFs, 500.0);
    std::printf("[lpc] EQ band 7 +9: at 2.5 kHz %+.2f dB (bar +9 +-2), at 500 Hz %+.2f dB (bar +-1.5)\n",
                at25k, at500);
    CHECK(std::fabs(at25k - 9.0) <= 2.0);
    CHECK(std::fabs(at500) <= 1.5);
}

TEST_CASE("lpc: silence hold keeps coefficients during the quiet second") {
    std::vector<float> x;
    const std::vector<float> vowel = vowelSignal(kFs, 1.0);
    x.insert(x.end(), vowel.begin(), vowel.end());
    {
        sig::LcgNoise lcg(2u, 1e-4); // -80 dBFS
        for (std::size_t n = 0; n < static_cast<std::size_t>(kFs); ++n)
            x.push_back(lcg.next());
    }
    x.insert(x.end(), vowel.begin(), vowel.end());
    LpcVoiceModel model(kFs, LpcConfig{});
    model.setSilenceDb(-60.0f);
    std::vector<float> before;
    std::vector<float> after;
    std::vector<float> residual(256, 0.0f);
    std::vector<float> env(1024, 0.0f);
    // Run block by block, snapshotting the envelope (a) just after the hop
    // boundary that ends the hop straddling the vowel/quiet transition —
    // that hop legitimately updates once, because its RMS is above the
    // threshold (it still contains vowel samples) — and (b) at the end of
    // the quiet second. Every hop between the two snapshots is fully quiet,
    // so the coefficients must not change there.
    const std::size_t transitionBoundary = 48128; // first hop boundary inside the quiet second
    const std::size_t quietEnd = 2 * static_cast<std::size_t>(kFs);
    std::size_t processed = 0;
    while (processed < quietEnd) {
        const std::size_t n = std::min<std::size_t>(256, quietEnd - processed);
        model.analyze(x.data() + processed, residual.data(), n);
        processed += n;
        if (processed == transitionBoundary) {
            model.envelopeDb(env.data(), env.size(), false);
            before = env;
        }
    }
    model.envelopeDb(env.data(), env.size(), false);
    after = env;
    const double diff = maxAbsDiff(before, after);
    std::printf("[lpc] silence hold: envelope max abs diff %.3e (bar 0)\n", diff);
    CHECK(diff == 0.0);
}

TEST_CASE("lpc: stability fuzz, 60 s, random warp every hop") {
    const std::vector<float> vowel = vowelSignal(kFs, 60.0);
    std::vector<float> x = vowel;
    {
        sig::LcgNoise lcg(3u, 0.05);
        for (float& v : x)
            v = static_cast<float>(static_cast<double>(v) + static_cast<double>(lcg.next()));
    }
    LpcVoiceModel model(kFs, LpcConfig{});
    std::vector<float> y(x.size(), 0.0f);
    std::vector<float> residual(256, 0.0f);
    std::uint32_t state = 999u;
    const auto nextRand = [&state]() {
        state = 1664525u * state + 1013904223u;
        return static_cast<double>(state) / 4294967296.0; // [0, 1)
    };
    double maxY = 0.0;
    bool allFinite = true;
    for (std::size_t i = 0; i < x.size(); i += 256) {
        const std::size_t n = std::min<std::size_t>(256, x.size() - i);
        WarpSettings warp;
        warp.formantFactor = static_cast<float>(0.6 + 1.0 * nextRand()); // 0.6..1.6
        warp.tiltDbPerOct = static_cast<float>(-6.0 + 12.0 * nextRand());
        for (float& eq : warp.eqDb)
            eq = static_cast<float>(-12.0 + 24.0 * nextRand());
        model.setWarp(warp);
        model.analyze(x.data() + i, residual.data(), n);
        model.synthesize(residual.data(), y.data() + i, n);
        for (std::size_t j = i; j < i + n; ++j) {
            allFinite = allFinite && std::isfinite(static_cast<double>(y[j]));
            maxY = std::max(maxY, std::fabs(static_cast<double>(y[j])));
        }
    }
    const double maxX = maxAbs(x);
    std::printf("[lpc] fuzz: max|y| %.3f, max|x| %.3f, ratio %.2f (bar <= 8)\n", maxY, maxX,
                maxY / maxX);
    CHECK(allFinite);
    CHECK(maxY <= 8.0 * maxX);
}

TEST_CASE("lpc: reset returns to the freshly constructed state") {
    const std::vector<float> x = identitySignal(kFs, 2.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    const std::vector<float> y1 = runPair(model, x);
    model.reset();
    const std::vector<float> y2 = runPair(model, x);
    LpcVoiceModel fresh(kFs, LpcConfig{});
    const std::vector<float> yFresh = runPair(fresh, x);
    CHECK(maxAbsDiff(y1, y2) == 0.0);
    CHECK(maxAbsDiff(y2, yFresh) == 0.0);
}

TEST_CASE("lpc: analyze, synthesize, setWarp, envelopeDb and reset do not allocate") {
    const std::vector<float> x = vowelSignal(kFs, 1.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    std::vector<float> residual(256, 0.0f);
    std::vector<float> y(256, 0.0f);
    std::vector<float> env(1024, 0.0f);
    WarpSettings warp;
    warp.formantFactor = 1.25f;
    std::size_t allocs = 0;
    {
        dsptb::test::AllocGuard guard;
        for (std::size_t i = 0; i < x.size(); i += 256) {
            const std::size_t n = std::min<std::size_t>(256, x.size() - i);
            model.analyze(x.data() + i, residual.data(), n);
            model.synthesize(residual.data(), y.data(), n);
        }
        model.setWarp(warp);
        model.envelopeDb(env.data(), env.size(), false);
        model.reset();
        allocs = guard.count();
    }
    CHECK(allocs == 0);
}

TEST_CASE("lpc: micro-benchmark analyze + synthesize, 48 kHz, order 52, warp on (report only, budget 60 us per 256 frames)") {
    const std::vector<float> x = vowelSignal(kFs, 10.0);
    LpcVoiceModel model(kFs, LpcConfig{});
    WarpSettings warp;
    warp.formantFactor = 1.25f;
    model.setWarp(warp);
    std::vector<float> residual(256, 0.0f);
    std::vector<float> y(256, 0.0f);
    // Warm up (one hop update + steady state).
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFs); i += 256) {
        const std::size_t n = 256;
        model.analyze(x.data() + i, residual.data(), n);
        model.synthesize(residual.data(), y.data(), n);
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t blocks = 0;
    for (std::size_t i = 0; i < x.size(); i += 256) {
        const std::size_t n = std::min<std::size_t>(256, x.size() - i);
        model.analyze(x.data() + i, residual.data(), n);
        model.synthesize(residual.data(), y.data(), n);
        ++blocks;
    }
    const auto stop = std::chrono::steady_clock::now();
    const double totalUs = std::chrono::duration<double, std::micro>(stop - start).count();
    const double usPerBlock = totalUs / static_cast<double>(blocks);
    std::printf("[bench] LpcVoiceModel analyze + synthesize, 48 kHz, order 52, warp on: %.2f us per 256 frames over %zu blocks (budget: 60 us)\n",
                usPerBlock, blocks);
    CHECK(usPerBlock > 0.0); // the benchmark actually ran
}





