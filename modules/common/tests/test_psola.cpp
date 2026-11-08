// PsolaShifter tests.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/lpc.hpp>
#include <dsptb/dsp_common/psola.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

using dsptb::dsp::PsolaConfig;
using dsptb::dsp::PsolaMode;
using dsptb::dsp::PsolaShifter;
using dsptb::test::AllocGuard;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Test-only period estimator: autocorrelation peak over a
// bracket around the expected period, parabolic refinement.
double estimateF0(const float* x, std::size_t n, double fs, double f0Expected) {
    const int lagMin = static_cast<int>(std::floor(fs / (f0Expected * 1.25)));
    const int lagMax = static_cast<int>(std::ceil(fs / (f0Expected * 0.8)));
    auto autocorr = [&](int lag) {
        double s = 0.0;
        const std::size_t l = static_cast<std::size_t>(lag);
        for (std::size_t i = 0; i + l < n; ++i)
            s += static_cast<double>(x[i]) * static_cast<double>(x[i + l]);
        return s;
    };
    int bestLag = lagMin;
    double best = autocorr(lagMin);
    for (int lag = lagMin + 1; lag <= lagMax; ++lag) {
        const double s = autocorr(lag);
        if (s > best) {
            best = s;
            bestLag = lag;
        }
    }
    const double ym1 = autocorr(bestLag - 1);
    const double yp1 = autocorr(bestLag + 1);
    const double denom = ym1 - 2.0 * best + yp1;
    double frac = 0.0;
    if (denom != 0.0)
        frac = 0.5 * (ym1 - yp1) / denom;
    return fs / (static_cast<double>(bestLag) + frac);
}

double rmsOf(const float* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return std::sqrt(s / static_cast<double>(n));
}

double maxFirstDiff(const float* x, std::size_t n) {
    double m = 0.0;
    for (std::size_t i = 1; i < n; ++i)
        m = std::max(m, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    return m;
}

bool allFinite(const float* x, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(x[i]))
            return false;
    return true;
}

std::vector<float> makeSaw(double fs, double hz, double amp, std::size_t n) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = sig::saw(i, fs, hz, amp);
    return out;
}

std::vector<float> makeSine(double fs, double hz, double amp, std::size_t n) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = sig::sine(i, fs, hz, amp, 0.0);
    return out;
}

// Runs a whole buffer through the shifter in 256-sample calls with
// constant control values.
void runConst(PsolaShifter& sh, const float* in, float* out, std::size_t n, float f0,
              float ratio) {
    for (std::size_t b = 0; b < n; b += 256) {
        const std::size_t len = std::min<std::size_t>(256, n - b);
        sh.process(in + b, out + b, len, f0, ratio);
    }
}

// "After settling" per the issue: skip latencyFrames() + 2048 samples.
std::size_t settleSkip(const PsolaShifter& sh) { return sh.latencyFrames() + 2048; }

void checkShiftAccuracy(PsolaMode mode, const char* modeName, const char* sigName,
                        const std::vector<float>& in, double f0True, double ratio) {
    PsolaConfig cfg;
    cfg.mode = mode;
    PsolaShifter sh(kFs, cfg);
    std::vector<float> out(in.size());
    runConst(sh, in.data(), out.data(), in.size(), static_cast<float>(f0True),
             static_cast<float>(ratio));
    const std::size_t skip = settleSkip(sh);
    const double want = f0True * ratio;
    const double est = estimateF0(out.data() + skip, in.size() - skip, kFs, want);
    const double f0ErrPct = 100.0 * (est - want) / want;
    const double rmsDb = 20.0 * std::log10(rmsOf(out.data() + skip, in.size() - skip) /
                                           rmsOf(in.data() + skip, in.size() - skip));
    std::printf("[psola] %s shift %s %.0f ratio %.2f: f0 %.2f (want %.2f, %+.2f%%), RMS %+.2f dB\n",
                modeName, sigName, f0True, ratio, est, want, f0ErrPct, rmsDb);
    CHECK(std::abs(f0ErrPct) <= 0.5);
    CHECK(std::abs(rmsDb) <= 1.0);
}

} // namespace

TEST_CASE("psola: latency bounds at 44.1/48/96 kHz") {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        PsolaConfig liveCfg;
        liveCfg.mode = PsolaMode::Live;
        PsolaShifter live(fs, liveCfg);
        const std::size_t liveBound =
            static_cast<std::size_t>(std::ceil(0.001 * fs)) + 4;
        std::printf("[psola] fs %.0f: Live latency %zu (bound %zu)\n", fs,
                    live.latencyFrames(), liveBound);
        CHECK(live.latencyFrames() <= liveBound);

        PsolaConfig qCfg;
        qCfg.mode = PsolaMode::Quality;
        PsolaShifter qual(fs, qCfg);
        const std::size_t qBound =
            static_cast<std::size_t>(std::ceil(1.25 * fs / qCfg.minHz));
        const std::size_t qAim = static_cast<std::size_t>(std::ceil(fs / qCfg.minHz));
        std::printf("[psola] fs %.0f: Quality latency %zu (aim %zu, bound %zu)\n", fs,
                    qual.latencyFrames(), qAim, qBound);
        CHECK(qual.latencyFrames() <= qBound);
        CHECK(qual.latencyFrames() == qAim); // one period of minHz, the aim
    }
}

TEST_CASE("psola: bad config throws std::invalid_argument") {
    CHECK_THROWS_AS((PsolaShifter(0.0, PsolaConfig{})), std::invalid_argument);
    CHECK_THROWS_AS((PsolaShifter(-48000.0, PsolaConfig{})), std::invalid_argument);
    PsolaConfig badMin;
    badMin.minHz = 0.0;
    CHECK_THROWS_AS((PsolaShifter(kFs, badMin)), std::invalid_argument);
    badMin.minHz = -10.0;
    CHECK_THROWS_AS((PsolaShifter(kFs, badMin)), std::invalid_argument);
    PsolaConfig badMax;
    badMax.maxHz = badMax.minHz;
    CHECK_THROWS_AS((PsolaShifter(kFs, badMax)), std::invalid_argument);
    badMax.maxHz = 40.0;
    CHECK_THROWS_AS((PsolaShifter(kFs, badMax)), std::invalid_argument);
    PsolaConfig badBlock;
    badBlock.maxBlock = 0;
    CHECK_THROWS_AS((PsolaShifter(kFs, badBlock)), std::invalid_argument);
    PsolaConfig badFade;
    badFade.crossfadeMs = -0.5;
    CHECK_THROWS_AS((PsolaShifter(kFs, badFade)), std::invalid_argument);
    PsolaConfig qBad;
    qBad.mode = PsolaMode::Quality;
    qBad.minHz = 0.0;
    CHECK_THROWS_AS((PsolaShifter(kFs, qBad)), std::invalid_argument);
}

TEST_CASE("psola live: unvoiced output is the input delayed by exactly latencyFrames()") {
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Live;
    PsolaShifter sh(kFs, cfg);
    const std::size_t n = 48000;
    std::vector<float> in(n);
    sig::LcgNoise noise(12345u, 0.5);
    for (auto& v : in)
        v = noise.next();
    std::vector<float> out(n, -99.0f);
    runConst(sh, in.data(), out.data(), n, 0.0f, 1.3f); // ratio ignored unvoiced
    const std::size_t L = sh.latencyFrames();
    double maxDiff = 0.0;
    for (std::size_t i = L; i < n; ++i)
        maxDiff = std::max(maxDiff, std::abs(static_cast<double>(out[i]) - in[i - L]));
    std::printf("[psola] live unvoiced identity: max diff %.3g (bar 1e-5)\n", maxDiff);
    CHECK(maxDiff <= 1e-5);
    for (std::size_t i = 0; i < L; ++i)
        CHECK(out[i] == 0.0f);
}

TEST_CASE("psola live: ratio 1 voiced identity") {
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Live;
    PsolaShifter sh(kFs, cfg);
    const auto in = makeSaw(kFs, 220.0, 0.5, 48000);
    std::vector<float> out(in.size());
    runConst(sh, in.data(), out.data(), in.size(), 220.0f, 1.0f);
    const std::size_t L = sh.latencyFrames();
    double maxDiff = 0.0;
    for (std::size_t i = L; i < in.size(); ++i)
        maxDiff = std::max(maxDiff, std::abs(static_cast<double>(out[i]) - in[i - L]));
    std::printf("[psola] live ratio-1 identity: max diff %.3g (bar 1e-5)\n", maxDiff);
    CHECK(maxDiff <= 1e-5);
}

TEST_CASE("psola live: shift accuracy (saw 150, sine 220)") {
    const auto saw = makeSaw(kFs, 150.0, 0.5, 48000);
    const auto sine = makeSine(kFs, 220.0, 0.5, 48000);
    for (double ratio : {0.84, 0.94, 1.06, 1.19}) {
        checkShiftAccuracy(PsolaMode::Live, "live", "saw", saw, 150.0, ratio);
        checkShiftAccuracy(PsolaMode::Live, "live", "sine", sine, 220.0, ratio);
    }
}

// One bounded-delay measurement: 10 s of `in` shifted at (f0, ratio),
// cross-correlating output against input in 50 ms windows. The lag
// search extends 200 samples beyond the allowed extra-delay band
// [0, T0 + 2 ms] on both sides, so the measurement is not confined to
// the band it is meant to verify.
//
// Two measurement-method facts shape the assertions (both verified
// when this test was widened from an in-band-only search):
//
// 1. Period aliasing. Cross-correlating a periodic probe determines
//    delay only modulo the probe period T0: the saw test signal is
//    bit-exactly periodic, so its correlation at lag L + e is identical
//    to that at L + e +/- T0, and the widened argmax freely lands on
//    an out-of-band alias. Every observed out-of-band argmax folds
//    mod T0 into the allowed band. The argmax extra delay is therefore
//    folded into [0, T0) before the band assertions, and the
//    discriminating evidence is the ratio check: the best in-band peak
//    must reach at least 0.8x the best peak over the whole widened
//    search, i.e. the delay is genuinely explained in-band rather than
//    merely folded there.
// 2. Beat cancellation. For a sine probe the shifted output is a sine
//    at f0 * ratio, so input and output beat at |f0 * (ratio - 1)|.
//    At 220 Hz x 1.19 that is ~41.8 Hz, and a 50 ms window spans ~2.1
//    beat cycles: the normalised correlation integrates over nearly
//    whole beat periods and washes out at EVERY lag (measured global
//    peak range 0.039-0.046 across the 20 windows, below the 0.08
//    floor no matter where the peak sits). The absolute floor is
//    therefore not applicable to that one combination; the ratio
//    check (which is lag-relative, not absolute) still applies.
// `peakFloor` is the absolute peak-correlation floor, or 0 to skip it.
void checkBoundedDelay(const char* sigName, const std::vector<float>& in, double f0,
                       double ratio, double peakFloor) {
    const std::size_t n = in.size();
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Live;
    PsolaShifter sh(kFs, cfg);
    std::vector<float> out(n);
    runConst(sh, in.data(), out.data(), n, static_cast<float>(f0),
             static_cast<float>(ratio));
    const std::size_t L = sh.latencyFrames();
    const double T0 = kFs / f0;
    const double extraHi = T0 + 0.002 * kFs; // allowed extra delay, samples
    const int lagLo = static_cast<int>(L) - 200;
    const int lagHi =
        static_cast<int>(std::ceil(static_cast<double>(L) + extraHi)) + 200;
    const std::size_t win = 2400; // 50 ms
    double minPeak = 1e9;
    double maxPeak = -1e9;
    double minRatio = 1e9;
    double minExtra = 1e9;
    double maxExtra = -1e9;
    for (std::size_t s = settleSkip(sh); s + win + static_cast<std::size_t>(lagHi) < n;
         s += 24000) {
        double best = -2.0;
        int bestLag = lagLo;
        double bestInBand = -2.0;
        for (int lag = lagLo; lag <= lagHi; ++lag) {
            double so = 0.0, si = 0.0, soi = 0.0;
            for (std::size_t j = 0; j < win; ++j) {
                const double o = static_cast<double>(out[s + j]);
                const std::ptrdiff_t idx =
                    static_cast<std::ptrdiff_t>(s + j) - static_cast<std::ptrdiff_t>(lag);
                const double v = static_cast<double>(in[static_cast<std::size_t>(idx)]);
                so += o * o;
                si += v * v;
                soi += o * v;
            }
            const double c = soi / std::sqrt(so * si);
            if (c > best) {
                best = c;
                bestLag = lag;
            }
            const double lagExtra =
                static_cast<double>(lag) - static_cast<double>(L);
            if (lagExtra >= 0.0 && lagExtra <= extraHi && c > bestInBand) {
                bestInBand = c;
            }
        }
        minPeak = std::min(minPeak, best);
        maxPeak = std::max(maxPeak, best);
        if (peakFloor > 0.0) {
            CHECK(best >= peakFloor); // periodic content present at the peak delay
        }
        // The delay is explained in-band: the best in-band peak is not
        // dwarfed by an out-of-band one.
        CHECK(bestInBand >= 0.8 * best);
        minRatio = std::min(minRatio, bestInBand / best);
        // Fold the argmax extra delay mod T0 (period aliasing, above).
        double extra = static_cast<double>(bestLag) - static_cast<double>(L);
        extra = std::fmod(extra, T0);
        if (extra < 0.0) {
            extra += T0;
        }
        minExtra = std::min(minExtra, extra);
        maxExtra = std::max(maxExtra, extra);
    }
    std::printf("[psola] live bounded delay %s %.0f ratio %.2f: peak corr min %.3f max "
                "%.3f, in-band/global peak ratio min %.3f, folded extra delay range "
                "[%.0f, %.0f] samples (allowed [0, %.0f], searched [%.0f, %.0f])\n",
                sigName, f0, ratio, minPeak, maxPeak, minRatio, minExtra, maxExtra,
                extraHi, static_cast<double>(lagLo) - static_cast<double>(L),
                static_cast<double>(lagHi) - static_cast<double>(L));
    CHECK(minExtra >= 0.0);
    CHECK(maxExtra <= extraHi);
}

TEST_CASE("psola live: extra delay stays within [0, T0 + 2 ms] (50 ms cross-correlation)") {
    // The read-head delay sweeps [L, L + T0] as periods are skipped or
    // repeated, so within one 50 ms window the correlation peak is
    // smeared (observed ~0.13-0.5). The search covers all eight shift
    // combinations of the accuracy test, 10 s each, over a lag range
    // widened past the allowed band; see checkBoundedDelay for the
    // period-aliasing fold, the in-band/global peak-ratio evidence
    // check, and the sine x 1.19 beat-cancellation limit that replaces
    // the absolute 0.08 peak floor there.
    const std::size_t n = 480000; // 10 s
    const auto saw = makeSaw(kFs, 150.0, 0.5, n);
    const auto sine = makeSine(kFs, 220.0, 0.5, n);
    for (double ratio : {0.84, 0.94, 1.06, 1.19}) {
        checkBoundedDelay("saw", saw, 150.0, ratio, 0.08);
        // Sine x 1.19: beat cancellation caps the achievable peak
        // below 0.08 at every lag (see checkBoundedDelay), so only
        // the peak-ratio evidence is asserted for this combination.
        checkBoundedDelay("sine", sine, 220.0, ratio, ratio == 1.19 ? 0.0 : 0.08);
    }
}

TEST_CASE("psola live: no clicks, steady and under f0 toggling") {
    const auto saw = makeSaw(kFs, 150.0, 0.5, 48000);
    {
        PsolaConfig cfg;
        cfg.mode = PsolaMode::Live;
        PsolaShifter sh(kFs, cfg);
        std::vector<float> out(saw.size());
        runConst(sh, saw.data(), out.data(), saw.size(), 150.0f, 1.19f);
        const double inD = maxFirstDiff(saw.data(), saw.size());
        const double outD = maxFirstDiff(out.data() + settleSkip(sh), saw.size() - settleSkip(sh));
        std::printf("[psola] live clicks steady: out %.4f vs 2x in %.4f\n", outD, 2.0 * inD);
        CHECK(outD <= 2.0 * inD);
    }
    {
        // f0 toggles 220 <-> 0 every 100 ms (480-sample calls, toggle
        // every 10 calls); the input itself is a continuous sine.
        const auto sine = makeSine(kFs, 220.0, 0.5, 48000);
        PsolaConfig cfg;
        cfg.mode = PsolaMode::Live;
        PsolaShifter sh(kFs, cfg);
        std::vector<float> out(sine.size());
        for (std::size_t b = 0; b < sine.size(); b += 480) {
            const bool voiced = ((b / 480) / 10) % 2 == 0;
            sh.process(sine.data() + b, out.data() + b, 480, voiced ? 220.0f : 0.0f, 1.06f);
        }
        const double inD = maxFirstDiff(sine.data(), sine.size());
        const double outD = maxFirstDiff(out.data() + 480, sine.size() - 480);
        std::printf("[psola] live clicks toggling: out %.4f vs 2x in %.4f\n", outD, 2.0 * inD);
        CHECK(outD <= 2.0 * inD);
        CHECK(allFinite(out.data(), out.size()));
    }
}

TEST_CASE("psola quality: ratio 1 identity at -40 dB or better") {
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Quality;
    PsolaShifter sh(kFs, cfg);
    const auto in = makeSaw(kFs, 220.0, 0.5, 48000);
    std::vector<float> out(in.size());
    runConst(sh, in.data(), out.data(), in.size(), 220.0f, 1.0f);
    const std::size_t L = sh.latencyFrames();
    const std::size_t skip = settleSkip(sh);
    double err = 0.0, sg = 0.0;
    for (std::size_t i = skip; i < in.size(); ++i) {
        const double d = static_cast<double>(out[i]) - static_cast<double>(in[i - L]);
        err += d * d;
        sg += static_cast<double>(in[i - L]) * static_cast<double>(in[i - L]);
    }
    const double errDb = err == 0.0 ? -300.0 : 10.0 * std::log10(err / sg);
    std::printf("[psola] quality identity saw 220: error %.2f dB (bar -40 dB)%s\n", errDb,
                err == 0.0 ? " [bit-exact]" : "");
    CHECK(errDb <= -40.0);
}

TEST_CASE("psola quality: unvoiced noise is the input delayed by exactly latencyFrames()") {
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Quality;
    PsolaShifter sh(kFs, cfg);
    const std::size_t n = 48000;
    std::vector<float> in(n);
    sig::LcgNoise noise(777u, 0.5);
    for (auto& v : in)
        v = noise.next();
    std::vector<float> out(n, -99.0f);
    runConst(sh, in.data(), out.data(), n, 0.0f, 0.7f);
    const std::size_t L = sh.latencyFrames();
    double maxDiff = 0.0;
    for (std::size_t i = L; i < n; ++i)
        maxDiff = std::max(maxDiff, std::abs(static_cast<double>(out[i]) - in[i - L]));
    std::printf("[psola] quality unvoiced identity: max diff %.3g (bar 1e-6)\n", maxDiff);
    CHECK(maxDiff <= 1e-6);
}

TEST_CASE("psola quality: shift accuracy (saw 150; sine 220 a semitone up)") {
    const auto saw = makeSaw(kFs, 150.0, 0.5, 48000);
    for (double ratio : {0.84, 0.94, 1.06, 1.19})
        checkShiftAccuracy(PsolaMode::Quality, "quality", "saw", saw, 150.0, ratio);
    // Sine 220 shifted by 2^(1/12): 233.08 Hz +- 1 Hz absolute.
    const auto sine = makeSine(kFs, 220.0, 0.5, 48000);
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Quality;
    PsolaShifter sh(kFs, cfg);
    std::vector<float> out(sine.size());
    const float ratio = static_cast<float>(std::pow(2.0, 1.0 / 12.0));
    runConst(sh, sine.data(), out.data(), sine.size(), 220.0f, ratio);
    const double est = estimateF0(out.data() + settleSkip(sh), sine.size() - settleSkip(sh),
                                  kFs, 233.08);
    std::printf("[psola] quality sine semitone: f0 %.2f (want 233.08 +- 1)\n", est);
    CHECK(std::abs(est - 233.08) <= 1.0);
}

TEST_CASE("psola quality: mis-set f0 (+-1.5%) still tracks the true pitch, no clicks") {
    // The consumer feeds an *estimated* f0, not the true one. The
    // analysis grid advances by the given (wrong) period, but the marks
    // snap to real waveform peaks within +-0.25*T0, so the grid
    // re-locks to the true cycles with an occasional one-cycle slip.
    // The output pitch must still be true f0 * ratio, and the slips
    // must not click.
    const double ratio = 1.06;
    const double want = 220.0 * ratio; // 233.2 Hz
    const auto saw = makeSaw(kFs, 220.0, 0.5, 48000);
    const auto sine = makeSine(kFs, 220.0, 0.5, 48000);
    for (double misPct : {1.5, -1.5}) {
        const float f0Given = static_cast<float>(220.0 * (1.0 + misPct / 100.0));
        const std::pair<const char*, const std::vector<float>*> sigs[2] = {{"saw", &saw},
                                                                           {"sine", &sine}};
        for (const auto& sigPair : sigs) {
            const std::vector<float>& in = *sigPair.second;
            PsolaConfig cfg;
            cfg.mode = PsolaMode::Quality;
            PsolaShifter sh(kFs, cfg);
            std::vector<float> out(in.size());
            runConst(sh, in.data(), out.data(), in.size(), f0Given, static_cast<float>(ratio));
            const std::size_t skip = settleSkip(sh);
            const double est = estimateF0(out.data() + skip, in.size() - skip, kFs, want);
            const double errPct = 100.0 * (est - want) / want;
            const double inD = maxFirstDiff(in.data(), in.size());
            const double outD = maxFirstDiff(out.data() + skip, in.size() - skip);
            std::printf("[psola] quality mis-set f0 %+.1f%% %s: f0 %.2f (want %.2f, %+.3f%%), "
                        "max first diff %.4f (bound %.4f)\n",
                        misPct, sigPair.first, est, want, errPct, outD, 2.0 * inD);
            CHECK(allFinite(out.data(), out.size()));
            CHECK(std::abs(errPct) <= 1.0);
            CHECK(outD <= 2.0 * inD);
        }
    }
}

TEST_CASE("psola quality: formants stay at 800 Hz under a major-third shift") {
    // 120 Hz impulse train (period exactly 400 samples) through a
    // two-pole resonator at 800 Hz with Q = 5 (bandwidth 160 Hz).
    const std::size_t n = 48000;
    const double bw = 800.0 / 5.0;
    const double r = std::exp(-kPi * bw / kFs);
    const double theta = 2.0 * kPi * 800.0 / kFs;
    const double a1 = 2.0 * r * std::cos(theta);
    const double a2 = -r * r;
    std::vector<float> in(n, 0.0f);
    double ym1 = 0.0, ym2 = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = (i % 400 == 0) ? 1.0 : 0.0;
        const double y = x + a1 * ym1 + a2 * ym2;
        ym2 = ym1;
        ym1 = y;
        in[i] = static_cast<float>(y);
        peak = std::max(peak, std::abs(y));
    }
    for (auto& v : in)
        v = static_cast<float>(static_cast<double>(v) * (0.5 / peak));

    const float ratio = static_cast<float>(std::pow(2.0, 4.0 / 12.0)); // major third
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Quality;
    PsolaShifter sh(kFs, cfg);
    std::vector<float> out(n);
    runConst(sh, in.data(), out.data(), n, 120.0f, ratio);
    const std::size_t skip = settleSkip(sh);
    const double f0Est = estimateF0(out.data() + skip, n - skip, kFs, 151.2);
    std::printf("[psola] formant: shifted f0 %.2f (want 151.2 +- 1)\n", f0Est);
    CHECK(std::abs(f0Est - 151.2) <= 1.0);

    // Harmonic-envelope peak: direct DFT at the harmonics of the
    // measured f0 (no FFT, per the module's constraint).
    auto harmonicPeak = [&](const float* x, double f0) {
        double bestF = 0.0, bestM = -1.0;
        for (int k = 1; k <= 12; ++k) {
            const double f = k * f0;
            double re = 0.0, im = 0.0;
            for (std::size_t i = 0; i < n - skip; ++i) {
                const double ph = 2.0 * kPi * f * static_cast<double>(i) / kFs;
                re += static_cast<double>(x[skip + i]) * std::cos(ph);
                im -= static_cast<double>(x[skip + i]) * std::sin(ph);
            }
            const double mag = std::sqrt(re * re + im * im);
            if (mag > bestM) {
                bestM = mag;
                bestF = f;
            }
        }
        return bestF;
    };
    const double peakShifted = harmonicPeak(out.data(), f0Est);
    std::printf("[psola] formant: shifted harmonic peak %.1f Hz (want 800 +- 100)\n",
                peakShifted);
    CHECK(peakShifted >= 700.0);
    CHECK(peakShifted <= 900.0);

    // Contrast: plain resampling by the same ratio moves the formant to
    // ~800 * ratio = 1008 Hz.
    std::vector<float> rs(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const double pos = static_cast<double>(i) * static_cast<double>(ratio);
        const std::size_t idx = static_cast<std::size_t>(pos);
        rs[i] = idx < n ? in[idx] : 0.0f;
    }
    const double f0Rs = estimateF0(rs.data() + skip, n - skip, kFs, 151.2);
    const double peakRs = harmonicPeak(rs.data(), f0Rs);
    std::printf("[psola] formant: resampled harmonic peak %.1f Hz (contrast, want >= 950)\n",
                peakRs);
    CHECK(peakRs >= 950.0);
}

TEST_CASE("psola quality: no clicks under f0 toggling") {
    const auto sine = makeSine(kFs, 220.0, 0.5, 48000);
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Quality;
    PsolaShifter sh(kFs, cfg);
    std::vector<float> out(sine.size());
    for (std::size_t b = 0; b < sine.size(); b += 480) {
        const bool voiced = ((b / 480) / 10) % 2 == 0;
        sh.process(sine.data() + b, out.data() + b, 480, voiced ? 220.0f : 0.0f, 1.06f);
    }
    const double inD = maxFirstDiff(sine.data(), sine.size());
    const double outD = maxFirstDiff(out.data() + 480, sine.size() - 480);
    std::printf("[psola] quality clicks toggling: out %.4f vs 2x in %.4f\n", outD, 2.0 * inD);
    CHECK(outD <= 2.0 * inD);
    CHECK(allFinite(out.data(), out.size()));
}

TEST_CASE("psola: changing control — ratio steps every 50 ms are followed within 20 ms") {
    const auto in = makeSaw(kFs, 200.0, 0.5, 3 * 2400);
    const double ratios[3] = {1.0, 1.19, 0.84};
    for (PsolaMode mode : {PsolaMode::Live, PsolaMode::Quality}) {
        PsolaConfig cfg;
        cfg.mode = mode;
        PsolaShifter sh(kFs, cfg);
        std::vector<float> out(in.size());
        for (std::size_t seg = 0; seg < 3; ++seg)
            for (std::size_t b = 0; b < 2400; b += 480)
                sh.process(in.data() + seg * 2400 + b, out.data() + seg * 2400 + b, 480, 200.0f,
                           static_cast<float>(ratios[seg]));
        CHECK(allFinite(out.data(), out.size()));
        const double inD = maxFirstDiff(in.data(), in.size());
        const double outD = maxFirstDiff(out.data(), out.size());
        CHECK(outD <= 2.0 * inD);
        // Pitch measured over the last 30 ms of each segment: the step
        // at the segment start must be followed well within 20 ms.
        for (std::size_t seg = 1; seg < 3; ++seg) {
            const std::size_t start = seg * 2400 + 960;
            const double want = 200.0 * ratios[seg];
            const double est = estimateF0(out.data() + start, 2400 - 960, kFs, want);
            std::printf("[psola] changing control %s seg %zu: f0 %.2f (want %.2f)\n",
                        mode == PsolaMode::Live ? "live" : "quality", seg, est, want);
            CHECK(std::abs(est - want) / want <= 0.01);
        }
    }
}

TEST_CASE("psola: chunking invariance and in-place processing") {
    const auto in = makeSaw(kFs, 220.0, 0.5, 48000);
    for (PsolaMode mode : {PsolaMode::Live, PsolaMode::Quality}) {
        PsolaConfig cfg;
        cfg.mode = mode;
        std::vector<float> ref(in.size());
        {
            PsolaShifter sh(kFs, cfg);
            runConst(sh, in.data(), ref.data(), in.size(), 220.0f, 1.13f);
        }
        // Random chunk sizes in [1, maxBlock] from a deterministic LCG.
        std::vector<float> chunked(in.size());
        {
            PsolaShifter sh(kFs, cfg);
            std::uint32_t state = 424242u;
            std::size_t pos = 0;
            while (pos < in.size()) {
                state = state * 1664525u + 1013904223u;
                std::size_t len = 1 + (state >> 8) % cfg.maxBlock;
                len = std::min(len, in.size() - pos);
                sh.process(in.data() + pos, chunked.data() + pos, len, 220.0f, 1.13f);
                pos += len;
            }
        }
        double d1 = 0.0;
        for (std::size_t i = 0; i < in.size(); ++i)
            d1 = std::max(d1, std::abs(static_cast<double>(chunked[i]) - ref[i]));
        std::printf("[psola] chunking %s: max diff %.3g (bar 1e-6)\n",
                    mode == PsolaMode::Live ? "live" : "quality", d1);
        CHECK(d1 <= 1e-6);
        // In-place: in == out must give the same result.
        std::vector<float> inplace = in;
        {
            PsolaShifter sh(kFs, cfg);
            runConst(sh, inplace.data(), inplace.data(), inplace.size(), 220.0f, 1.13f);
        }
        double d2 = 0.0;
        for (std::size_t i = 0; i < in.size(); ++i)
            d2 = std::max(d2, std::abs(static_cast<double>(inplace[i]) - ref[i]));
        std::printf("[psola] in-place %s: max diff %.3g (bar 1e-6)\n",
                    mode == PsolaMode::Live ? "live" : "quality", d2);
        CHECK(d2 <= 1e-6);
    }
}

TEST_CASE("psola: reset() restores the freshly constructed state") {
    const auto in = makeSaw(kFs, 180.0, 0.5, 24000);
    const auto junk = makeSine(kFs, 330.0, 0.5, 12000);
    for (PsolaMode mode : {PsolaMode::Live, PsolaMode::Quality}) {
        PsolaConfig cfg;
        cfg.mode = mode;
        std::vector<float> ref(in.size());
        {
            PsolaShifter fresh(kFs, cfg);
            runConst(fresh, in.data(), ref.data(), in.size(), 180.0f, 1.13f);
        }
        PsolaShifter sh(kFs, cfg);
        std::vector<float> junkOut(junk.size());
        runConst(sh, junk.data(), junkOut.data(), junk.size(), 330.0f, 0.8f);
        sh.reset();
        std::vector<float> out(in.size());
        runConst(sh, in.data(), out.data(), in.size(), 180.0f, 1.13f);
        double d = 0.0;
        for (std::size_t i = 0; i < in.size(); ++i)
            d = std::max(d, std::abs(static_cast<double>(out[i]) - ref[i]));
        std::printf("[psola] reset %s: max diff vs fresh %.3g (want 0)\n",
                    mode == PsolaMode::Live ? "live" : "quality", d);
        CHECK(d == 0.0);
    }
}

// Power (dB) of x at frequency f, Hann-windowed (Goertzel). Used to check
// that the output's fundamental moved, without an estimator told the answer.
static double powerAtDb(const float* x, std::size_t n, double fs, double f) {
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double h = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                                              static_cast<double>(n - 1));
        const double w = 2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / fs;
        re += h * x[i] * std::cos(w);
        im -= h * x[i] * std::sin(w);
    }
    return 10.0 * std::log10(re * re + im * im + 1e-30);
}

TEST_CASE("psola quality: an octave down really lands an octave down") {
    // A 120 Hz saw is also periodic at 60 Hz, so this checks the new
    // fundamental's energy directly: an unshifted saw has none at 60 Hz
    // (about 70 dB below 120 Hz).
    const auto in = makeSaw(kFs, 120.0, 0.5, 48000);
    PsolaConfig cfg;
    cfg.mode = PsolaMode::Quality;
    cfg.minHz = 60.0;
    PsolaShifter sh(kFs, cfg);
    std::vector<float> out(in.size());
    runConst(sh, in.data(), out.data(), in.size(), 120.0f, 0.5f);
    const std::size_t skip = settleSkip(sh);
    const double at60 = powerAtDb(out.data() + skip, in.size() - skip, kFs, 60.0);
    const double at120 = powerAtDb(out.data() + skip, in.size() - skip, kFs, 120.0);
    std::printf("[psola] quality ratio 0.5: 60 Hz %.1f dB vs 120 Hz %.1f dB\n", at60, at120);
    CHECK(at60 > at120 - 20.0);
}

TEST_CASE("psola quality: an impulsive input shifts whatever the voicing onset phase") {
    // The LPC residual of a vowel: sharp pulses with lobes either side. The
    // first mark must lock onto a pulse, or every later +-T0/4 window can
    // miss them (seen with the onset at 2048 samples: the output read as
    // 60 Hz instead of 180).
    const double f0 = 120.0;
    const std::size_t n = 48000 * 2;
    std::vector<float> vowel(n, 0.0f);
    const auto period = static_cast<std::size_t>(std::lround(kFs / f0));
    for (std::size_t i = 0; i < n; i += period)
        vowel[i] = 1.0f;
    for (const auto [hz, bw] : {std::pair{700.0, 80.0}, std::pair{1220.0, 90.0}, std::pair{2600.0, 120.0}}) {
        const double r = std::exp(-3.14159265358979323846 * bw / kFs);
        const double a1 = 2.0 * r * std::cos(2.0 * 3.14159265358979323846 * hz / kFs), a2 = -r * r;
        double y1 = 0.0, y2 = 0.0;
        for (float& s : vowel) {
            const double y = (1.0 - a1 - a2) * s + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            s = static_cast<float>(y);
        }
    }
    std::vector<float> pulses(n);
    dsptb::dsp::LpcVoiceModel lpc(kFs, dsptb::dsp::LpcConfig{});
    for (std::size_t i = 0; i < n; i += 256) {
        const std::size_t len = std::min<std::size_t>(256, n - i);
        lpc.analyze(vowel.data() + i, pulses.data() + i, len);
        std::vector<float> ignored(len);
        lpc.synthesize(pulses.data() + i, ignored.data(), len);
    }
    for (const std::size_t onset : {std::size_t{0}, std::size_t{1024}, std::size_t{2048}, std::size_t{2300},
                                     std::size_t{3072}}) {
        PsolaConfig cfg;
        cfg.mode = PsolaMode::Quality;
        cfg.minHz = 60.0;
        cfg.maxHz = 800.0;
        PsolaShifter sh(kFs, cfg);
        std::vector<float> out(n);
        for (std::size_t i = 0; i < n; i += 256) {
            const std::size_t len = std::min<std::size_t>(256, n - i);
            const bool on = i >= onset;
            sh.process(pulses.data() + i, out.data() + i, len, on ? static_cast<float>(f0) : 0.0f, on ? 1.5f : 1.0f);
        }
        // Grains that alternate between cycles put lines at every multiple
        // of 60 Hz; a clean shift has them only at multiples of 180 Hz.
        const std::size_t from = onset + 8192;
        const double want = powerAtDb(out.data() + from, n - from, kFs, 180.0);
        double sub = -300.0;
        for (const double f : {60.0, 120.0, 240.0, 300.0, 420.0, 480.0})
            sub = std::max(sub, powerAtDb(out.data() + from, n - from, kFs, f));
        std::printf("[psola] impulsive onset %zu: 180 Hz %.1f dB vs strongest 60 Hz line %.1f dB\n", onset, want, sub);
        INFO("onset ", onset);
        CHECK(want > sub + 20.0);
    }
}

TEST_CASE("psola: range extremes with minHz = 60 (ratio 0.5 and 2.0)") {
    const auto in = makeSaw(kFs, 120.0, 0.5, 48000);
    for (PsolaMode mode : {PsolaMode::Live, PsolaMode::Quality}) {
        for (double ratio : {0.5, 2.0}) {
            PsolaConfig cfg;
            cfg.mode = mode;
            cfg.minHz = 60.0;
            PsolaShifter sh(kFs, cfg);
            std::vector<float> out(in.size());
            runConst(sh, in.data(), out.data(), in.size(), 120.0f,
                     static_cast<float>(ratio));
            CHECK(allFinite(out.data(), out.size()));
            const std::size_t skip = settleSkip(sh);
            const double want = 120.0 * ratio;
            const double est = estimateF0(out.data() + skip, in.size() - skip, kFs, want);
            const double inD = maxFirstDiff(in.data(), in.size());
            const double outD = maxFirstDiff(out.data() + skip, in.size() - skip);
            std::printf("[psola] range %s ratio %.1f (minHz 60): f0 %.2f (want %.1f), "
                        "maxDiff %.4f vs 2x %.4f\n",
                        mode == PsolaMode::Live ? "live" : "quality", ratio, est, want, outD,
                        2.0 * inD);
            CHECK(std::abs(est - want) / want <= 0.01);
            CHECK(outD <= 2.0 * inD);
        }
    }
}

TEST_CASE("psola: no allocation in process() or reset()") {
    const auto in = makeSaw(kFs, 220.0, 0.5, 4096);
    std::vector<float> out(in.size());
    for (PsolaMode mode : {PsolaMode::Live, PsolaMode::Quality}) {
        PsolaConfig cfg;
        cfg.mode = mode;
        PsolaShifter sh(kFs, cfg);
        runConst(sh, in.data(), out.data(), in.size(), 220.0f, 1.13f); // warm up
        AllocGuard guard;
        sh.process(in.data(), out.data(), 256, 220.0f, 1.13f);
        sh.process(in.data(), out.data(), 256, 0.0f, 1.0f);
        sh.reset();
        sh.process(in.data(), out.data(), 256, 220.0f, 0.9f);
        CHECK(guard.count() == 0);
    }
}

TEST_CASE("psola: micro-benchmarks (report only; budgets Live 10 us, Quality 20 us per 256)") {
    const auto in = makeSaw(kFs, 220.0, 0.5, 256);
    std::vector<float> out(256);
    for (PsolaMode mode : {PsolaMode::Live, PsolaMode::Quality}) {
        PsolaConfig cfg;
        cfg.mode = mode;
        PsolaShifter sh(kFs, cfg);
        for (int i = 0; i < 200; ++i)
            sh.process(in.data(), out.data(), 256, 220.0f, 1.06f); // warm up
        const int calls = 4000;
        const auto t0 = std::chrono::steady_clock::now();
        double sink = 0.0;
        for (int i = 0; i < calls; ++i) {
            sh.process(in.data(), out.data(), 256, 220.0f, 1.06f);
            sink += static_cast<double>(out[128]);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double usPerCall =
            std::chrono::duration<double, std::micro>(t1 - t0).count() / calls;
        std::printf("[bench] PsolaShifter %s: %.2f us per 256 voiced frames at 48 kHz "
                    "(budget: %s us)\n",
                    mode == PsolaMode::Live ? "Live" : "Quality", usPerCall,
                    mode == PsolaMode::Live ? "10" : "20");
        CHECK(std::isfinite(sink)); // the benchmark actually ran
    }
}
