#pragma once

// TuningEstimator: how far a recording's tuning is from A4 = 440 Hz.
//
// Real recordings are often a few to 40+ cents off 440, and near the halfway point every pitch decision
// flips. The estimate is the mode of a circular histogram of the peaks' cent deviations, not a weighted
// mean, because harmonics sit off the equal-tempered grid (the 5th harmonic is about 14 cents flat) and
// would bias a mean.
//
// An offset of exactly +-50 cents is ambiguous: A = 452.9 is +50 from A or -50 from A#. Note names stay
// right either way, because the reference moves with it.
//
// addPeaks, addFrames, result and reset are noexcept and never allocate, lock or throw.

#include <dsptb/dsp_common/pitch_spectrum.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dsptb::dsp {

struct TuningEstimate {
    bool valid = false;       // enough tonal evidence: at least 2 s analysed, and some peak energy
    float offsetCents = 0.0f; // [-50, +50); 0 when not valid
    double a4Hz = 440.0;      // 440 * 2^(offsetCents/1200); 440 when not valid
    float confidence = 0.0f;  // 0..1: 1 means all peaks agree, 0 means random
    bool confident = false;   // valid && confidence >= 0.5
};

class TuningEstimator {
public:
    // Assumes frames at the default hop of fftSizeForRate(rate, 4096), about 85 ms.
    TuningEstimator() noexcept;
    // sampleRate and hop say how much audio one frame stands for, which decides when the estimate is valid.
    TuningEstimator(double sampleRate, std::size_t hop) noexcept;

    void reset() noexcept;
    // Peaks analysed with Tuning{440}.
    void addPeaks(const SpectralPeak* peaks, std::size_t n) noexcept;
    // Frames analysed so far.
    void addFrames(std::uint32_t count) noexcept;
    // Can be called at any time.
    TuningEstimate result() const noexcept;

private:
    double secondsPerFrame_;
    std::array<double, 100> histogram_{}; // 1 cent per bin; bin 0 covers [-50, -49)
    double sumRe_ = 0.0;
    double sumIm_ = 0.0;
    double weight_ = 0.0;
    std::uint64_t frames_ = 0;
};

// Convenience: runs STFT + PitchSpectrum + TuningEstimator over a whole buffer. For a worker thread:
// it allocates. Throws std::invalid_argument for a non-positive sample rate.
TuningEstimate estimateTuning(const float* mono, std::size_t n, double sampleRate);

} // namespace dsptb::dsp
