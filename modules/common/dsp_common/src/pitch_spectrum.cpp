#include <dsptb/dsp_common/pitch_spectrum.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

PitchSpectrum::PitchSpectrum(double sampleRate, std::size_t fftSize, const PitchSpectrumConfig& config,
                             const Tuning& tuning)
    : sampleRate_(sampleRate), fftSize_(fftSize), config_(config), tuning_(tuning),
      binsPerSemitone_(config.binsPerSemitone) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("PitchSpectrum: sample rate must be positive");
    if (fftSize < 32 || (fftSize & (fftSize - 1)) != 0)
        throw std::invalid_argument("PitchSpectrum: fftSize must be a power of two of at least 32");
    if (!(config.minHz > 0.0) || !(config.minHz < config.maxHz))
        throw std::invalid_argument("PitchSpectrum: need 0 < minHz < maxHz");
    if (config.binsPerSemitone < 1 || config.binsPerSemitone > 10)
        throw std::invalid_argument("PitchSpectrum: binsPerSemitone must be in 1..10");
    if (!(config.windowSemitones > 0.0))
        throw std::invalid_argument("PitchSpectrum: windowSemitones must be positive");
    if (config.maxPeaks == 0)
        throw std::invalid_argument("PitchSpectrum: maxPeaks must be at least 1");
    if (config.harmonics < 1)
        throw std::invalid_argument("PitchSpectrum: harmonics must be at least 1");

    minMidi_ = static_cast<int>(std::floor(hzToMidi(config.minHz, tuning)));
    const int maxMidi = static_cast<int>(std::ceil(hzToMidi(config.maxHz, tuning)));
    bins_.assign(static_cast<std::size_t>(maxMidi - minMidi_) * static_cast<std::size_t>(binsPerSemitone_), 0.0f);

    // The bins whose centre lies in [minHz, maxHz], plus one guard bin on each side. Neighbours stay in
    // range because DC and Nyquist are never peaks.
    const std::size_t spectrumBins = fftSize / 2 + 1;
    const double binHz = sampleRate / static_cast<double>(fftSize);
    const double first = std::ceil(config.minHz / binHz) - 1.0;
    const double last = std::floor(config.maxHz / binHz) + 1.0;
    firstBin_ = static_cast<std::size_t>(std::max(first, 1.0));
    lastBin_ = static_cast<std::size_t>(std::min(last, static_cast<double>(spectrumBins - 2)));

    magnitudes_.assign(spectrumBins, 0.0f);
    candidates_.assign(spectrumBins / 2 + 1, Candidate{});
    peaks_.assign(config.maxPeaks, SpectralPeak{});
    harmonicShift_.reserve(static_cast<std::size_t>(config.harmonics));
    harmonicWeight_.reserve(static_cast<std::size_t>(config.harmonics));
    float weight = 1.0f;
    for (int h = 1; h <= config.harmonics; ++h) {
        harmonicShift_.push_back(12.0 * std::log2(static_cast<double>(h)));
        harmonicWeight_.push_back(weight);
        weight *= config.harmonicSlope;
    }
}

void PitchSpectrum::setTuning(const Tuning& tuning) noexcept {
    tuning_ = tuning;
}

void PitchSpectrum::findPeaks(const float* power) noexcept {
    peakCount_ = 0;
    if (firstBin_ > lastBin_)
        return;

    float largest = 0.0f;
    for (std::size_t k = firstBin_ - 1; k <= lastBin_ + 1; ++k)
        magnitudes_[k] = std::sqrt(power[k]);
    for (std::size_t k = firstBin_; k <= lastBin_; ++k)
        largest = std::max(largest, magnitudes_[k]);
    if (!(largest > 0.0f))
        return;

    const float floorValue = config_.peakFloor * largest;
    const double binHz = sampleRate_ / static_cast<double>(fftSize_);
    std::size_t count = 0;
    for (std::size_t k = firstBin_; k <= lastBin_; ++k) {
        const float cur = magnitudes_[k];
        const float prev = magnitudes_[k - 1];
        const float next = magnitudes_[k + 1];
        if (!(cur > prev && cur >= next && cur >= floorValue))
            continue;
        // A 3-point parabola on the log-magnitude. A zero neighbour has no log, so the peak stays on the
        // bin centre.
        double delta = 0.0;
        double magnitude = cur;
        if (prev > 0.0f && next > 0.0f) {
            const double a = std::log(prev);
            const double b = std::log(cur);
            const double c = std::log(next);
            const double denom = a - 2.0 * b + c;
            if (denom < 0.0) {
                delta = std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
                magnitude = std::exp(b - 0.25 * (a - c) * delta);
            }
        }
        const double hz = (static_cast<double>(k) + delta) * binHz;
        if (hz < config_.minHz || hz > config_.maxHz)
            continue;
        candidates_[count++] = Candidate{static_cast<float>(hz), static_cast<float>(magnitude)};
    }

    const std::size_t keep = std::min(count, config_.maxPeaks);
    std::partial_sort(candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(keep),
                      candidates_.begin() + static_cast<std::ptrdiff_t>(count),
                      [](const Candidate& x, const Candidate& y) { return x.magnitude > y.magnitude; });
    for (std::size_t i = 0; i < keep; ++i) {
        const Candidate& c = candidates_[i];
        peaks_[i] = SpectralPeak{c.hz, c.magnitude, static_cast<float>(hzToMidi(c.hz, tuning_))};
    }
    peakCount_ = keep;
}

void PitchSpectrum::spreadPeak(double midi, float weight) noexcept {
    const double half = config_.windowSemitones * 0.5;
    const double bps = static_cast<double>(binsPerSemitone_);
    const double low = std::ceil((midi - half - minMidi_) * bps);
    const double high = std::floor((midi + half - minMidi_) * bps);
    const double top = static_cast<double>(bins_.size()) - 1.0;
    if (high < 0.0 || low > top)
        return;
    const auto first = static_cast<std::size_t>(std::max(low, 0.0));
    const auto last = static_cast<std::size_t>(std::min(high, top));
    for (std::size_t i = first; i <= last; ++i) {
        const double distance = binMidi(i) - midi;
        if (std::abs(distance) >= half)
            continue;
        const double c = std::cos(kPi * distance / config_.windowSemitones);
        bins_[i] += weight * static_cast<float>(c * c);
    }
}

void PitchSpectrum::analyze(const float* power) noexcept {
    std::fill(bins_.begin(), bins_.end(), 0.0f);
    findPeaks(power);
    for (std::size_t p = 0; p < peakCount_; ++p) {
        const double midi = static_cast<double>(peaks_[p].midi);
        for (std::size_t h = 0; h < harmonicShift_.size(); ++h)
            spreadPeak(midi - harmonicShift_[h], peaks_[p].magnitude * harmonicWeight_[h]);
    }
    if (config_.normalizeFrame) {
        const float largest = *std::max_element(bins_.begin(), bins_.end());
        if (largest > 0.0f) {
            const float scale = 1.0f / largest;
            for (float& v : bins_)
                v *= scale;
        }
    }
}

void PitchSpectrum::foldToPitchClasses(float* out) const noexcept {
    const std::size_t classes = 12u * static_cast<std::size_t>(binsPerSemitone_);
    std::fill(out, out + classes, 0.0f);
    const std::size_t offset = static_cast<std::size_t>(minMidi_ % 12 + 12) * static_cast<std::size_t>(binsPerSemitone_);
    for (std::size_t i = 0; i < bins_.size(); ++i)
        out[(i + offset) % classes] += bins_[i];
}

} // namespace dsptb::dsp
