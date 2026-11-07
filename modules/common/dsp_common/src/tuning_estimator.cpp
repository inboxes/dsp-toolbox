#include <dsptb/dsp_common/tuning_estimator.hpp>

#include <dsptb/dsp_common/stft.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kHistogramBins = 100;
constexpr int kKernelHalfWidth = 5; // triangular kernel 1, 2, ... 6, ... 2, 1 over +-5 cents
constexpr double kMinSeconds = 2.0;
constexpr double kDefaultSecondsPerFrame = 4096.0 / 48000.0;

// Wraps into [-50, +50).
double wrapCents(double cents) noexcept {
    return cents - 100.0 * std::floor((cents + 50.0) / 100.0);
}

} // namespace

TuningEstimator::TuningEstimator() noexcept : secondsPerFrame_(kDefaultSecondsPerFrame) {}

TuningEstimator::TuningEstimator(double sampleRate, std::size_t hop) noexcept
    : secondsPerFrame_(sampleRate > 0.0 ? static_cast<double>(hop) / sampleRate : kDefaultSecondsPerFrame) {}

void TuningEstimator::reset() noexcept {
    histogram_.fill(0.0);
    sumRe_ = 0.0;
    sumIm_ = 0.0;
    weight_ = 0.0;
    frames_ = 0;
}

void TuningEstimator::addPeaks(const SpectralPeak* peaks, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const double midi = static_cast<double>(peaks[i].midi);
        const double magnitude = static_cast<double>(peaks[i].magnitude);
        const double dev = 100.0 * (midi - std::floor(midi + 0.5)); // [-50, +50)
        const auto bin = static_cast<std::size_t>(std::floor(dev + 50.0)) % kHistogramBins;
        histogram_[bin] += magnitude;
        const double angle = 2.0 * kPi * dev / 100.0;
        sumRe_ += magnitude * std::cos(angle);
        sumIm_ += magnitude * std::sin(angle);
        weight_ += magnitude;
    }
}

void TuningEstimator::addFrames(std::uint32_t count) noexcept {
    frames_ += count;
}

TuningEstimate TuningEstimator::result() const noexcept {
    TuningEstimate out;
    if (!(weight_ > 1e-6))
        return out;
    out.confidence = static_cast<float>(std::min(std::hypot(sumRe_, sumIm_) / weight_, 1.0));
    if (static_cast<double>(frames_) * secondsPerFrame_ < kMinSeconds)
        return out;

    std::array<double, kHistogramBins> smooth{};
    const int size = static_cast<int>(kHistogramBins);
    for (int i = 0; i < size; ++i) {
        double sum = 0.0;
        for (int d = -kKernelHalfWidth; d <= kKernelHalfWidth; ++d) {
            const int index = (i + d + size) % size;
            sum += histogram_[static_cast<std::size_t>(index)] * static_cast<double>(kKernelHalfWidth + 1 - std::abs(d));
        }
        smooth[static_cast<std::size_t>(i)] = sum;
    }
    const int top = static_cast<int>(std::max_element(smooth.begin(), smooth.end()) - smooth.begin());

    const double a = smooth[static_cast<std::size_t>((top + size - 1) % size)];
    const double b = smooth[static_cast<std::size_t>(top)];
    const double c = smooth[static_cast<std::size_t>((top + 1) % size)];
    const double denom = a - 2.0 * b + c;
    const double delta = denom < 0.0 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;

    out.valid = true;
    out.offsetCents = static_cast<float>(wrapCents(static_cast<double>(top) + 0.5 + delta - 50.0));
    out.a4Hz = 440.0 * std::pow(2.0, static_cast<double>(out.offsetCents) / 1200.0);
    out.confident = out.confidence >= 0.5f;
    return out;
}

TuningEstimate estimateTuning(const float* mono, std::size_t n, double sampleRate) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("estimateTuning: sample rate must be positive");
    const StftConfig stftConfig{fftSizeForRate(sampleRate, 8192), fftSizeForRate(sampleRate, 4096), Window::Hann};
    StftAnalyzer stft(stftConfig);
    PitchSpectrum spectrum(sampleRate, stftConfig.fftSize);
    TuningEstimator estimator(sampleRate, stftConfig.hop);
    stft.pushPower(mono, n, [&](const float* power, std::size_t, std::uint64_t) noexcept {
        spectrum.analyze(power);
        estimator.addPeaks(spectrum.peaks(), spectrum.peakCount());
        estimator.addFrames(1);
    });
    return estimator.result();
}

} // namespace dsptb::dsp
