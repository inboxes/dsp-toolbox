#pragma once

// Mel filterbank and MFCC.
//
// Numerically matched to librosa: the filterbank equals
// librosa.filters.mel(sr, n_fft, n_mels, fmin, fmax, htk, norm), the
// log-mel equals librosa.power_to_db(mel_power, ref=1.0, amin=1e-10,
// top_db=None), and the MFCC equals scipy.fft.dct(log_mel, type=2,
// norm='ortho') truncated to nMfcc coefficients. Golden files generated
// by tools/gen_goldens.py pin this equivalence in tests.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb::dsp {

// Area normalisation of the triangular mel filters: Slaney divides each
// triangle by (2 / bandwidth) so each channel carries approximately
// constant energy (librosa norm='slaney'); None leaves the peak at 1.0.
enum class MelNorm : std::uint8_t { None, Slaney };

struct MelConfig {
    std::size_t fftSize = 2048; // power of two in [32, 65536], as for RealFft
    double sampleRate = 48000;  // Hz
    std::size_t nMels = 40;
    double fmin = 0; // Hz
    double fmax = 0; // Hz; 0 means sampleRate / 2
    bool htk = false; // HTK mel scale instead of Slaney
    MelNorm norm = MelNorm::Slaney;
};

// Triangular mel filterbank over the power spectrum of one frame.
//
// The nMels + 2 band-edge frequencies are evenly spaced on the mel scale
// between fmin and fmax (converted back to Hz); bin k sits at
// k * sampleRate / fftSize Hz; each filter is the triangle through edges
// (i, i+1, i+2), exactly as in librosa.filters.mel. Weights are computed
// in double (librosa itself returns float32; the difference is below
// 1e-7) and stored as float, row-major nMels x (fftSize/2 + 1).
//
// All memory is allocated by the constructor. apply() is noexcept and
// never allocates, so it is safe on a real-time thread.
class MelFilterbank {
public:
    // Throws std::invalid_argument for an invalid config: fftSize not a
    // power of two in [32, 65536], sampleRate <= 0, nMels == 0,
    // fmin < 0, or fmax (after the 0 -> fs/2 substitution) <= fmin or
    // above sampleRate / 2.
    explicit MelFilterbank(const MelConfig& config);

    // melPower[m] = sum_k weights[m][k] * power[k]. power has
    // fftSize/2 + 1 values, melPower has nMels() values.
    void apply(const float* power, float* melPower) const noexcept;

    std::size_t nMels() const noexcept { return nMels_; }
    std::size_t bins() const noexcept { return bins_; }
    // Row-major weights, nMels() x bins(), for tests and inspection.
    const float* weights() const noexcept { return weights_.data(); }

private:
    std::size_t nMels_ = 0;
    std::size_t bins_ = 0;
    std::vector<float> weights_;
    // Per-row first/last bin with a nonzero weight: apply() sums only
    // the row's support (the skipped terms are exactly 0.0, so the sum
    // is unchanged) — the triangles are narrow compared with the full
    // spectrum, which keeps apply() fast.
    std::vector<std::size_t> rowFirst_;
    std::vector<std::size_t> rowLast_; // exclusive
};

// MFCC pipeline on top of a MelFilterbank built from the same config:
//   mel power -> 10*log10(max(1e-10, mel)) -> DCT-II (orthonormal)
//   -> first nMfcc coefficients.
// This is librosa.feature.mfcc with dct_type=2, norm='ortho' applied to
// a precomputed power spectrum (librosa's ref=1.0, amin=1e-10,
// top_db=None log conversion).
//
// All memory is allocated by the constructor. compute() and logMel()
// are noexcept and never allocate.
class Mfcc {
public:
    // nMfcc must satisfy 1 <= nMfcc <= config.nMels. Throws
    // std::invalid_argument otherwise (or for an invalid MelConfig).
    Mfcc(const MelConfig& config, std::size_t nMfcc = 13);

    // Full pipeline: power (fftSize/2 + 1 values) -> mfccOut (nMfcc
    // values).
    void compute(const float* power, float* mfccOut) noexcept;
    // Intermediate stage: power -> logMelOut (nMels values), the dB
    // log-mel spectrum the DCT consumes. Exposed for tests.
    void logMel(const float* power, float* logMelOut) noexcept;

    std::size_t nMfcc() const noexcept { return nMfcc_; }
    std::size_t nMels() const noexcept { return filterbank_.nMels(); }

private:
    MelFilterbank filterbank_;
    std::size_t nMfcc_ = 0;
    std::vector<float> melPower_;    // scratch, nMels
    std::vector<float> logMel_;      // scratch, nMels
    std::vector<double> dctCos_;     // nMfcc x nMels DCT-II ortho table
};

} // namespace dsptb::dsp
