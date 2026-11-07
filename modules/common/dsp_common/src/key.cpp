// KeyDetector implementation.
//
// Method: Krumhansl–Schmuckler key-finding — accumulate a chroma vector
// over the whole stream, then take the key whose Krumhansl–Kessler
// profile has the highest Pearson correlation with the chroma.
// C. Krumhansl, "Cognitive Foundations of Musical Pitch" (1990);
// S. Pauws, "Musical Key Extraction from Audio", ISMIR 2004.

#include <dsptb/dsp_common/key.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

// Krumhansl & Kessler (1982) key profiles, index 0 = tonic.
constexpr std::array<double, 12> kMajorProfile = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                                  2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                                  2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

// Peak floor: a spectral peak whose
// magnitude is below this fraction of the frame's largest peak
// (1% = -40 dB) contributes nothing, so Hann sidelobes and noise-floor
// ripple do not add chroma.
constexpr double kPeakFloorRatio = 0.01;

// Pearson correlation of the chroma with `profile` rotated so its tonic
// (index 0) sits on pitch class `root`: the profile value for pitch
// class pc is profile[(pc - root) mod 12]. Returns 0 when either side
// has zero variance (e.g. a perfectly flat chroma), where r is
// undefined — such input matches no key better than any other.
double pearson(const std::array<double, 12>& chroma, const std::array<double, 12>& profile,
               int root) noexcept {
    double meanX = 0.0;
    double meanY = 0.0;
    for (std::size_t pc = 0; pc < 12; ++pc) {
        meanX += chroma[pc];
        meanY += profile[(pc + 12 - static_cast<std::size_t>(root)) % 12];
    }
    meanX /= 12.0;
    meanY /= 12.0;
    double sxy = 0.0;
    double sxx = 0.0;
    double syy = 0.0;
    for (std::size_t pc = 0; pc < 12; ++pc) {
        const double dx = chroma[pc] - meanX;
        const double dy = profile[(pc + 12 - static_cast<std::size_t>(root)) % 12] - meanY;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }
    if (sxx <= 0.0 || syy <= 0.0)
        return 0.0;
    return sxy / std::sqrt(sxx * syy);
}

} // namespace

namespace detail {

std::size_t extractFramePeaks(const float* power, std::size_t bins, double binHz, double minHz,
                              double maxHz, double* magnitudes, FramePeak* peaks,
                              std::size_t peakCapacity) noexcept {
    if (bins < 3 || peakCapacity == 0)
        return 0;
    for (std::size_t k = 0; k < bins; ++k)
        magnitudes[k] = std::sqrt(static_cast<double>(power[k]));

    // First pass: the frame's largest peak (the floor reference).
    double largest = 0.0;
    for (std::size_t k = 1; k + 1 < bins; ++k) {
        const double sCur = magnitudes[k];
        if (sCur > magnitudes[k - 1] && sCur > magnitudes[k + 1])
            largest = std::max(largest, sCur);
    }
    if (largest <= 0.0)
        return 0; // silent frame: no peaks
    const double floor = kPeakFloorRatio * largest;

    // Second pass: refine each surviving peak. The DC and Nyquist bins
    // have no both-neighbours and never contribute.
    std::size_t count = 0;
    for (std::size_t k = 1; k + 1 < bins; ++k) {
        const double sPrev = magnitudes[k - 1];
        const double sCur = magnitudes[k];
        const double sNext = magnitudes[k + 1];
        if (sCur > sPrev && sCur > sNext) {
            // Parabolic interpolation on log-magnitudes
            // a = log S[k-1], b = log S[k], c = log S[k+1]: vertex
            // offset delta = 0.5(a-c)/(a-2b+c), vertex log-magnitude
            // b - 0.25(a-c)*delta. For a strict local maximum the
            // denominator is strictly negative; a zero neighbour (log
            // undefined) or a degenerate denominator falls back to the
            // bin centre with the raw magnitude.
            double delta = 0.0;
            double peakMagnitude = sCur;
            if (sPrev > 0.0 && sNext > 0.0) {
                const double a = std::log(sPrev);
                const double b = std::log(sCur);
                const double c = std::log(sNext);
                const double denom = a - 2.0 * b + c;
                if (denom < 0.0) {
                    delta = 0.5 * (a - c) / denom;
                    delta = std::clamp(delta, -0.5, 0.5);
                    peakMagnitude = std::exp(b - 0.25 * (a - c) * delta);
                }
            }
            if (peakMagnitude >= floor) {
                const double f = (static_cast<double>(k) + delta) * binHz;
                if (f >= minHz && f <= maxHz && count < peakCapacity) {
                    peaks[count] = FramePeak{f, peakMagnitude};
                    ++count;
                }
            }
        }
    }
    return count;
}

} // namespace detail

StftConfig KeyDetector::effectiveStftConfig(double sampleRate, const KeyConfig& config) {
    // Validate the configured values as given, before scaling
    //.
    if (config.fftSize < 32 || config.fftSize > 65536 ||
        (config.fftSize & (config.fftSize - 1)) != 0)
        throw std::invalid_argument("KeyDetector: fftSize must be a power of two in [32, 65536]");
    if (config.hop == 0)
        throw std::invalid_argument("KeyDetector: hop must be at least 1");

    // Scale by s = 2^round(log2(sampleRate / 48000)), with exact integer
    // arithmetic once the exponent is known: shifting left multiplies,
    // shifting right divides. The configured defaults (8192/4096)
    // divide cleanly; any other value truncates toward zero when scaled
    // down, and a configured hop smaller than the downscale factor
    // scales to 0, which the StftAnalyzer rejects. The exponent is
    // clamped to [-20, 20] — far outside any plausible audio rate —
    // only to keep the shifts defined for absurd inputs; an invalid
    // sample rate falls back to no scaling and is reported by the
    // constructor's own sample-rate check below.
    int exponent = 0;
    if (sampleRate > 0.0 && std::isfinite(sampleRate)) {
        const double e = std::round(std::log2(sampleRate / 48000.0));
        exponent = static_cast<int>(std::clamp(e, -20.0, 20.0));
    }
    std::size_t fftSize = config.fftSize;
    std::size_t hop = config.hop;
    if (exponent >= 0) {
        fftSize <<= static_cast<std::size_t>(exponent);
        hop <<= static_cast<std::size_t>(exponent);
    } else {
        fftSize >>= static_cast<std::size_t>(-exponent);
        hop >>= static_cast<std::size_t>(-exponent);
    }
    return StftConfig{fftSize, hop, Window::Hann};
}

KeyDetector::KeyDetector(double sampleRate, const KeyConfig& config)
    : sampleRate_(sampleRate), config_(config),
      effective_(effectiveStftConfig(sampleRate, config)), stft_(effective_) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("KeyDetector: sample rate must be positive");
    if (!(config.minHz > 0.0) || !(config.minHz < config.maxHz))
        throw std::invalid_argument("KeyDetector: need 0 < minHz < maxHz");
    magnitudes_.assign(stft_.bins(), 0.0);
    peaks_.assign(stft_.bins() / 2 + 1, detail::FramePeak{});
}

void KeyDetector::reset() noexcept {
    stft_.reset();
    chroma_.fill(0.0);
    frames_ = 0;
}

void KeyDetector::push(const float* mono, std::size_t n) noexcept {
    // pushPower hands over the raw power spectrum |X_k|^2 of each
    // Hann-windowed frame. The chroma accumulates the frame's spectral
    // peaks in double precision (peak-aware chroma, see
    // detail::extractFramePeaks), each
    // folded in by the pitch-class formula applied to its interpolated
    // frequency:
    //   pc = (round(12 * log2(f / 440)) + 9) mod 12   (so C = 0, A = 9)
    // All scratch is constructor-sized — no allocation.
    const double binHz = sampleRate_ / static_cast<double>(effective_.fftSize);
    stft_.pushPower(
        mono, n,
        [this, binHz](const float* power, std::size_t bins, std::uint64_t /*frameIndex*/) noexcept {
            ++frames_;
            const std::size_t count = detail::extractFramePeaks(
                power, bins, binHz, config_.minHz, config_.maxHz, magnitudes_.data(), peaks_.data(),
                peaks_.size());
            for (std::size_t i = 0; i < count; ++i) {
                const long steps = std::lround(12.0 * std::log2(peaks_[i].frequencyHz / 440.0));
                long pc = (steps + 9) % 12;
                if (pc < 0)
                    pc += 12;
                chroma_[static_cast<std::size_t>(pc)] += peaks_[i].magnitude;
            }
        });
}

void KeyDetector::chroma(float out[12]) const noexcept {
    double total = 0.0;
    for (const double value : chroma_)
        total += value;
    if (total <= 0.0) {
        for (std::size_t pc = 0; pc < 12; ++pc)
            out[pc] = 0.0f;
        return;
    }
    for (std::size_t pc = 0; pc < 12; ++pc)
        out[pc] = static_cast<float>(chroma_[pc] / total);
}

KeyResult KeyDetector::result() const noexcept {
    KeyResult out;
    double total = 0.0;
    for (const double value : chroma_)
        total += value;
    // valid = false when fewer than 2 frames were analysed
    // or the total chroma energy is below 1e-6.
    out.valid = frames_ >= 2 && total >= 1e-6;
    if (!out.valid)
        return out;

    // Score all 24 profiles; the highest Pearson r wins. Iteration is
    // in a fixed order (root 0..11, major before minor) and only a
    // strictly greater r replaces the winner, so ties resolve
    // deterministically.
    double rMajor[12];
    double rMinor[12];
    int bestRoot = 0;
    KeyMode bestMode = KeyMode::Major;
    double bestR = -2.0;
    for (int root = 0; root < 12; ++root) {
        rMajor[root] = pearson(chroma_, kMajorProfile, root);
        rMinor[root] = pearson(chroma_, kMinorProfile, root);
        if (rMajor[root] > bestR) {
            bestR = rMajor[root];
            bestRoot = root;
            bestMode = KeyMode::Major;
        }
        if (rMinor[root] > bestR) {
            bestR = rMinor[root];
            bestRoot = root;
            bestMode = KeyMode::Minor;
        }
    }

    // Margin: relative keys share their notes, so confusing
    // them does not matter for tuning — the margin is the winner's r
    // minus the best other r EXCLUDING the winner's relative key
    // (major root r -> minor (r + 9) mod 12; minor root r -> major
    // (r + 3) mod 12).
    const int relativeRoot = bestMode == KeyMode::Major ? (bestRoot + 9) % 12 : (bestRoot + 3) % 12;
    double bestOther = -2.0;
    for (int root = 0; root < 12; ++root) {
        if (!(root == bestRoot && bestMode == KeyMode::Major) &&
            !(root == relativeRoot && bestMode == KeyMode::Minor))
            bestOther = std::max(bestOther, rMajor[root]);
        if (!(root == bestRoot && bestMode == KeyMode::Minor) &&
            !(root == relativeRoot && bestMode == KeyMode::Major))
            bestOther = std::max(bestOther, rMinor[root]);
    }
    const double margin = bestR - bestOther;

    out.root = bestRoot;
    out.mode = bestMode;
    out.correlation = static_cast<float>(bestR);
    out.margin = static_cast<float>(margin);
    out.confident = bestR >= 0.6 && margin >= 0.05;
    return out;
}

} // namespace dsptb::dsp
