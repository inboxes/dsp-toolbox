// VoiceStats: formants, vocal-tract length (VTL), LTAS and HNR of a speaker.

#include <dsptb/dsp_common/voice_stats.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace dsptb::dsp {
namespace {

constexpr int kLpcOrder = 12;
constexpr std::size_t kEnvelopePoints = 2048;
constexpr double kFormantMinHz = 200.0;
constexpr double kFormantMaxHz = 5000.0;
constexpr double kMaxFormantWidthHz = 600.0;
// Taps of the windowed-sinc decimation filter per unit of decimation
// (taps = kFirTapsPerDecimation * D + 1). 16 keeps the transition narrow
// enough that the 4th formant (up to ~4.4 kHz for a 14 cm tract) sits in
// the passband while the polyphase cost stays inside the §6 budget; the
// prototype showed 16 and 32 taps/D equivalent on the criteria (flat worst
// 1.65-1.78% vs 1.80-1.93%, tilted worst 4.55% vs 4.35%).
constexpr int kFirTapsPerDecimation = 16;
// Pre-emphasis coefficient for the formant path (maintainer ruling):
// the usual coefficient is 0.97, but 0.97 over-corrects on
// the flat test vowel (F1 +8.6% at 48 kHz, outside the ±5% bar), while
// dropping pre-emphasis loses formants on steep voices (about -12 dB/oct
// net: 6 of 24 frames under 3 formants, VTL invalid). 0.7 balances the
// two: worst F1 error 1.65-1.79% across 44.1/48/96 kHz on the flat vowel
// (measured in the committed tests) and the tilted-source regression test
// passes at 4.55% worst. A prototype sweep of 0.68-0.75 found 0.72
// marginally better on our tilted model only (within model variation),
// so the ruled 0.7 is kept.
constexpr double kFormantPreEmphasis = 0.7;
constexpr double kPi = 3.14159265358979323846;

bool isPowerOfTwo(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

// Hann window, periodic (as dsptb::hannWindow): 0.5 - 0.5*cos(2*pi*i/n).
double hannPeriodic(std::size_t i, std::size_t n) noexcept {
    return 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
}

float medianOf(const float* values, std::size_t n, std::vector<float>& scratch) noexcept {
    if (n == 0) return 0.0f;
    std::copy(values, values + n, scratch.begin());
    const std::size_t mid = n / 2;
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid),
                      scratch.begin() + static_cast<std::ptrdiff_t>(n));
    const float hi = scratch[mid];
    if (n % 2 == 1) return hi;
    const float lo = *std::max_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5f * (lo + hi);
}

} // namespace

VoiceStats::VoiceStats(double sampleRate, const VoiceStatsConfig& config)
    : sampleRate_(sampleRate), maxFrames_(config.maxFrames), speedOfSoundCmS_(config.speedOfSoundCmS),
      minVtlCm_(config.minVtlCm), maxVtlCm_(config.maxVtlCm), fft_(config.frameSize == 0
                                                        ? (sampleRate <= 50000.0 ? 2048 : 4096)
                                                        : config.frameSize) {
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) {
        throw std::invalid_argument("VoiceStats: sampleRate must be positive and finite");
    }
    // LTAS floor: the 32-band table's top band starts at
    // sqrt(centres[30]*centres[31]) ~= 11.84 kHz. A band whose whole range
    // lies above Nyquist gets zero bin overlap (weightSum == 0), so its
    // mean bin power is 0/0 = NaN and summary() averages that NaN into
    // every ltasDb value. Reject sample rates whose Nyquist is below the
    // top band's lower edge instead of clamping bands to Nyquist (the spec
    // does not define LTAS behaviour there). The threshold is derived from
    // the band table, not hard-coded.
    float centres[32];
    ltasBandCentres(centres);
    if (sampleRate < 2.0 * std::sqrt(static_cast<double>(centres[30]) *
                                     static_cast<double>(centres[31]))) {
        throw std::invalid_argument(
            "VoiceStats: sampleRate too low: Nyquist below the top LTAS band's lower edge");
    }
    if (config.frameSize != 0 && (!isPowerOfTwo(config.frameSize) || config.frameSize < 512)) {
        throw std::invalid_argument("VoiceStats: frameSize must be a power of two >= 512");
    }
    if (config.maxFrames == 0) {
        throw std::invalid_argument("VoiceStats: maxFrames must be > 0");
    }
    if (!(config.minVtlCm < config.maxVtlCm)) {
        throw std::invalid_argument("VoiceStats: minVtlCm must be < maxVtlCm");
    }
    if (!(config.speedOfSoundCmS > 0.0) || !std::isfinite(config.speedOfSoundCmS)) {
        throw std::invalid_argument("VoiceStats: speedOfSoundCmS must be positive and finite");
    }
    frameSize_ = fft_.size();

    decimation_ = static_cast<std::size_t>(sampleRate / 11025.0);
    if (decimation_ < 1) decimation_ = 1;
    decimatedSize_ = frameSize_ / decimation_;
    if (decimatedSize_ <= static_cast<std::size_t>(kLpcOrder)) {
        throw std::invalid_argument("VoiceStats: frameSize/decimation leaves too few samples for LPC");
    }

    // Windowed-sinc decimation filter: cutoff 0.45*fs/D, Hann window,
    // normalised to unit DC gain.
    const std::size_t taps =
        static_cast<std::size_t>(kFirTapsPerDecimation) * decimation_ + 1;
    const double centre = static_cast<double>(taps - 1) / 2.0;
    const double fc = 0.45 / static_cast<double>(decimation_); // cycles/sample
    std::vector<double> firDouble(taps);
    double firSum = 0.0;
    for (std::size_t n = 0; n < taps; ++n) {
        const double m = static_cast<double>(n) - centre;
        const double x = 2.0 * fc * m;
        const double sinc = (x == 0.0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                                   static_cast<double>(taps - 1));
        firDouble[n] = 2.0 * fc * sinc * window;
        firSum += firDouble[n];
    }
    // Store the coefficients as float: the decimation FIR is the hot loop
    // and float arithmetic (4-wide SSE2) halves its cost. The coefficients
    // are designed in double and rounded once; the formant errors are
    // unchanged (worst 1.93% at 48 kHz either way).
    fir_.resize(taps);
    for (std::size_t n = 0; n < taps; ++n) {
        fir_[n] = static_cast<float>(firDouble[n] / firSum);
    }

    decimated_.resize(decimatedSize_);
    windowedDec_.resize(decimatedSize_);
    hannDec_.resize(decimatedSize_);
    for (std::size_t i = 0; i < decimatedSize_; ++i) hannDec_[i] = hannPeriodic(i, decimatedSize_);
    envelopeMag2_.resize(kEnvelopePoints);
    hannFull_.resize(frameSize_);
    for (std::size_t i = 0; i < frameSize_; ++i) {
        hannFull_[i] = static_cast<float>(hannPeriodic(i, frameSize_));
    }
    windowedFull_.resize(frameSize_);
    power_.resize(fft_.bins());

    // LTAS band table: centres 100*128^(i/31), edges at the geometric
    // midpoints, outer edges half a band beyond. For each band, the bins it
    // overlaps, with fractional weights on the edge bins (a bin covers
    // [k*df - df/2, k*df + df/2]); interior bins have weight 1. The band
    // value is the weighted mean bin power, so a band with no whole bin
    // still gets a value and white noise stays flat.
    const double df = sampleRate_ / static_cast<double>(frameSize_);
    const double ratio = std::pow(128.0, 1.0 / 31.0);
    const double rootRatio = std::sqrt(ratio);
    ltasBands_.resize(32);
    for (int b = 0; b < 32; ++b) {
        const double lo = (b == 0) ? static_cast<double>(centres[0]) / rootRatio
                                   : std::sqrt(static_cast<double>(centres[b - 1]) *
                                               static_cast<double>(centres[b]));
        const double hi = (b == 31) ? static_cast<double>(centres[31]) * rootRatio
                                    : std::sqrt(static_cast<double>(centres[b]) *
                                                static_cast<double>(centres[b + 1]));
        const std::size_t bins = fft_.bins();
        // The floor can be -1 (band 0's lower edge below df/2 at a small
        // frameSize), so take it signed and clamp before converting:
        // casting a negative double to size_t is undefined behaviour.
        const double firstD = std::max(std::floor((lo - df / 2.0) / df) + 1.0, 0.0);
        std::size_t first = static_cast<std::size_t>(firstD);
        std::size_t last = static_cast<std::size_t>(
            std::ceil((hi + df / 2.0) / df));
        if (last > 0) --last;
        if (first >= bins) first = bins - 1;
        if (last >= bins) last = bins - 1;
        const auto overlap = [&](std::size_t k) {
            const double blo = static_cast<double>(k) * df - df / 2.0;
            const double bhi = static_cast<double>(k) * df + df / 2.0;
            return std::max(0.0, std::min(bhi, hi) - std::max(blo, lo)) / df;
        };
        LtasBand& band = ltasBands_[static_cast<std::size_t>(b)];
        band.firstBin = first;
        band.lastBin = last;
        band.firstWeight = overlap(first);
        band.lastWeight = overlap(last);
        band.weightSum = (first == last)
                             ? band.firstWeight
                             : band.firstWeight + band.lastWeight +
                                   static_cast<double>(last - first - 1);
    }

    vtlValues_.reserve(maxFrames_);
    hnrValues_.reserve(maxFrames_);
    medianScratch_.resize(maxFrames_);
}

FormantFrame VoiceStats::measureFormants(const float* frame) noexcept {
    // Decimate (polyphase: only every D-th output is computed). The filter
    // window is in range for the interior outputs, so those run branchless
    // with four independent accumulators; the few edge outputs keep the
    // bounds check. The FIR runs in float (see the header note).
    const std::size_t taps = fir_.size();
    const std::size_t centre = (taps - 1) / 2;
    const float* fir = fir_.data();
    const std::size_t firstInterior =
        (centre + decimation_ - 1) / decimation_; // first m with base >= 0
    // Last m with base + taps <= frameSize, + 1 (signed: the numerator can
    // go negative for very high fs with a small explicit frameSize).
    const std::ptrdiff_t interiorEnd =
        (static_cast<std::ptrdiff_t>(frameSize_) + static_cast<std::ptrdiff_t>(centre) -
         static_cast<std::ptrdiff_t>(taps)) /
            static_cast<std::ptrdiff_t>(decimation_) +
        1;
    const std::size_t lastInterior =
        interiorEnd > 0 ? static_cast<std::size_t>(interiorEnd) : firstInterior;
    for (std::size_t m = 0; m < decimatedSize_; ++m) {
        const std::ptrdiff_t base = static_cast<std::ptrdiff_t>(m * decimation_) -
                                    static_cast<std::ptrdiff_t>(centre);
        float acc = 0.0f;
        if (m >= firstInterior && m < lastInterior) {
            const float* x = frame + base;
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            std::size_t k = 0;
            for (; k + 4 <= taps; k += 4) {
                s0 += fir[k] * x[k];
                s1 += fir[k + 1] * x[k + 1];
                s2 += fir[k + 2] * x[k + 2];
                s3 += fir[k + 3] * x[k + 3];
            }
            for (; k < taps; ++k) s0 += fir[k] * x[k];
            acc = (s0 + s1) + (s2 + s3);
        } else {
            for (std::size_t k = 0; k < taps; ++k) {
                const std::ptrdiff_t idx = base + static_cast<std::ptrdiff_t>(k);
                if (idx >= 0 && idx < static_cast<std::ptrdiff_t>(frameSize_)) {
                    acc += fir[k] * frame[idx];
                }
            }
        }
        decimated_[m] = static_cast<double>(acc);
    }
    // Pre-emphasise, then Hann-window.
    windowedDec_[0] = decimated_[0] * hannDec_[0];
    for (std::size_t n = 1; n < decimatedSize_; ++n) {
        windowedDec_[n] = (decimated_[n] - kFormantPreEmphasis * decimated_[n - 1]) * hannDec_[n];
    }

    // LPC order 12 by the autocorrelation method + Levinson-Durbin
    // (predictor convention: A(z) = 1 - sum a_k z^-k).
    double r[kLpcOrder + 1];
    const std::size_t n = decimatedSize_;
    for (int k = 0; k <= kLpcOrder; ++k) {
        double acc = 0.0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(k) < n; ++i) {
            acc += windowedDec_[i] * windowedDec_[i + static_cast<std::size_t>(k)];
        }
        r[k] = acc;
    }
    double a[kLpcOrder + 1] = {};
    if (r[0] > 0.0) {
        double e = r[0];
        for (int i = 1; i <= kLpcOrder; ++i) {
            double acc = 0.0;
            for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
            const double kRef = (r[i] - acc) / e;
            if (std::abs(kRef) >= 1.0) break; // unstable step: keep the fit so far
            double an[kLpcOrder + 1] = {};
            for (int j = 1; j < i; ++j) an[j] = a[j] - kRef * a[i - j];
            an[i] = kRef;
            for (int j = 1; j <= i; ++j) a[j] = an[j];
            e *= (1.0 - kRef * kRef);
            if (e <= 0.0) break;
        }
    }

    // Envelope |1/A(z)| on 2048 points over 0...fs/(2D), evaluated in the
    // linear domain as |A|^2 = R_0 + 2*sum_m R_m*cos(m*omega), where R_m is
    // the autocorrelation of the A(z) coefficients (A(z) = 1 - sum a_k z^-k,
    // so alpha_0 = 1, alpha_k = -a_k), with cos(m*omega) from the Chebyshev
    // recurrence. Two points are interleaved (independent chains) to keep
    // the serial recurrence off the critical path. No log is taken per
    // point; the peak interpolation below needs log values only at the few
    // peak candidates. This MUST stay in double: for a steep voice the
    // |A|^2 minima are extremely deep (dynamic range ~2e10) and a float
    // evaluation (e.g. an FFT of float coefficients) leaves rounding
    // ripples that the peak picker mistakes for formants.
    const double fsDec = sampleRate_ / static_cast<double>(decimation_);
    const double dOmega = kPi / static_cast<double>(kEnvelopePoints - 1);
    const double cosStep = std::cos(dOmega);
    const double sinStep = std::sin(dOmega);
    const double cos2Step = std::cos(2.0 * dOmega);
    const double sin2Step = std::sin(2.0 * dOmega);
    double alpha[kLpcOrder + 1];
    alpha[0] = 1.0;
    for (int k = 1; k <= kLpcOrder; ++k) alpha[k] = -a[k];
    double rCoeff[kLpcOrder + 1];
    for (int m = 0; m <= kLpcOrder; ++m) {
        double acc = 0.0;
        for (int k = 0; k + m <= kLpcOrder; ++k) acc += alpha[k] * alpha[k + m];
        rCoeff[m] = acc;
    }
    const double df = fsDec / 2.0 / static_cast<double>(kEnvelopePoints - 1);
    double cosA = 1.0; // cos(omega_j)
    double sinA = 0.0; // sin(omega_j)
    double cosB = cosStep; // cos(omega_{j+1})
    double sinB = sinStep; // sin(omega_{j+1})
    for (std::size_t j = 0; j < kEnvelopePoints; j += 2) {
        double cPrevA = 1.0; // cos(0)
        double cCurA = cosA; // cos(omega_j)
        double magA = rCoeff[0] + 2.0 * rCoeff[1] * cCurA;
        double cPrevB = 1.0;
        double cCurB = cosB; // cos(omega_{j+1})
        double magB = rCoeff[0] + 2.0 * rCoeff[1] * cCurB;
        for (int m = 2; m <= kLpcOrder; ++m) {
            const double nextA = 2.0 * cosA * cCurA - cPrevA;
            magA += 2.0 * rCoeff[m] * nextA;
            cPrevA = cCurA;
            cCurA = nextA;
            const double nextB = 2.0 * cosB * cCurB - cPrevB;
            magB += 2.0 * rCoeff[m] * nextB;
            cPrevB = cCurB;
            cCurB = nextB;
        }
        envelopeMag2_[j] = std::max(magA, 1e-20);
        envelopeMag2_[j + 1] = std::max(magB, 1e-20);
        const double nextCosA = cosA * cos2Step - sinA * sin2Step;
        const double nextSinA = sinA * cos2Step + cosA * sin2Step;
        cosA = nextCosA;
        sinA = nextSinA;
        const double nextCosB = cosB * cos2Step - sinB * sin2Step;
        const double nextSinB = sinB * cos2Step + cosB * sin2Step;
        cosB = nextCosB;
        sinB = nextSinB;
    }

    // Peaks between 200 and 5000 Hz, parabolic interpolation on dB (log
    // taken only at the three points around each candidate), first 4 whose
    // -3 dB width is below 600 Hz. The envelope is |1/A|, so its peaks are
    // the MINIMA of |A|^2 stored here: the local-extremum test, the width
    // target and the width walks are all in that inverted sense.
    FormantFrame out{};
    out.count = 0;
    out.vtlCm = 0.0f;
    out.valid = false;
    for (int i = 0; i < 4; ++i) out.hz[i] = 0.0f;
    for (std::size_t j = 1; j + 1 < kEnvelopePoints; ++j) {
        const double f = static_cast<double>(j) * df;
        if (f < kFormantMinHz || f > kFormantMaxHz) continue;
        const double peakMag2 = envelopeMag2_[j];
        if (!(peakMag2 < envelopeMag2_[j - 1] && peakMag2 <= envelopeMag2_[j + 1])) continue;
        const double yPrev = -10.0 * std::log10(envelopeMag2_[j - 1]);
        const double y0 = -10.0 * std::log10(peakMag2);
        const double yNext = -10.0 * std::log10(envelopeMag2_[j + 1]);
        const double denom = yPrev - 2.0 * y0 + yNext;
        double shift = 0.0;
        if (denom != 0.0) {
            shift = 0.5 * (yPrev - yNext) / denom;
            shift = std::clamp(shift, -1.0, 1.0);
        }
        const double fp = f + shift * df;
        // -3 dB width: the envelope is 3 dB below its peak where |A|^2
        // is 10^(3/10) times the peak's |A|^2. Walk out from the peak
        // (a minimum of |A|^2) until it rises to the target, interpolating
        // linearly.
        const double target = peakMag2 * 1.9952623149688795; // 10^(3/10)
        double fLeft = 0.0;
        bool leftOk = false;
        for (std::size_t m = j;; --m) {
            if (envelopeMag2_[m] >= target) {
                // Crossing between m and m+1 (envelopeMag2_[m+1] < target).
                const double vHi = envelopeMag2_[m];
                const double vLo = envelopeMag2_[m + 1];
                const double frac = (vHi - target) / (vHi - vLo);
                fLeft = static_cast<double>(m) * df + frac * df;
                leftOk = true;
                break;
            }
            if (m == 0) break;
        }
        if (!leftOk) continue;
        double fRight = 0.0;
        bool rightOk = false;
        for (std::size_t m = j; m + 1 < kEnvelopePoints; ++m) {
            if (envelopeMag2_[m + 1] >= target) {
                const double vLo = envelopeMag2_[m];
                const double vHi = envelopeMag2_[m + 1];
                const double frac = (target - vLo) / (vHi - vLo);
                fRight = static_cast<double>(m) * df + frac * df;
                rightOk = true;
                break;
            }
        }
        if (!rightOk) continue;
        if (fRight - fLeft >= kMaxFormantWidthHz) continue;
        out.hz[out.count] = static_cast<float>(fp);
        ++out.count;
        if (out.count == 4) break;
    }
    if (out.count >= 3) {
        out.vtlCm = vtlFromFormants(out.hz, out.count, speedOfSoundCmS_);
        out.valid = out.vtlCm >= minVtlCm_ && out.vtlCm <= maxVtlCm_;
    }
    return out;
}

void VoiceStats::accumulateLtas(const float* frame) noexcept {
    for (std::size_t i = 0; i < frameSize_; ++i) {
        windowedFull_[i] = frame[i] * hannFull_[i];
    }
    fft_.powerSpectrum(windowedFull_.data(), power_.data());
    for (int b = 0; b < 32; ++b) {
        const LtasBand& band = ltasBands_[static_cast<std::size_t>(b)];
        double acc = 0.0;
        if (band.firstBin == band.lastBin) {
            acc = band.firstWeight * static_cast<double>(power_[band.firstBin]);
        } else {
            acc = band.firstWeight * static_cast<double>(power_[band.firstBin]) +
                  band.lastWeight * static_cast<double>(power_[band.lastBin]);
            for (std::size_t k = band.firstBin + 1; k < band.lastBin; ++k) {
                acc += static_cast<double>(power_[k]);
            }
        }
        ltasSum_[b] += acc / band.weightSum;
    }
}

FormantFrame VoiceStats::pushFrame(const float* frame, float yinConfidence) noexcept {
    FormantFrame result = measureFormants(frame);
    accumulateLtas(frame);
    // A NaN confidence is unvoiced (-10 dB): std::clamp passes NaN
    // through, and a stored NaN would poison the HNR median.
    const double c = std::isnan(yinConfidence)
                         ? 0.01
                         : std::clamp(static_cast<double>(yinConfidence), 0.01, 0.9999);
    const double hnr = std::clamp(10.0 * std::log10(c / (1.0 - c)), -10.0, 40.0);
    if (result.valid) {
        ++vtlFrames_;
        if (vtlValues_.size() < maxFrames_) vtlValues_.push_back(result.vtlCm);
    }
    if (hnrValues_.size() < maxFrames_) hnrValues_.push_back(static_cast<float>(hnr));
    ++frames_;
    return result;
}

VoiceSummary VoiceStats::summary() const noexcept {
    VoiceSummary out{};
    out.vtlCmMedian = medianOf(vtlValues_.data(), vtlValues_.size(), medianScratch_);
    out.vtlFrames = vtlFrames_;
    out.hnrDbMedian = medianOf(hnrValues_.data(), hnrValues_.size(), medianScratch_);
    out.frames = frames_;
    if (frames_ > 0) {
        double db[32];
        double mean = 0.0;
        for (int b = 0; b < 32; ++b) {
            db[b] = 10.0 * std::log10(std::max(ltasSum_[static_cast<std::size_t>(b)], 1e-30));
            mean += db[b];
        }
        mean /= 32.0;
        for (int b = 0; b < 32; ++b) {
            out.ltasDb[b] = static_cast<float>(db[b] - mean);
        }
    } else {
        for (int b = 0; b < 32; ++b) out.ltasDb[b] = 0.0f;
    }
    return out;
}

void VoiceStats::reset() noexcept {
    frames_ = 0;
    vtlFrames_ = 0;
    vtlValues_.clear();
    hnrValues_.clear();
    for (double& s : ltasSum_) s = 0.0;
}

float VoiceStats::vtlFromFormants(const float* hz, int count, double speedOfSoundCmS) noexcept {
    if (count <= 0 || hz == nullptr || !std::isfinite(speedOfSoundCmS)) return 0.0f;
    double num = 0.0;
    double den = 0.0;
    for (int i = 1; i <= count; ++i) {
        const double f = static_cast<double>(hz[i - 1]);
        if (!std::isfinite(f) || !(f > 0.0)) return 0.0f;
        const double odd = static_cast<double>(2 * i - 1);
        num += odd * odd;
        den += odd * f;
    }
    if (den <= 0.0) return 0.0f;
    return static_cast<float>(speedOfSoundCmS * num / (4.0 * den));
}

void VoiceStats::ltasBandCentres(float out[32]) noexcept {
    for (int i = 0; i < 32; ++i) {
        out[i] = static_cast<float>(100.0 * std::pow(128.0, static_cast<double>(i) / 31.0));
    }
}

} // namespace dsptb::dsp
