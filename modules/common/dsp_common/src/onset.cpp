#include <dsptb/dsp_common/onset.hpp>

#include <dsptb/dsp/window.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dsptb::dsp {

SpectralFluxOnset::SpectralFluxOnset(double sampleRate, const OnsetConfig& config)
    : config_(config), sampleRate_(sampleRate), lambda_(config.lambda), delta_(config.delta),
      fft_(config.fftSize) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("SpectralFluxOnset: sample rate must be positive");
    if (config.hop == 0)
        throw std::invalid_argument("SpectralFluxOnset: hop must be at least 1");
    if (config.medianFrames == 0 || config.medianFrames > kMaxMedianFrames)
        throw std::invalid_argument("SpectralFluxOnset: medianFrames must be in [1, 63]");
    if (!(config.compression > 0.0f))
        throw std::invalid_argument("SpectralFluxOnset: compression must be positive");
    // The constructor of RealFft above has already validated fftSize (a
    // power of two in [32, 65536]).

    minIntervalFrames_ = config.minIntervalMs / 1000.0 * sampleRate / static_cast<double>(config.hop);

    window_.assign(config.fftSize, 0.0f);
    hannWindow(window_.data(), config.fftSize); // periodic Hann, as for the STFT
    frame_.assign(config.fftSize, 0.0f);
    windowed_.assign(config.fftSize, 0.0f);
    magnitudes_.assign(fft_.bins(), 0.0f);
    prevCompressed_.assign(fft_.bins(), 0.0f);
}

void SpectralFluxOnset::reset() noexcept {
    std::fill(frame_.begin(), frame_.end(), 0.0f);
    std::fill(prevCompressed_.begin(), prevCompressed_.end(), 0.0f);
    fill_ = 0;
    skip_ = 0;
    frameIndex_ = 0;
    histStart_ = 0;
    histSize_ = 0;
    prevNovelty_ = 0.0;
    prevPrevNovelty_ = 0.0;
    prevThreshold_ = 0.0;
    lastOnsetFrame_ = 0;
    hasLastOnset_ = false;
}

double SpectralFluxOnset::historyMedian() noexcept {
    if (histSize_ == 0)
        return 0.0;
    // Fixed-size scratch, insertion sort (the median uses a
    // fixed-size buffer with an insertion sort). M <= 63, so this is a
    // bounded, allocation-free handful of operations per frame.
    std::array<double, kMaxMedianFrames> sorted{};
    for (std::size_t i = 0; i < histSize_; ++i)
        sorted[i] = history_[(histStart_ + i) % config_.medianFrames];
    for (std::size_t i = 1; i < histSize_; ++i) {
        const double value = sorted[i];
        std::size_t j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    const std::size_t mid = histSize_ / 2;
    if (histSize_ % 2 == 1)
        return sorted[mid];
    return 0.5 * (sorted[mid - 1] + sorted[mid]); // even count: mean of the middle pair
}

bool SpectralFluxOnset::processFrame(float& noveltyOut, OnsetEvent& eventOut) noexcept {
    const std::size_t bins = fft_.bins();
    for (std::size_t i = 0; i < config_.fftSize; ++i)
        windowed_[i] = frame_[i] * window_[i];
    fft_.magnitudeSpectrum(windowed_.data(), magnitudes_.data());

    // Novelty: mean half-wave-rectified increase of the log-compressed
    // magnitudes, nu_t = (1/K) * sum_k max(0, C_k(t) - C_k(t-1)). Only
    // increases count, so steady or decaying spectra give nu = 0.
    const double gamma = static_cast<double>(config_.compression);
    double sum = 0.0;
    for (std::size_t k = 0; k < bins; ++k) {
        const double compressed = std::log(1.0 + gamma * static_cast<double>(magnitudes_[k]));
        const double increase = compressed - static_cast<double>(prevCompressed_[k]);
        if (increase > 0.0)
            sum += increase;
        prevCompressed_[k] = static_cast<float>(compressed);
    }
    const double novelty = sum / static_cast<double>(bins);
    noveltyOut = static_cast<float>(novelty);

    // Threshold for THIS frame, from the M novelty values before it (the
    // history does not contain nu_t yet): theta_t = delta + lambda * median.
    const double threshold = static_cast<double>(delta_) + static_cast<double>(lambda_) * historyMedian();

    // Peak picking with one frame of look-ahead: frame t-1 is confirmed now
    // that nu_t is known. The left neighbour of frame 0 is the silence
    // before the stream, nu = 0, consistent with C_k(-1) = 0.
    bool fired = false;
    if (frameIndex_ >= 1) {
        const std::uint64_t candidate = frameIndex_ - 1;
        const double left = candidate == 0 ? 0.0 : prevPrevNovelty_;
        if (prevNovelty_ > prevThreshold_ && prevNovelty_ >= left && prevNovelty_ > novelty &&
            (!hasLastOnset_ ||
             static_cast<double>(candidate - lastOnsetFrame_) >= minIntervalFrames_)) {
            eventOut.frameIndex = candidate;
            // Per the maintainer ruling on PR #26 (2026-11-05), timeSeconds
            // is the frame CENTRE, not the frame start: a click is only
            // detected once it is well inside the frame, so the start trails
            // it by close to a whole window — in samples fixed, in time
            // sample-rate dependent. The centre roughly halves the bias and
            // keeps the +/-20 ms criterion meaningful at any sample rate.
            eventOut.timeSeconds = (static_cast<double>(candidate) * static_cast<double>(config_.hop) +
                                    static_cast<double>(config_.fftSize) / 2.0) /
                                   sampleRate_;
            eventOut.strength = static_cast<float>(prevNovelty_);
            lastOnsetFrame_ = candidate;
            hasLastOnset_ = true;
            fired = true;
        }
    }

    // Shift the peak-picking state and record nu_t in the median history.
    prevPrevNovelty_ = prevNovelty_;
    prevNovelty_ = novelty;
    prevThreshold_ = threshold;
    if (histSize_ < config_.medianFrames) {
        history_[(histStart_ + histSize_) % config_.medianFrames] = novelty;
        ++histSize_;
    } else {
        history_[histStart_] = novelty;
        histStart_ = (histStart_ + 1) % config_.medianFrames;
    }
    return fired;
}

void SpectralFluxOnset::advanceFrame() noexcept {
    if (config_.hop < config_.fftSize) {
        // The tail of this frame is the head of the next one.
        const std::size_t keep = config_.fftSize - config_.hop;
        std::memmove(frame_.data(), frame_.data() + config_.hop, keep * sizeof(float));
        fill_ = keep;
    } else {
        fill_ = 0;
        skip_ = config_.hop - config_.fftSize;
    }
}

} // namespace dsptb::dsp
