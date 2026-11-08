#pragma once

// LpcVoiceModel: real-time LPC source-filter model of a single voice.
//
// The voice is split into a flat residual (the source, which carries the
// pitch) and an all-pole filter (the formants). `analyze` runs the inverse
// (all-zero) lattice filter; `synthesize` runs the matching all-pole lattice
// with warped coefficients. A caller can pitch-shift the residual between
// `analyze` and `synthesize`.
//
// Pre-emphasis is part of the signal path (maintainer ruling): `analyze`
// filters the pre-emphasised signal, and `synthesize` applies the inverse
// one-pole de-emphasis after G/A'(z). A(z) is fit to whiten the emphasised
// signal, so this is what makes the residual flat; filtering the raw input
// would leave the emphasis tilt (about 22 dB across 200-4000 Hz) in it.
// The coefficient is `preEmphasis` at 48 kHz, scaled to
// mu = preEmphasis^(48000/fs) so the emphasis corner is at the same
// frequency at every rate.
//
// Sign and order conventions:
//   * Predictor: xhat[n] = sum_{i=1..p} a_i x[n-i], so A(z) = 1 - sum a_i z^-i
//     and the residual is e[n] = x[n] - xhat[n].
//   * Reflection coefficients k_i come from Levinson-Durbin on the
//     autocorrelation of the pre-emphasised, Hann-windowed frame, and are
//     clamped to (-0.9999, 0.9999).
//   * Lattice FIR (analysis), per sample, with b_m[n] the backward error:
//       f_0 = b_0 = x (pre-emphasised); for m = 1..p:
//       f_m = f_{m-1} - k_m b_{m-1}[n-1]; b_m = b_{m-1}[n-1] - k_m f_{m-1};
//     and e = f_p. Lattice IIR (synthesis) inverts it:
//       f_p = e; for m = p..1:
//       f_{m-1} = f_m + k'_m b_{m-1}[n-1]; b_m = b_{m-1}[n-1] - k'_m f_{m-1};
//     and y = G f_0, then de-emphasis y[n] = v[n] + mu y[n-1].
//   * `k` is linearly interpolated from the previous hop's set to the new
//     set over the hop (per-sample). `synthesize` evaluates the same
//     interpolated trajectory at time n - synthDelay, so with no warp the
//     synthesis filter is the exact inverse of the analysis filter at every
//     sample.
//   * `envelopeDb` reports the spectral envelope of the voice itself:
//     E(f) / |P(f)|^2, with E(f) = err / |A(e^{jw})|^2 the envelope of the
//     emphasised signal and P(z) = 1 - mu z^-1.
//   * Warp: the voice envelope E(f)/|P(f)|^2 is resampled at
//     min(f/phi, fs/2), multiplied back by |P(f)|^2 and by the tilt and EQ
//     gains, inverse-transformed to an autocorrelation and refit by
//     Levinson. G = sqrt(err'/err), clamped to [0.125, 8]. Warping the
//     emphasised envelope directly would also stretch the emphasis tilt,
//     which de-emphasis doesn't undo; that pulled F1 down by about 4 % at
//     phi 1.25 and up at phi 0.8.
//   * EQ gains are interpolated linearly in log-frequency between the band
//     centres, flat outside them. Tilt is T(f) = tilt * log2(max(f, 250)/1000).
//
// Known limit: a 21 ms causal window can't resolve narrow formants exactly,
// so the residual keeps a 3-4 dB trace of the input's formants. After a
// formant shift that trace stays at the original positions, slightly
// pulling a shifted formant that lands near an original one.
//
// References: J. Makhoul, "Linear prediction: a tutorial review", Proc.
// IEEE 63(4), 1975; J. Markel & A. Gray, "Linear Prediction of Speech"
// (1976), lattice filters; D. Ellis, "Spectral warping of LPC resonance
// models" (LabROSA); Lawlor & Fagan, "A novel efficient algorithm for voice
// gender conversion" (1999).

#include <dsptb/dsp_common/fft.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb::dsp {

// Highest order the implementation supports (order > 128 throws). The auto
// order keeps about one pole pair per kHz, which needs 100 at 96 kHz.
inline constexpr std::size_t kLpcMaxOrder = 128;
// Envelope grid points used by the warp (spec: 1024 points over 0..fs/2).
inline constexpr std::size_t kLpcEnvelopePoints = 1024;
// Coefficient sets kept for the delayed synthesis trajectory. The caller
// may analyze one call of up to `maxBlock` samples before synthesizing
// it, and `synthDelay` is at most 4*hop, so the trajectory needs the sets
// from the hop boundaries spanning (maxBlock + synthDelay) samples, plus
// one for the interpolation pair; the constructor sizes the ring to that
// (at least 8 entries), so no set is evicted inside the call contract.
inline constexpr std::size_t kLpcHistorySetsMin = 8;

struct LpcConfig {
    std::size_t hop = 256;          // coefficient update interval, frames
    std::size_t order = 0;          // 0 = auto: min(104, round(fs/1000) + 4)
    std::size_t window = 0;         // 0 = auto: 1024 at fs <= 50 kHz, else 2048 (Hann over the newest samples)
    std::size_t maxBlock = 4096;
    std::size_t synthDelay = 0;     // synthesis uses the coefficients of the input `synthDelay` frames ago
    double lagWindowHz = 40.0;      // Gaussian lag window (smooths harmonics out of the envelope)
    double whiteNoiseDb = -40.0;
    double preEmphasis = 0.97;      // at 48 kHz; part of the signal path (see the header comment)
};

struct WarpSettings {
    float formantFactor = 1.f;
    float tiltDbPerOct = 0.f;
    float eqDb[10] = {};
};

// Real-time LPC source-filter model. All memory is allocated by the
// constructor. `analyze`, `synthesize`, `setWarp`, `setSilenceDb`,
// `envelopeDb` and `reset` are noexcept and never allocate, lock or throw,
// so they are safe on a real-time thread. `setWarp` takes effect from the
// next hop boundary. Call `analyze(n)` then `synthesize(n)` with the SAME
// n, alternately; n <= maxBlock; in == out is allowed for both calls.
//
// Thread safety: one instance is a single stateful stream; it must not be
// used concurrently from several threads.
class LpcVoiceModel {
public:
    // Throws std::invalid_argument for an invalid config: non-positive or
    // non-finite sample rate, hop == 0, maxBlock == 0, order > 128,
    // window < 2*order, synthDelay > 4*hop, or a non-finite lagWindowHz /
    // whiteNoiseDb / preEmphasis.
    LpcVoiceModel(double sampleRate, const LpcConfig& config);
    // Drops all state and restarts at sample 0, exactly as freshly
    // constructed (same output as a new instance on the same input).
    void reset() noexcept;

    std::size_t order() const noexcept { return order_; }
    std::size_t hop() const noexcept { return config_.hop; }

    // Warp applied from the next hop boundary. Identity settings
    // (formantFactor == 1, tilt == 0, all EQ == 0) skip the warp, so the
    // synthesis coefficients are bit-identical to the analysis ones.
    void setWarp(const WarpSettings& warp) noexcept;
    // Hop RMS below this level (dBFS, default -70): hold coefficients.
    void setSilenceDb(float db) noexcept;

    // Residual of the pre-emphasised input. e = A(z) x_emph.
    void analyze(const float* x, float* residual, std::size_t n) noexcept;
    // y = G e / A'(z), then inverse one-pole de-emphasis.
    // The coefficients used are those of the input `synthDelay` frames ago.
    void synthesize(const float* e, float* y, std::size_t n) noexcept;

    // Prediction error `err` of the newest analysis.
    float predictionError() const noexcept { return static_cast<float>(err_); }
    // Newest envelope, 0..fs/2, in dB: out[i] = 10*log10(E(f_i)) with
    // f_i = i * (fs/2) / (points - 1) for points >= 2 (f_0 = 0, and the
    // last point is fs/2). `warped` selects E'(f) (the warped envelope)
    // or E(f). Writes nothing when points == 0.
    void envelopeDb(float* out, std::size_t points, bool warped) const noexcept;

    // EQ band centres in Hz: 100 * 128^(i/9), i = 0..9.
    static void eqBandCentres(float out[10]) noexcept;

private:
    // One hop-boundary analysis: autocorrelation + Levinson on the newest
    // `window` samples, then the warp (or the identity copy). Runs at the
    // end of every hop; keeps the previous coefficients on silence or on a
    // degenerate frame (r[0] < 1e-12).
    void hopUpdate() noexcept;
    // Levinson-Durbin on r[0..p]: fills k[1..p], a[1..p] and err.
    // a follows the predictor convention above. k is clamped to +-0.9999.
    static void levinson(const double* r, std::size_t p, double* k, double* a,
                         double& err) noexcept;
    struct HistoryEntry;
    // The pair of history sets that the synthesis clock interpolates between,
    // valid for sample indices in [start, end). synthesize looks it up once
    // per hop instead of once per sample.
    struct SynthSegment {
        const HistoryEntry* prev = nullptr;
        const HistoryEntry* cur = nullptr;
        std::uint64_t start = 0;
        std::uint64_t end = 0;
    };
    SynthSegment findSegment(std::uint64_t s) const noexcept;
    // Interpolated analysis coefficients at absolute sample index s:
    // lerp from the previous hop's set to the set computed at the latest
    // boundary <= s. Used by synthesize with s = sample - synthDelay.
    void coefficientsAt(const SynthSegment& seg, std::uint64_t s, double* kOut, double& gOut) const noexcept;
    // Tilt + EQ gain per envelope-grid point for the current warp settings
    // (signal-independent); recomputed in setWarp and in the constructor.
    void updateWarpGain() noexcept;

    double sampleRate_ = 0.0;
    LpcConfig config_{};
    // Pre-emphasis coefficient at this rate: preEmphasis^(48000/fs), so the
    // emphasis corner sits at the same frequency at every sample rate.
    double mu_ = 0.97;
    std::size_t order_ = 0;
    std::size_t window_ = 0;

    WarpSettings warp_{};
    float silenceDb_ = -70.0f;

    // Analysis state (double, per the spec).
    std::vector<double> ringEmph_;   // newest `window` pre-emphasised samples (circular)
    std::size_t ringPos_ = 0;        // next write position in the ring
    double prevRaw_ = 0.0;           // previous raw sample, for the pre-emphasis filter
    std::uint64_t sample_ = 0;       // samples analysed since construction / reset
    std::size_t phase_ = 0;          // samples since the last hop boundary
    double hopSumSq_ = 0.0;          // sum of squared raw samples in the current hop
    std::size_t hopCount_ = 0;       // samples in the current hop
    std::array<double, kLpcMaxOrder + 1> kPrev_{}; // reflection coefficients, previous hop
    std::array<double, kLpcMaxOrder + 1> kCur_{};  // reflection coefficients, newest hop
    std::array<double, kLpcMaxOrder + 1> aCur_{};  // predictor coefficients, newest hop
    double err_ = 1.0;               // prediction error, newest hop
    // Warped coefficients (bit-identical copies of the analysis ones when
    // the warp is identity).
    std::array<double, kLpcMaxOrder + 1> kWarpPrev_{};
    std::array<double, kLpcMaxOrder + 1> kWarpCur_{};
    std::array<double, kLpcMaxOrder + 1> aWarpCur_{};
    double errWarp_ = 1.0;
    double gPrev_ = 1.0;
    double gCur_ = 1.0;
    // Lattice state (double, per the spec): backward errors b_m[n-1].
    std::array<double, kLpcMaxOrder + 1> bFir_{};
    std::array<double, kLpcMaxOrder + 1> bIir_{};
    double deemphState_ = 0.0; // previous de-emphasised output sample

    // Synthesis clock and coefficient history. Each entry is the warped
    // coefficient set computed at a hop boundary, with the boundary's
    // absolute sample index (the set becomes current at that index).
    struct HistoryEntry {
        std::uint64_t boundary = 0;
        std::array<double, kLpcMaxOrder + 1> k{};
        double g = 1.0;
    };
    std::vector<HistoryEntry> history_{}; // ring, sized in the constructor to span (maxBlock + synthDelay)
    std::size_t historyHead_ = 0;   // index of the oldest entry held
    std::size_t historyCount_ = 0;  // entries currently held (oldest first)
    bool historyDropped_ = false;   // set once the ring has wrapped (oldest sets lost)
    std::uint64_t synthSample_ = 0; // samples synthesised since construction / reset

    // Hop-update scratch (all allocated in the constructor).
    std::vector<double> frameScratch_;   // window samples: pre-emphasised, Hann-windowed
    std::vector<double> autocorr_;       // r[0..p]
    // Tilt + EQ gain per envelope-grid point (kLpcEnvelopePoints + 1 entries,
    // the last being Nyquist), precomputed in setWarp.
    std::array<double, kLpcEnvelopePoints + 1> warpGain_{};
    // |P(f)|^2 = |1 - mu e^{-jw}|^2 per envelope-grid point, for moving the
    // envelope between the emphasised and the voice domain.
    std::array<double, kLpcEnvelopePoints + 1> emph2_{};
    std::vector<double> hann_;            // analysis window, `window` points
    RealFft warpFft_;                     // transform size 2 * kLpcEnvelopePoints
    std::vector<float> warpFftIn_;        // zero-padded a coefficients (float, for RealFft)
    std::vector<std::complex<float>> warpSpectrum_; // A(e^{jw}) on the envelope grid
    std::vector<std::complex<float>> warpInvSpectrum_; // E'(f) half-spectrum (float, for RealFft::inverse)
    std::vector<double> warpEnv_;         // E(f) on the envelope grid
    std::vector<float> warpAutocorr_;     // r'[0..N-1] from the inverse FFT
};

} // namespace dsptb::dsp
