#include <dsptb/dsp_common/stft.hpp>

#include <algorithm>
#include <dsptb/dsp/window.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace dsptb::dsp {
namespace {

void validateHopAndWindow(const StftConfig& config) {
    if (config.hop == 0)
        throw std::invalid_argument("StftAnalyzer: hop must be at least 1");
    if (config.window != Window::Hann && config.window != Window::BlackmanHarris)
        throw std::invalid_argument("StftAnalyzer: unknown window");
}

// All of the analyzer's working buffers for one config, built off to the
// side so configure() can commit them with only noexcept moves.
struct Buffers {
    std::vector<float> window;
    std::vector<float> frame;
    std::vector<float> windowed;
    std::vector<float> scratch;
    double windowSum = 0.0;
};

Buffers makeBuffers(const StftConfig& config) {
    Buffers buffers;
    buffers.window.assign(config.fftSize, 0.0f);
    if (config.window == Window::Hann)
        hannWindow(buffers.window.data(), config.fftSize);
    else
        blackmanHarrisWindow(buffers.window.data(), config.fftSize);
    double sum = 0.0;
    for (const float w : buffers.window)
        sum += static_cast<double>(w);
    buffers.windowSum = sum;
    buffers.frame.assign(config.fftSize, 0.0f);
    buffers.windowed.assign(config.fftSize, 0.0f);
    buffers.scratch.assign(config.fftSize / 2 + 1, 0.0f);
    return buffers;
}

} // namespace

StftAnalyzer::StftAnalyzer(const StftConfig& config) : config_(config), fft_(config.fftSize) {
    validateHopAndWindow(config);
    buildBuffers();
}

void StftAnalyzer::configure(const StftConfig& config) {
    validateHopAndWindow(config);
    // Build everything for the new config first — the FFT and all buffers.
    // If anything throws (invalid size, bad_alloc), no member has changed
    // and the previous configuration stays fully in place. The commit below
    // is moves and scalar assignments only, hence noexcept.
    RealFft newFft(config.fftSize);
    Buffers buffers = makeBuffers(config);
    fft_ = std::move(newFft);
    config_ = config;
    window_ = std::move(buffers.window);
    frame_ = std::move(buffers.frame);
    windowed_ = std::move(buffers.windowed);
    scratch_ = std::move(buffers.scratch);
    windowSum_ = buffers.windowSum;
    reset();
}

void StftAnalyzer::buildBuffers() {
    Buffers buffers = makeBuffers(config_);
    window_ = std::move(buffers.window);
    frame_ = std::move(buffers.frame);
    windowed_ = std::move(buffers.windowed);
    scratch_ = std::move(buffers.scratch);
    windowSum_ = buffers.windowSum;
}

void StftAnalyzer::reset() noexcept {
    fill_ = 0;
    skip_ = 0;
    frameIndex_ = 0;
}

const float* StftAnalyzer::analyzeFrame(bool power) noexcept {
    for (std::size_t i = 0; i < config_.fftSize; ++i)
        windowed_[i] = frame_[i] * window_[i];
    if (power) {
        fft_.powerSpectrum(windowed_.data(), scratch_.data());
    } else {
        fft_.magnitudeSpectrum(windowed_.data(), scratch_.data());
        // Amplitude correction: a sine of amplitude A on a bin has
        // |X_k| = A * sum(w) / 2, so 2/sum(w) makes it read as A (0 dBFS
        // for A = 1). The 1e-12 floor keeps silence finite instead of -inf.
        const float scale = static_cast<float>(2.0 / windowSum_);
        for (std::size_t k = 0; k < scratch_.size(); ++k)
            scratch_[k] = static_cast<float>(20.0 * std::log10(scratch_[k] * scale + 1e-12));
    }
    return scratch_.data();
}

void StftAnalyzer::advanceFrame() noexcept {
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

void mixToMono(const float* l, const float* r, std::size_t n, ChannelMix mix, float* out) noexcept {
    switch (mix) {
    case ChannelMix::Left:
        for (std::size_t i = 0; i < n; ++i)
            out[i] = l[i];
        break;
    case ChannelMix::Right:
        for (std::size_t i = 0; i < n; ++i)
            out[i] = r[i];
        break;
    case ChannelMix::Mid:
        for (std::size_t i = 0; i < n; ++i)
            out[i] = 0.5f * (l[i] + r[i]);
        break;
    case ChannelMix::Side:
        for (std::size_t i = 0; i < n; ++i)
            out[i] = 0.5f * (l[i] - r[i]);
        break;
    }
}

std::size_t fftSizeForRate(double sampleRate, std::size_t sizeAt48k) noexcept {
    int exponent = 0;
    if (sampleRate > 0.0 && std::isfinite(sampleRate))
        exponent = static_cast<int>(std::clamp(std::round(std::log2(sampleRate / 48000.0)), -20.0, 20.0));
    const std::size_t scaled = exponent >= 0 ? sizeAt48k << exponent : sizeAt48k >> -exponent;
    return std::max<std::size_t>(scaled, 256);
}

} // namespace dsptb::dsp
