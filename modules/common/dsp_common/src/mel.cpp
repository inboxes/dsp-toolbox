#include <dsptb/dsp_common/mel.hpp>

#include <dsptb/dsp_common/freq_map.hpp>

#include <cmath>
#include <stdexcept>

namespace dsptb::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLogMelAmin = 1e-10; // spec silence floor (power)

bool isPowerOfTwo(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

} // namespace

MelFilterbank::MelFilterbank(const MelConfig& config) {
    if (!isPowerOfTwo(config.fftSize) || config.fftSize < 32 || config.fftSize > 65536)
        throw std::invalid_argument("MelFilterbank: fftSize must be a power of two in [32, 65536]");
    if (!(config.sampleRate > 0.0))
        throw std::invalid_argument("MelFilterbank: sampleRate must be positive");
    if (config.nMels == 0)
        throw std::invalid_argument("MelFilterbank: nMels must be at least 1");
    const double fmax = config.fmax == 0.0 ? config.sampleRate / 2.0 : config.fmax;
    if (config.fmin < 0.0)
        throw std::invalid_argument("MelFilterbank: fmin must not be negative");
    if (!(fmax > config.fmin))
        throw std::invalid_argument("MelFilterbank: fmax must be greater than fmin");
    if (fmax > config.sampleRate / 2.0)
        throw std::invalid_argument("MelFilterbank: fmax must not exceed sampleRate / 2");
    if (config.norm != MelNorm::None && config.norm != MelNorm::Slaney)
        throw std::invalid_argument("MelFilterbank: unknown norm");

    nMels_ = config.nMels;
    bins_ = config.fftSize / 2 + 1;

    // Band edges: nMels + 2 frequencies evenly spaced on the mel scale
    // between fmin and fmax (librosa mel_frequencies). The Hz<->mel
    // conversions are the shared freq_map ones, which already
    // match librosa.hz_to_mel / mel_to_hz for both scales.
    const auto hzToMel = config.htk ? hzToMelHtk : hzToMelSlaney;
    const auto melToHz = config.htk ? melToHzHtk : melToHzSlaney;
    const double melMin = hzToMel(config.fmin);
    const double melMax = hzToMel(fmax);
    std::vector<double> edges(nMels_ + 2);
    for (std::size_t i = 0; i < edges.size(); ++i) {
        // np.linspace includes both endpoints exactly.
        const double mel = (i + 1 == edges.size())
                               ? melMax
                               : melMin + (melMax - melMin) * static_cast<double>(i) /
                                              static_cast<double>(edges.size() - 1);
        edges[i] = melToHz(mel);
    }

    // Triangles, following librosa.filters.mel term for term: for filter
    // i and bin frequency f, lower = (f - edges[i]) / (edges[i+1] -
    // edges[i]) and upper = (edges[i+2] - f) / (edges[i+2] - edges[i+1]);
    // the weight is max(0, min(lower, upper)). Slaney normalisation
    // scales filter i by 2 / (edges[i+2] - edges[i]).
    weights_.assign(nMels_ * bins_, 0.0f);
    rowFirst_.assign(nMels_, bins_);
    rowLast_.assign(nMels_, 0);
    for (std::size_t i = 0; i < nMels_; ++i) {
        const double fdiffLow = edges[i + 1] - edges[i];
        const double fdiffHigh = edges[i + 2] - edges[i + 1];
        const double enorm = config.norm == MelNorm::Slaney ? 2.0 / (edges[i + 2] - edges[i]) : 1.0;
        for (std::size_t k = 0; k < bins_; ++k) {
            const double f = static_cast<double>(k) * config.sampleRate / static_cast<double>(config.fftSize);
            const double lower = (f - edges[i]) / fdiffLow;
            const double upper = (edges[i + 2] - f) / fdiffHigh;
            const double w = lower < upper ? lower : upper;
            weights_[i * bins_ + k] = static_cast<float>(w > 0.0 ? w * enorm : 0.0);
            if (w > 0.0) {
                if (rowFirst_[i] == bins_)
                    rowFirst_[i] = k;
                rowLast_[i] = k + 1;
            }
        }
        if (rowFirst_[i] == bins_) { // empty filter: empty range
            rowFirst_[i] = 0;
            rowLast_[i] = 0;
        }
    }
}

void MelFilterbank::apply(const float* power, float* melPower) const noexcept {
    // Accumulate in double: a mel channel sums up to bins() power values
    // and the golden comparison is against float64 librosa arithmetic.
    for (std::size_t i = 0; i < nMels_; ++i) {
        const float* row = weights_.data() + i * bins_;
        double sum = 0.0;
        for (std::size_t k = rowFirst_[i]; k < rowLast_[i]; ++k)
            sum += static_cast<double>(row[k]) * static_cast<double>(power[k]);
        melPower[i] = static_cast<float>(sum);
    }
}

Mfcc::Mfcc(const MelConfig& config, std::size_t nMfcc) : filterbank_(config), nMfcc_(nMfcc) {
    if (nMfcc == 0 || nMfcc > filterbank_.nMels())
        throw std::invalid_argument("Mfcc: nMfcc must be in [1, nMels]");
    const std::size_t nMels = filterbank_.nMels();
    melPower_.assign(nMels, 0.0f);
    logMel_.assign(nMels, 0.0f);
    // DCT-II with orthonormal scaling (scipy.fft.dct type=2,
    // norm='ortho'): C_j = s_j * sum_i L_i * cos(pi * j * (2i + 1) /
    // (2 * nMels)), with s_0 = sqrt(1 / nMels) and s_j = sqrt(2 / nMels)
    // for j >= 1. Precomputed once; compute() is then multiply-adds only.
    dctCos_.assign(nMfcc_ * nMels, 0.0);
    for (std::size_t j = 0; j < nMfcc_; ++j) {
        const double scale = j == 0 ? std::sqrt(1.0 / static_cast<double>(nMels))
                                    : std::sqrt(2.0 / static_cast<double>(nMels));
        for (std::size_t i = 0; i < nMels; ++i)
            dctCos_[j * nMels + i] =
                scale * std::cos(kPi * static_cast<double>(j) * (2.0 * static_cast<double>(i) + 1.0) /
                                 (2.0 * static_cast<double>(nMels)));
    }
}

void Mfcc::logMel(const float* power, float* logMelOut) noexcept {
    filterbank_.apply(power, melPower_.data());
    // librosa.power_to_db(M, ref=1.0, amin=1e-10, top_db=None):
    // 10 * log10(max(amin, M)) (the ref term is log10(1) = 0).
    for (std::size_t i = 0; i < logMel_.size(); ++i) {
        const double m = static_cast<double>(melPower_[i]);
        logMel_[i] = static_cast<float>(10.0 * std::log10(m > kLogMelAmin ? m : kLogMelAmin));
    }
    for (std::size_t i = 0; i < logMel_.size(); ++i)
        logMelOut[i] = logMel_[i];
}

void Mfcc::compute(const float* power, float* mfccOut) noexcept {
    logMel(power, logMel_.data());
    const std::size_t nMels = logMel_.size();
    for (std::size_t j = 0; j < nMfcc_; ++j) {
        const double* row = dctCos_.data() + j * nMels;
        double sum = 0.0;
        for (std::size_t i = 0; i < nMels; ++i)
            sum += row[i] * static_cast<double>(logMel_[i]);
        mfccOut[j] = static_cast<float>(sum);
    }
}

} // namespace dsptb::dsp
