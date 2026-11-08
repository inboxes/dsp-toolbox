// LpcVoiceModel implementation. See lpc.hpp for the conventions.

#include <dsptb/dsp_common/lpc.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace dsptb::dsp {
namespace {

// pi, matching the value the test support headers use.
constexpr double kPi = 3.14159265358979323846;
// The envelope is evaluated on a 2048-point FFT grid (1024 points over
// 0..fs/2).
constexpr std::size_t kWarpFftSize = 2 * kLpcEnvelopePoints;

bool isWarpIdentity(const WarpSettings& warp) noexcept {
    if (warp.formantFactor != 1.0f || warp.tiltDbPerOct != 0.0f)
        return false;
    for (const float eq : warp.eqDb) {
        if (eq != 0.0f)
            return false;
    }
    return true;
}

} // namespace

LpcVoiceModel::LpcVoiceModel(double sampleRate, const LpcConfig& config)
    : sampleRate_(sampleRate), config_(config), warpFft_(kWarpFftSize) {
    if (!std::isfinite(sampleRate_) || sampleRate_ <= 0.0)
        throw std::invalid_argument("LpcVoiceModel: sampleRate must be positive and finite");
    if (config_.hop == 0)
        throw std::invalid_argument("LpcVoiceModel: hop must be >= 1");
    if (config_.maxBlock == 0)
        throw std::invalid_argument("LpcVoiceModel: maxBlock must be >= 1");
    if (config_.synthDelay > 4 * config_.hop)
        throw std::invalid_argument("LpcVoiceModel: synthDelay must be <= 4*hop");
    if (!std::isfinite(config_.lagWindowHz) || config_.lagWindowHz < 0.0)
        throw std::invalid_argument("LpcVoiceModel: lagWindowHz must be finite and >= 0");
    if (!std::isfinite(config_.whiteNoiseDb))
        throw std::invalid_argument("LpcVoiceModel: whiteNoiseDb must be finite");
    if (!std::isfinite(config_.preEmphasis))
        throw std::invalid_argument("LpcVoiceModel: preEmphasis must be finite");

    // Auto order: min(104, round(fs/1000) + 4), about one pole pair per kHz.
    // A lower cap starves high rates: order 56 at 96 kHz fits formants as
    // loosely as order 28 at 48 kHz (F1 +8 % on the test vowel).
    if (config_.order == 0) {
        const long autoOrder = std::lround(sampleRate_ / 1000.0) + 4;
        order_ = static_cast<std::size_t>(std::min<long>(104, autoOrder));
    } else {
        order_ = config_.order;
    }
    if (order_ > kLpcMaxOrder)
        throw std::invalid_argument("LpcVoiceModel: order must be <= 128");
    // Auto window: 1024 at fs <= 50 kHz, else 2048.
    window_ = config_.window == 0 ? (sampleRate_ <= 50000.0 ? 1024 : 2048) : config_.window;
    if (window_ < 2 * order_)
        throw std::invalid_argument("LpcVoiceModel: window must be >= 2*order");

    mu_ = std::pow(config_.preEmphasis, 48000.0 / sampleRate_);
    for (std::size_t kk = 0; kk <= kLpcEnvelopePoints; ++kk) {
        const double w = kPi * static_cast<double>(kk) / static_cast<double>(kLpcEnvelopePoints);
        emph2_[kk] = 1.0 - 2.0 * mu_ * std::cos(w) + mu_ * mu_;
    }
    hann_.resize(window_);
    for (std::size_t n = 0; n < window_; ++n)
        hann_[n] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / static_cast<double>(window_ - 1));
    ringEmph_.assign(window_, 0.0);
    frameScratch_.assign(window_, 0.0);
    autocorr_.assign(order_ + 1, 0.0);
    // History ring: the sets from the hop boundaries spanning one full
    // analyze call (up to maxBlock) plus synthDelay, plus one for the
    // interpolation pair, so no set is evicted inside the call contract.
    const std::size_t historySets =
        std::max<std::size_t>(kLpcHistorySetsMin,
                              (config_.maxBlock + config_.synthDelay) / config_.hop + 2);
    history_.assign(historySets, HistoryEntry{});
    warpFftIn_.assign(kWarpFftSize, 0.0f);
    warpSpectrum_.assign(kWarpFftSize / 2 + 1, std::complex<float>{});
    warpInvSpectrum_.assign(kWarpFftSize / 2 + 1, std::complex<float>{});
    warpEnv_.assign(kLpcEnvelopePoints, 0.0);
    warpAutocorr_.assign(kWarpFftSize, 0.0f);
    updateWarpGain();
}

void LpcVoiceModel::reset() noexcept {
    std::fill(ringEmph_.begin(), ringEmph_.end(), 0.0);
    ringPos_ = 0;
    prevRaw_ = 0.0;
    sample_ = 0;
    phase_ = 0;
    hopSumSq_ = 0.0;
    hopCount_ = 0;
    kPrev_.fill(0.0);
    kCur_.fill(0.0);
    aCur_.fill(0.0);
    err_ = 1.0;
    kWarpPrev_.fill(0.0);
    kWarpCur_.fill(0.0);
    aWarpCur_.fill(0.0);
    errWarp_ = 1.0;
    gPrev_ = 1.0;
    gCur_ = 1.0;
    bFir_.fill(0.0);
    bIir_.fill(0.0);
    deemphState_ = 0.0;
    historyHead_ = 0;
    historyCount_ = 0;
    historyDropped_ = false;
    synthSample_ = 0;
}

void LpcVoiceModel::setWarp(const WarpSettings& warp) noexcept {
    warp_ = warp;
    updateWarpGain();
}

void LpcVoiceModel::updateWarpGain() noexcept {
    // Tilt + EQ gain per envelope-grid point, precomputed here (it depends
    // only on the warp settings and fs, not on the signal) so the per-hop
    // warp loop is a single multiply per point instead of a log2 + pow.
    float centres[10];
    eqBandCentres(centres);
    const double tilt = static_cast<double>(warp_.tiltDbPerOct);
    for (std::size_t kk = 0; kk <= kLpcEnvelopePoints; ++kk) {
        const double f = static_cast<double>(kk) * sampleRate_ /
                         static_cast<double>(kWarpFftSize);
        const double tGain = tilt * std::log2(std::max(f, 250.0) / 1000.0);
        double eq = 0.0;
        if (f <= static_cast<double>(centres[0])) {
            eq = static_cast<double>(warp_.eqDb[0]);
        } else if (f >= static_cast<double>(centres[9])) {
            eq = static_cast<double>(warp_.eqDb[9]);
        } else {
            for (std::size_t i = 0; i < 9; ++i) {
                if (f < static_cast<double>(centres[i + 1])) {
                    const double t = std::log(f / static_cast<double>(centres[i])) /
                                     std::log(static_cast<double>(centres[i + 1]) /
                                              static_cast<double>(centres[i]));
                    eq = static_cast<double>(warp_.eqDb[i]) * (1.0 - t) +
                         static_cast<double>(warp_.eqDb[i + 1]) * t;
                    break;
                }
            }
        }
        warpGain_[kk] = std::pow(10.0, (tGain + eq) / 10.0);
    }
}

void LpcVoiceModel::setSilenceDb(float db) noexcept {
    silenceDb_ = db;
}

void LpcVoiceModel::eqBandCentres(float out[10]) noexcept {
    for (std::size_t i = 0; i < 10; ++i)
        out[i] = static_cast<float>(100.0 * std::pow(128.0, static_cast<double>(i) / 9.0));
}

void LpcVoiceModel::analyze(const float* x, float* residual, std::size_t n) noexcept {
    if (n == 0)
        return;
    const std::size_t p = order_;
    const std::size_t hop = config_.hop;
    const double pre = mu_;
    for (std::size_t i = 0; i < n; ++i) {
        const double xn = static_cast<double>(x[i]);
        // The pre-emphasis filter is part of the signal path, so
        // it runs here with continuous state; the ring keeps the newest
        // `window` emphasised samples for the hop analysis.
        const double emph = xn - pre * prevRaw_;
        prevRaw_ = xn;
        ringEmph_[ringPos_] = emph;
        if (++ringPos_ == window_)
            ringPos_ = 0;
        hopSumSq_ += xn * xn;
        ++hopCount_;

        // Lattice FIR on the emphasised sample, with k interpolated from
        // the previous hop's set to the new set over the hop. The lerp is
        // precomputed into a scratch array first (independent FMAs, off
        // the f-chain's critical path) so the stage loop only loads k.
        const double frac = static_cast<double>(phase_) / static_cast<double>(hop);
        std::array<double, kLpcMaxOrder + 1> kNow{};
        for (std::size_t m = 1; m <= p; ++m)
            kNow[m] = kPrev_[m] + (kCur_[m] - kPrev_[m]) * frac;
        double f = emph;
        double bPrev = bFir_[0]; // b_0[n-1] = the previous emphasised sample
        bFir_[0] = emph;         // b_0[n]
        for (std::size_t m = 1; m <= p; ++m) {
            const double km = kNow[m];
            const double fPrev = f;
            f = fPrev - km * bPrev;
            const double bNew = bPrev - km * fPrev;
            const double bOld = bFir_[m]; // b_m[n-1], still needed below
            bFir_[m] = bNew;              // b_m[n]
            bPrev = bOld;
        }
        residual[i] = static_cast<float>(f);

        ++phase_;
        ++sample_;
        if (phase_ == hop) {
            hopUpdate();
            phase_ = 0;
        }
    }
}

void LpcVoiceModel::synthesize(const float* e, float* y, std::size_t n) noexcept {
    if (n == 0)
        return;
    const std::size_t p = order_;
    const double pre = mu_;
    std::array<double, kLpcMaxOrder + 1> k{};
    // No hop update happens inside this call, so a segment found here stays
    // valid until the clock leaves it.
    SynthSegment seg{};
    bool haveSeg = false;
    for (std::size_t i = 0; i < n; ++i) {
        // The coefficients of the input `synthDelay` frames ago: the same
        // interpolated trajectory analyze uses, evaluated at the delayed
        // sample index (clamped at 0 before the stream starts).
        const std::uint64_t sEff =
            synthSample_ >= config_.synthDelay ? synthSample_ - config_.synthDelay : 0;
        if (!haveSeg || sEff < seg.start || sEff >= seg.end) {
            seg = findSegment(sEff);
            haveSeg = true;
        }
        double g = 1.0;
        coefficientsAt(seg, sEff, k.data(), g);

        // Lattice IIR (the inverse of the analysis lattice), then the gain.
        double f = static_cast<double>(e[i]);
        for (std::size_t m = p; m >= 1; --m) {
            const double bold = bIir_[m - 1]; // b_{m-1}[n-1]
            const double fPrev = f + k[m] * bold;
            bIir_[m] = bold - k[m] * fPrev; // b_m[n]
            f = fPrev;
        }
        bIir_[0] = f; // b_0[n] = f_0[n]
        const double v = g * f;
        // Inverse one-pole de-emphasis after G/A'(z).
        const double yv = v + pre * deemphState_;
        deemphState_ = yv;
        y[i] = static_cast<float>(yv);
        ++synthSample_;
    }
}

void LpcVoiceModel::envelopeDb(float* out, std::size_t points, bool warped) const noexcept {
    if (points == 0 || out == nullptr)
        return;
    const double* a = warped ? aWarpCur_.data() : aCur_.data();
    const double err = warped ? errWarp_ : err_;
    const std::size_t p = order_;
    for (std::size_t i = 0; i < points; ++i) {
        const double f = points > 1
                             ? static_cast<double>(i) * (sampleRate_ / 2.0) /
                                   static_cast<double>(points - 1)
                             : 0.0;
        const double w = 2.0 * kPi * f / sampleRate_;
        // A(e^{jw}) = 1 - sum_j a_j e^{-jwj}, evaluated by rotating
        // (cos w, sin w) one step per coefficient (one cos/sin per point).
        const double cw = std::cos(w);
        const double sw = std::sin(w);
        double cj = 1.0; // cos(w*j)
        double sj = 0.0; // sin(w*j)
        double re = 1.0;
        double im = 0.0;
        for (std::size_t j = 1; j <= p; ++j) {
            const double cjNext = cj * cw - sj * sw;
            const double sjNext = sj * cw + cj * sw;
            cj = cjNext;
            sj = sjNext;
            re -= a[j] * cj;
            im += a[j] * sj;
        }
        // Divide out the emphasis, so this is the envelope of the voice itself.
        const double emph2 = 1.0 - 2.0 * mu_ * cw + mu_ * mu_;
        out[i] = static_cast<float>(10.0 * std::log10(err / ((re * re + im * im) * emph2)));
    }
}

void LpcVoiceModel::levinson(const double* r, std::size_t p, double* k, double* a,
                             double& err) noexcept {
    // a[0] and k[0] are unused (kept 0). `prev` holds the previous
    // iteration's predictor coefficients while a[1..i-1] is updated.
    std::array<double, kLpcMaxOrder + 1> prev{};
    err = r[0];
    k[0] = 0.0;
    a[0] = 0.0;
    for (std::size_t i = 1; i <= p; ++i) {
        double acc = r[i];
        for (std::size_t j = 1; j < i; ++j)
            acc -= a[j] * r[i - j];
        // err > 0 whenever r comes from a non-degenerate frame (r[0] >=
        // 1e-12 is checked by the caller); guard anyway.
        const double ki = err != 0.0 ? acc / err : 0.0;
        // Clamp for safety: the synthesis lattice needs |k| < 1, and the
        // clamped value is used consistently for k, a and err below.
        const double kClamped = std::clamp(ki, -0.9999, 0.9999);
        k[i] = kClamped;
        for (std::size_t j = 1; j < i; ++j)
            prev[j] = a[j];
        for (std::size_t j = 1; j < i; ++j)
            a[j] = prev[j] - kClamped * prev[i - j];
        a[i] = kClamped;
        err *= 1.0 - kClamped * kClamped;
    }
}

void LpcVoiceModel::hopUpdate() noexcept {
    const std::size_t p = order_;

    // Hop RMS (raw input) against the silence threshold.
    const double rms = hopCount_ > 0
                           ? std::sqrt(hopSumSq_ / static_cast<double>(hopCount_))
                           : 0.0;
    const double thresh = std::pow(10.0, static_cast<double>(silenceDb_) / 20.0);
    hopSumSq_ = 0.0;
    hopCount_ = 0;

    bool hold = !(rms >= thresh);
    std::array<double, kLpcMaxOrder + 1> kNew{};
    std::array<double, kLpcMaxOrder + 1> aNew{};
    double errNew = 1.0;
    if (!hold) {
        // Newest `window` emphasised samples, in order, Hann-windowed
        // (symmetric Hann, 0.5 - 0.5*cos(2*pi*n/(W-1))). The ring is read
        // in two linear segments (no per-element modulo) starting at the
        // oldest sample, ringPos_.
        const std::size_t firstLen = window_ - ringPos_;
        for (std::size_t n = 0; n < firstLen; ++n)
            frameScratch_[n] = ringEmph_[ringPos_ + n] * hann_[n];
        for (std::size_t n = firstLen; n < window_; ++n)
            frameScratch_[n] = ringEmph_[n - firstLen] * hann_[n];
        // Autocorrelation r[0..p] by direct sum. Four independent
        // accumulators break the serial FMA chain on `acc` (a single
        // accumulator is latency-bound at ~4-5 cycles per element and
        // costs ~100 us per hop update at order 52).
        for (std::size_t m = 0; m <= p; ++m) {
            const std::size_t limit = window_ - m;
            double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
            std::size_t n = 0;
            for (; n + 3 < limit; n += 4) {
                acc0 += frameScratch_[n] * frameScratch_[n + m];
                acc1 += frameScratch_[n + 1] * frameScratch_[n + 1 + m];
                acc2 += frameScratch_[n + 2] * frameScratch_[n + 2 + m];
                acc3 += frameScratch_[n + 3] * frameScratch_[n + 3 + m];
            }
            for (; n < limit; ++n)
                acc0 += frameScratch_[n] * frameScratch_[n + m];
            autocorr_[m] = (acc0 + acc1) + (acc2 + acc3);
        }
        // Gaussian lag window, then the white-noise floor on r[0].
        for (std::size_t m = 0; m <= p; ++m) {
            const double t = 2.0 * kPi * config_.lagWindowHz * static_cast<double>(m) / sampleRate_;
            autocorr_[m] *= std::exp(-0.5 * t * t);
        }
        autocorr_[0] *= 1.0 + std::pow(10.0, config_.whiteNoiseDb / 10.0);
        if (!(autocorr_[0] >= 1e-12))
            hold = true;
        else
            levinson(autocorr_.data(), p, kNew.data(), aNew.data(), errNew);
    }

    if (!hold) {
        kPrev_ = kCur_;
        kCur_ = kNew;
        aCur_ = aNew;
        err_ = errNew;
        if (isWarpIdentity(warp_)) {
            // Exact bypass: the synthesis coefficients are bit-identical
            // copies of the analysis ones (not a recomputed refit).
            kWarpPrev_ = kWarpCur_;
            kWarpCur_ = kNew;
            aWarpCur_ = aNew;
            errWarp_ = errNew;
            gPrev_ = gCur_;
            gCur_ = 1.0;
        } else {
            // Warp: envelope on the 1024-point grid, resampled and
            // tilt/EQ-weighted, inverse-transformed to an autocorrelation,
            // refit by Levinson. The resampling acts on the voice envelope
            // (emphasis divided out, then put back at the target frequency):
            // stretching the emphasised envelope would stretch the emphasis
            // tilt as well, which de-emphasis then doesn't undo, and the
            // leftover tilt pulls F1 down for phi > 1 and up for phi < 1.
            std::fill(warpFftIn_.begin(), warpFftIn_.end(), 0.0f);
            warpFftIn_[0] = 1.0f;
            for (std::size_t j = 1; j <= p; ++j)
                warpFftIn_[j] = static_cast<float>(-aNew[j]);
            warpFft_.forward(warpFftIn_.data(), warpSpectrum_.data());
            for (std::size_t kk = 0; kk < kLpcEnvelopePoints; ++kk) {
                const double mag2 = std::norm(warpSpectrum_[kk]);
                // Voice domain: divide out the emphasis before warping (see below).
                warpEnv_[kk] = errNew / std::max(mag2 * emph2_[kk], 1e-30);
            }
            // E(fs/2) for the Nyquist bin and for source frequencies at
            // the top of the grid.
            double aNyqRe = 1.0;
            for (std::size_t j = 1; j <= p; ++j)
                aNyqRe -= aNew[j] * ((j % 2 == 1) ? -1.0 : 1.0);
            const double eNyq = errNew / std::max(aNyqRe * aNyqRe * emph2_[kLpcEnvelopePoints], 1e-30);

            const double phi = static_cast<double>(warp_.formantFactor);
            const auto envelopeAt = [&](double f) noexcept {
                const double pos = f * static_cast<double>(kWarpFftSize) / sampleRate_;
                if (pos >= static_cast<double>(kLpcEnvelopePoints))
                    return eNyq;
                const std::size_t i0 = static_cast<std::size_t>(pos);
                const double frac = pos - static_cast<double>(i0);
                const double e0 = warpEnv_[i0];
                const double e1 = i0 + 1 < kLpcEnvelopePoints ? warpEnv_[i0 + 1] : eNyq;
                return e0 * (1.0 - frac) + e1 * frac;
            };
            // Tilt + EQ gain is precomputed per grid point in setWarp
            // (warpGain_); the loop only resamples the envelope.
            for (std::size_t kk = 0; kk < kLpcEnvelopePoints; ++kk) {
                const double f = static_cast<double>(kk) * sampleRate_ /
                                 static_cast<double>(kWarpFftSize);
                const double fSrc = std::min(f / phi, sampleRate_ / 2.0);
                const double eSrc = envelopeAt(fSrc);
                warpInvSpectrum_[kk] = std::complex<float>{
                    static_cast<float>(eSrc * emph2_[kk] * warpGain_[kk]), 0.0f};
            }
            {
                const double f = sampleRate_ / 2.0;
                const double fSrc = std::min(f / phi, sampleRate_ / 2.0);
                const double eSrc = envelopeAt(fSrc);
                warpInvSpectrum_[kLpcEnvelopePoints] = std::complex<float>{
                    static_cast<float>(eSrc * emph2_[kLpcEnvelopePoints] * warpGain_[kLpcEnvelopePoints]),
                    0.0f};
            }
            warpFft_.inverse(warpInvSpectrum_.data(), warpAutocorr_.data());
            std::array<double, kLpcMaxOrder + 1> rPrime{};
            for (std::size_t m = 0; m <= p; ++m)
                rPrime[m] = static_cast<double>(warpAutocorr_[m]);
            std::array<double, kLpcMaxOrder + 1> kWarpNew{};
            std::array<double, kLpcMaxOrder + 1> aWarpNew{};
            double errWarpNew = 1.0;
            levinson(rPrime.data(), p, kWarpNew.data(), aWarpNew.data(), errWarpNew);
            const double gNew = std::clamp(std::sqrt(errWarpNew / errNew), 0.125, 8.0);
            kWarpPrev_ = kWarpCur_;
            kWarpCur_ = kWarpNew;
            aWarpCur_ = aWarpNew;
            errWarp_ = errWarpNew;
            gPrev_ = gCur_;
            gCur_ = gNew;
        }
    } else {
        // Silence hold: freeze the interpolation at the current sets so
        // neither analyze nor synthesize re-ramps towards an older set.
        kPrev_ = kCur_;
        kWarpPrev_ = kWarpCur_;
        gPrev_ = gCur_;
    }

    // Record the warped set for the synthesis trajectory (held or new).
    // The set computed at this boundary becomes current at sample_ (the
    // count after the sample that completed the hop).
    if (historyCount_ < history_.size()) {
        HistoryEntry& entry = history_[(historyHead_ + historyCount_) % history_.size()];
        entry.boundary = sample_;
        entry.k = kWarpCur_;
        entry.g = gCur_;
        ++historyCount_;
    } else {
        HistoryEntry& entry = history_[historyHead_];
        entry.boundary = sample_;
        entry.k = kWarpCur_;
        entry.g = gCur_;
        historyHead_ = (historyHead_ + 1) % history_.size();
        historyDropped_ = true;
    }
}

LpcVoiceModel::SynthSegment LpcVoiceModel::findSegment(std::uint64_t s) const noexcept {
    SynthSegment seg{};
    seg.end = UINT64_MAX;
    for (std::size_t i = 0; i < historyCount_; ++i) {
        const HistoryEntry& entry = history_[(historyHead_ + i) % history_.size()];
        if (entry.boundary <= s) {
            seg.prev = seg.cur;
            seg.cur = &entry;
            seg.start = entry.boundary;
        } else {
            seg.end = entry.boundary;
            break;
        }
    }
    return seg;
}

void LpcVoiceModel::coefficientsAt(const SynthSegment& seg, std::uint64_t s, double* kOut,
                                   double& gOut) const noexcept {
    const HistoryEntry* cur = seg.cur;
    const HistoryEntry* prev = seg.prev;
    if (cur == nullptr) {
        // Before the first hop boundary: the identity filter.
        for (std::size_t m = 0; m <= order_; ++m)
            kOut[m] = 0.0;
        gOut = 1.0;
        return;
    }
    // Fraction of the hop from this set's boundary to the next; clamped so a
    // delayed synthesis clock past the newest boundary holds the set.
    const double frac = std::min(static_cast<double>(s - cur->boundary) /
                                     static_cast<double>(config_.hop),
                                 1.0);
    if (prev == nullptr && historyDropped_) {
        // The previous set was evicted (analyze ran far ahead of
        // synthesize, outside the alternating-call contract): hold the
        // current set instead of ramping from zero.
        for (std::size_t m = 0; m <= order_; ++m)
            kOut[m] = cur->k[m];
        gOut = cur->g;
        return;
    }
    const double gPrev = prev != nullptr ? prev->g : 1.0;
    gOut = gPrev + (cur->g - gPrev) * frac;
    for (std::size_t m = 0; m <= order_; ++m) {
        const double kPrev = prev != nullptr ? prev->k[m] : 0.0;
        kOut[m] = kPrev + (cur->k[m] - kPrev) * frac;
    }
}

} // namespace dsptb::dsp
