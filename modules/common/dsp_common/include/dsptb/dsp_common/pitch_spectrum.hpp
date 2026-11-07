#pragma once

// PitchSpectrum: a semitone-spaced spectrum built from spectral peaks.
//
// Only real tones count: window leakage and noise floors would otherwise add mass to the wrong notes, so
// each frame's power spectrum is reduced to interpolated peaks (as in HPCP, Gomez 2006). Each peak is then
// spread over neighbouring bins with cos^2 weighting, so a peak near a note boundary splits smoothly rather
// than jumping into one note. The mapping relabels what the FFT measured and can't add resolution: choose
// the FFT size with fftSizeForRate.
//
// The constructor allocates. Per-frame calls are noexcept and never allocate, lock or throw. One instance
// must not be used from several threads at once.

#include <dsptb/dsp_common/tuning.hpp>

#include <cstddef>
#include <vector>

namespace dsptb::dsp {

struct PitchSpectrumConfig {
    double minHz = 55.0;              // A1. Rounded outward to whole semitones of the tuning
    double maxHz = 1760.0;            // A6
    int binsPerSemitone = 3;          // 1..10
    double windowSemitones = 4.0 / 3; // cos^2 window width; a peak feeds every bin within +-window/2
    float peakFloor = 0.01f;          // ignore peaks below this fraction of the largest (-40 dB)
    std::size_t maxPeaks = 64;        // keep the strongest N peaks per frame
    int harmonics = 1;                // 1 = off. N > 1: a peak also adds to f/2 ... f/N, weight slope^(h-1)
    float harmonicSlope = 0.6f;
    bool normalizeFrame = true; // divide each frame by its max, so louder frames don't dominate
};

struct SpectralPeak {
    float hz = 0.0f;
    float magnitude = 0.0f;
    float midi = 0.0f; // fractional, with the tuning applied
};

class PitchSpectrum {
public:
    // fftSize is the actual size of the power spectra passed to analyze, after fftSizeForRate.
    // Throws std::invalid_argument for an invalid configuration.
    PitchSpectrum(double sampleRate, std::size_t fftSize, const PitchSpectrumConfig& config = {},
                  const Tuning& tuning = {});

    // Takes effect from the next frame. numBins() and the bin layout never change.
    void setTuning(const Tuning& tuning) noexcept;

    // power: fftSize/2+1 raw |X|^2 values from StftAnalyzer::pushPower (Hann window). Fills the semitone
    // bins and the peak list for this frame.
    void analyze(const float* power) noexcept;

    // (maxMidi - minMidi) * binsPerSemitone. Bin i's centre is minMidi + i / binsPerSemitone, so with
    // 3 bins per semitone every third bin sits exactly on a tuned note.
    std::size_t numBins() const noexcept { return bins_.size(); }
    const float* bins() const noexcept { return bins_.data(); }
    double binMidi(std::size_t i) const noexcept {
        return static_cast<double>(minMidi_) + static_cast<double>(i) / binsPerSemitone_;
    }
    std::size_t peakCount() const noexcept { return peakCount_; }
    // This frame's kept peaks, strongest first.
    const SpectralPeak* peaks() const noexcept { return peaks_.data(); }

    // Sums the bins over octaves into 12 * binsPerSemitone values. out[0] is C, centred on the tuned note.
    void foldToPitchClasses(float* out) const noexcept;

private:
    struct Candidate {
        float hz;
        float magnitude;
    };

    void findPeaks(const float* power) noexcept;
    void spreadPeak(double midi, float weight) noexcept;

    double sampleRate_;
    std::size_t fftSize_;
    PitchSpectrumConfig config_;
    Tuning tuning_;
    int binsPerSemitone_;
    int minMidi_ = 0;
    std::size_t firstBin_ = 0;
    std::size_t lastBin_ = 0;
    std::vector<float> bins_;
    std::vector<float> magnitudes_;
    std::vector<Candidate> candidates_;
    std::vector<SpectralPeak> peaks_;
    std::vector<double> harmonicShift_;  // 12*log2(h)
    std::vector<float> harmonicWeight_;  // slope^(h-1)
    std::size_t peakCount_ = 0;
};

} // namespace dsptb::dsp
