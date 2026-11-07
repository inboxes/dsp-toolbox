#include <dsptb/dsp_common/pitch.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

// Smallest power of two that is >= 2*frameSize (and at least RealFft's
// minimum size), capped so an absurd frameSize is reported by RealFft's
// own range check instead of overflowing here.
std::size_t fftSizeFor(std::size_t frameSize) noexcept {
    std::size_t n = 32;
    while (n < 131072 && n / 2 < frameSize)
        n *= 2;
    return n;
}

} // namespace

YinPitch::YinPitch(double sampleRate, const PitchConfig& config)
    : sampleRate_(sampleRate), config_(config), fft_(fftSizeFor(config.frameSize)) {
    const std::size_t frameSize = config_.frameSize;
    const std::size_t w = frameSize / 2;
    if (!std::isfinite(sampleRate_) || sampleRate_ <= 0.0)
        throw std::invalid_argument("YinPitch: sample rate must be positive and finite");
    if (frameSize < 2)
        throw std::invalid_argument("YinPitch: frameSize must be at least 2");
    if (config_.hop == 0)
        throw std::invalid_argument("YinPitch: hop must be at least 1");
    if (!std::isfinite(config_.minHz) || config_.minHz <= 0.0)
        throw std::invalid_argument("YinPitch: minHz must be positive and finite");
    if (!std::isfinite(config_.maxHz) || config_.maxHz <= 0.0)
        throw std::invalid_argument("YinPitch: maxHz must be positive and finite");
    // the search runs to ceil(fs/minHz), which must fit the
    // integration window W = frameSize/2.
    if (sampleRate_ / config_.minHz > static_cast<double>(w))
        throw std::invalid_argument("YinPitch: sampleRate/minHz exceeds frameSize/2");

    windowW_ = w;
    // The bounds above pin fs/maxHz < fs/minHz <= W, so both casts are in
    // range. A lag of 0 is meaningless (the CMND is 1 there by
    // definition), so a maxHz above the sample rate clamps to lag 1.
    tauMax_ = std::min(w, static_cast<std::size_t>(std::ceil(sampleRate_ / config_.minHz)));
    const double tauMinReal = std::floor(sampleRate_ / config_.maxHz);
    tauMin_ = tauMinReal < 1.0 ? 1 : static_cast<std::size_t>(tauMinReal);
    if (tauMin_ > tauMax_)
        throw std::invalid_argument("YinPitch: empty lag search range (check minHz/maxHz)");

    const std::size_t fftSize = fft_.size();
    frameBuf_.assign(frameSize, 0.0f);
    paddedFrame_.assign(fftSize, 0.0f);
    paddedWindow_.assign(fftSize, 0.0f);
    spectrumFrame_.assign(fft_.bins(), std::complex<float>{});
    spectrumWindow_.assign(fft_.bins(), std::complex<float>{});
    corr_.assign(fftSize, 0.0f);
    cumSq_.assign(frameSize + 1, 0.0);
    cmnd_.assign(tauMax_ + 1, 1.0);
}

void YinPitch::reset() noexcept {
    fill_ = 0;
    skip_ = 0;
    frameIndex_ = 0;
}

void YinPitch::advanceFrame() noexcept {
    if (config_.hop >= config_.frameSize) {
        skip_ = config_.hop - config_.frameSize;
        fill_ = 0;
        return;
    }
    const std::size_t keep = config_.frameSize - config_.hop;
    std::memmove(frameBuf_.data(), frameBuf_.data() + config_.hop, keep * sizeof(float));
    fill_ = keep;
}

PitchResult YinPitch::analyzeFrame(const float* frame) noexcept {
    const std::size_t frameSize = config_.frameSize;
    const std::size_t w = windowW_;

    // Cross-correlation r(tau) = sum_{j<W} x_j * x_{j+tau} via FFT: with
    // X the transform of the zero-padded frame and Y the transform of its
    // first W samples (zero-padded), IFFT(X * conj(Y)) at lag tau is
    // exactly that sum for tau <= W (no wraparound: j + tau < 2W <= FFT
    // size). RealFft's inverse supplies the 1/N scaling.
    std::fill(paddedFrame_.begin(), paddedFrame_.end(), 0.0f);
    std::copy(frame, frame + frameSize, paddedFrame_.begin());
    std::fill(paddedWindow_.begin(), paddedWindow_.end(), 0.0f);
    std::copy(frame, frame + w, paddedWindow_.begin());
    fft_.forward(paddedFrame_.data(), spectrumFrame_.data());
    fft_.forward(paddedWindow_.data(), spectrumWindow_.data());
    for (std::size_t k = 0; k < spectrumFrame_.size(); ++k)
        spectrumFrame_[k] *= std::conj(spectrumWindow_[k]);
    fft_.inverse(spectrumFrame_.data(), corr_.data());

    // Cumulative sum of x^2: the energy of the window starting at tau is
    // cumSq_[tau + W] - cumSq_[tau].
    cumSq_[0] = 0.0;
    for (std::size_t i = 0; i < frameSize; ++i) {
        const double v = static_cast<double>(frame[i]);
        cumSq_[i + 1] = cumSq_[i] + v * v;
    }

    // Difference function and its cumulative mean normalised
    // form (YIN step 2). d(tau) >= 0 mathematically; the clamp only eats
    // FFT round-off. A zero running mean means a silent (or constant)
    // frame so far: the CMND is defined as 1 there, i.e. no periodicity.
    const double headEnergy = cumSq_[w];
    double runningSum = 0.0;
    cmnd_[0] = 1.0;
    for (std::size_t tau = 1; tau <= tauMax_; ++tau) {
        double d = headEnergy + (cumSq_[tau + w] - cumSq_[tau]) - 2.0 * static_cast<double>(corr_[tau]);
        if (d < 0.0)
            d = 0.0;
        runningSum += d;
        cmnd_[tau] = runningSum > 0.0 ? d * static_cast<double>(tau) / runningSum : 1.0;
    }

    // YIN step 3 (absolute threshold): the first lag whose CMND dips
    // below the threshold, refined to the bottom of that dip. If no lag
    // qualifies, fall back to the global CMND minimum (best guess,
    // reported unvoiced).
    std::size_t tauStar = tauMin_;
    bool belowThreshold = false;
    for (std::size_t tau = tauMin_; tau <= tauMax_; ++tau) {
        if (cmnd_[tau] < config_.threshold) {
            tauStar = tau;
            belowThreshold = true;
            break;
        }
    }
    if (belowThreshold) {
        while (tauStar + 1 <= tauMax_ && cmnd_[tauStar + 1] < cmnd_[tauStar])
            ++tauStar;
    } else {
        for (std::size_t tau = tauMin_ + 1; tau <= tauMax_; ++tau) {
            if (cmnd_[tau] < cmnd_[tauStar])
                tauStar = tau;
        }
    }
    const double cmndStar = cmnd_[tauStar];

    // YIN step 5: parabolic interpolation of the CMND around tauStar for
    // a sub-sample period. Needs both neighbours, so a minimum sitting on
    // tauMax keeps its integer lag.
    double shift = 0.0;
    if (tauStar >= 1 && tauStar < tauMax_) {
        const double y1 = cmnd_[tauStar - 1];
        const double y3 = cmnd_[tauStar + 1];
        const double denom = y1 - 2.0 * cmndStar + y3;
        if (denom > 0.0) {
            shift = 0.5 * (y1 - y3) / denom;
            if (shift > 1.0)
                shift = 1.0;
            if (shift < -1.0)
                shift = -1.0;
        }
    }
    const double tauHat = static_cast<double>(tauStar) + shift;

    PitchResult result;
    result.hz = static_cast<float>(sampleRate_ / tauHat);
    double confidence = 1.0 - cmndStar;
    if (confidence < 0.0)
        confidence = 0.0;
    if (confidence > 1.0)
        confidence = 1.0;
    result.confidence = static_cast<float>(confidence);
    result.voiced = cmndStar < config_.threshold;
    result.frameIndex = frameIndex_;
    ++frameIndex_;
    return result;
}

} // namespace dsptb::dsp
