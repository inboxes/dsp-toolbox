#include <dsptb/dsp_common/spectral_features.hpp>

#include <dsptb/dsp/window.hpp>

#include <algorithm> // std::max — include directly rather than relying on a transitive one
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

// librosa's amin for spectral_flatness: the power spectrum is clamped to
// at least this before the geometric/arithmetic means.
constexpr double kAmin = 1e-10;

} // namespace

SpectralFeatureExtractor::SpectralFeatureExtractor(double sampleRate, std::size_t fftSize,
                                                   std::size_t hop, float rolloffPct)
    : sampleRate_(sampleRate), fftSize_(fftSize), hop_(hop), rolloffPct_(rolloffPct),
      fft_(fftSize) {
    // Note the construction order follows StftAnalyzer/YinPitch: the FFT is
    // sized from fftSize before these checks run, so an out-of-range
    // fftSize surfaces as RealFft's invalid_argument.
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("SpectralFeatureExtractor: sample rate must be positive");
    if (hop == 0)
        throw std::invalid_argument("SpectralFeatureExtractor: hop must be at least 1");
    if (!(rolloffPct > 0.0f) || rolloffPct > 1.0f)
        throw std::invalid_argument("SpectralFeatureExtractor: rolloffPct must be in (0, 1]");

    window_.assign(fftSize_, 0.0f);
    hannWindow(window_.data(), fftSize_);
    frame_.assign(fftSize_, 0.0f);
    windowed_.assign(fftSize_, 0.0f);
    mag_.assign(bins(), 0.0f);
    prevMag_.assign(bins(), 0.0f);
}

void SpectralFeatureExtractor::reset() noexcept {
    fill_ = 0;
    skip_ = 0;
    frameIndex_ = 0;
    hasPrev_ = false;
}

SpectralFeatures SpectralFeatureExtractor::computeFeatures(const float* frame,
                                                           const float* prevMag) noexcept {
    const std::size_t n = fftSize_;
    const std::size_t binCount = bins();
    const double binHz = sampleRate_ / static_cast<double>(n);

    for (std::size_t i = 0; i < n; ++i)
        windowed_[i] = frame[i] * window_[i];
    fft_.magnitudeSpectrum(windowed_.data(), mag_.data());

    // One pass over the bins for the sums the definitions share. All
    // accumulation is in double: the features are ratios of sums over up
    // to 32769 bins, and float accumulation would cost real precision.
    double sumMag = 0.0;
    double sumFreqMag = 0.0;
    double sumLogPower = 0.0;
    double sumClampedPower = 0.0;
    for (std::size_t k = 0; k < binCount; ++k) {
        const double mag = static_cast<double>(mag_[k]);
        sumMag += mag;
        sumFreqMag += static_cast<double>(k) * binHz * mag;
        const double clamped = std::max(kAmin, mag * mag);
        sumLogPower += std::log(clamped);
        sumClampedPower += clamped;
    }

    SpectralFeatures out{};

    // Flatness needs no silence guard: on silence every clamped power is
    // amin, so the geometric and arithmetic means are both amin and the
    // ratio is exactly 1, as required.
    const double meanClamped = sumClampedPower / static_cast<double>(binCount);
    out.flatness = static_cast<float>(
        std::exp(sumLogPower / static_cast<double>(binCount)) / meanClamped);

    if (sumMag > 0.0) {
        const double centroid = sumFreqMag / sumMag;
        double sumSpread = 0.0;
        for (std::size_t k = 0; k < binCount; ++k) {
            const double dev = static_cast<double>(k) * binHz - centroid;
            sumSpread += dev * dev * static_cast<double>(mag_[k]);
        }
        out.centroidHz = static_cast<float>(centroid);
        out.spreadHz = static_cast<float>(std::sqrt(sumSpread / sumMag));

        // Smallest k whose cumulative magnitude reaches the threshold.
        // The loop always terminates: the full sum is sumMag itself,
        // which is >= rolloffPct * sumMag for rolloffPct <= 1.
        const double threshold = static_cast<double>(rolloffPct_) * sumMag;
        double cumulative = 0.0;
        std::size_t rolloffBin = binCount - 1;
        for (std::size_t k = 0; k < binCount; ++k) {
            cumulative += static_cast<double>(mag_[k]);
            if (cumulative >= threshold) {
                rolloffBin = k;
                break;
            }
        }
        out.rolloffHz = static_cast<float>(static_cast<double>(rolloffBin) * binHz);
    } else {
        // Silence: centroid, spread and rolloff are 0. (The
        // rolloff search alone would also land on bin 0, since 0 >=
        // pct * 0, but the centroid/spread divisions need the guard.)
        out.centroidHz = 0.0f;
        out.spreadHz = 0.0f;
        out.rolloffHz = 0.0f;
    }

    if (prevMag != nullptr) {
        double sumSq = 0.0;
        for (std::size_t k = 0; k < binCount; ++k) {
            const double diff =
                static_cast<double>(mag_[k]) - static_cast<double>(prevMag[k]);
            sumSq += diff * diff;
        }
        // K is the number of bins, following the (1/K) * sum_k normaliser
        // of §4.8's novelty.
        out.flux = static_cast<float>(std::sqrt(sumSq) / static_cast<double>(binCount));
    } else {
        out.flux = 0.0f;
    }

    // Temporal features of the unwindowed frame. sign(0) counts as
    // positive, so the sign test is simply x >= 0; sample 0 has no
    // predecessor and never counts, matching librosa's indicator array.
    double sumSqTime = 0.0;
    std::size_t crossings = 0;
    bool prevPositive = frame[0] >= 0.0f;
    sumSqTime += static_cast<double>(frame[0]) * static_cast<double>(frame[0]);
    for (std::size_t i = 1; i < n; ++i) {
        const double x = static_cast<double>(frame[i]);
        sumSqTime += x * x;
        const bool positive = frame[i] >= 0.0f;
        if (positive != prevPositive)
            ++crossings;
        prevPositive = positive;
    }
    out.zcr = static_cast<float>(static_cast<double>(crossings) / static_cast<double>(n));
    out.rms = static_cast<float>(std::sqrt(sumSqTime / static_cast<double>(n)));

    return out;
}

void SpectralFeatureExtractor::advanceFrame() noexcept {
    if (hop_ < fftSize_) {
        // The tail of this frame is the head of the next one.
        const std::size_t keep = fftSize_ - hop_;
        std::memmove(frame_.data(), frame_.data() + hop_, keep * sizeof(float));
        fill_ = keep;
    } else {
        fill_ = 0;
        skip_ = hop_ - fftSize_;
    }
}

} // namespace dsptb::dsp
