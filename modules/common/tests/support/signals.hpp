#pragma once

// Test signals.
//
// These exact formulas are shared by the C++ tests and the Python goldens, so
// both sides produce bit-for-bit identical inputs. All math is done in
// double, then cast to float. `n` is the sample index and `fs` the sample
// rate in Hz.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb::test::signals {

inline constexpr double kPi = 3.14159265358979323846;

// sine(f, amp, phaseDeg): amp * sin(2*pi*f*n/fs + phase)
inline float sine(std::size_t n, double fs, double f, double amp, double phaseDeg) noexcept {
    const double phase = 2.0 * kPi * f * static_cast<double>(n) / fs + phaseDeg * kPi / 180.0;
    return static_cast<float>(amp * std::sin(phase));
}

// lcgNoise(seed, amp): s <- (1664525*s + 1013904223) mod 2^32, then
// x = amp * (2*s/2^32 - 1). The state is updated before each sample.
// (Numerical Recipes LCG constants.)
class LcgNoise {
public:
    LcgNoise(std::uint32_t seed, double amp) noexcept : state_(seed), amp_(amp) {}

    float next() noexcept {
        // Unsigned 32-bit wraparound is exactly "mod 2^32".
        state_ = 1664525u * state_ + 1013904223u;
        const double unit = static_cast<double>(state_) / 4294967296.0; // s / 2^32
        return static_cast<float>(amp_ * (2.0 * unit - 1.0));
    }

    void reset(std::uint32_t seed) noexcept { state_ = seed; }
    std::uint32_t state() const noexcept { return state_; }

private:
    std::uint32_t state_;
    double amp_;
};

// sweepLog(f0, f1, T, amp):
//   amp * sin(2*pi*f0*T/ln(f1/f0) * (exp(n/fs * ln(f1/f0)/T) - 1))
// T is the sweep duration in seconds, n/fs the time of sample n.
inline float sweepLog(std::size_t n, double fs, double f0, double f1, double T, double amp) noexcept {
    const double t = static_cast<double>(n) / fs;
    const double logRatio = std::log(f1 / f0);
    const double phase = 2.0 * kPi * f0 * T / logRatio * (std::exp(t * logRatio / T) - 1.0);
    return static_cast<float>(amp * std::sin(phase));
}

// clickTrain(bpm, amp): a single sample of amp at n = round(k*60/bpm*fs)
// for k = 0, 1, 2, ..., zero elsewhere.
inline float clickTrain(std::size_t n, double fs, double bpm, double amp) noexcept {
    const double periodSamples = 60.0 / bpm * fs;
    const double k = std::round(static_cast<double>(n) / periodSamples);
    const std::size_t clickIndex = static_cast<std::size_t>(std::round(k * periodSamples));
    return n == clickIndex ? static_cast<float>(amp) : 0.0f;
}

// saw(f, amp): naive amp * (2*frac(f*n/fs) - 1)
inline float saw(std::size_t n, double fs, double f, double amp) noexcept {
    const double cycles = f * static_cast<double>(n) / fs;
    const double frac = cycles - std::floor(cycles);
    return static_cast<float>(amp * (2.0 * frac - 1.0));
}

// goldenSignal(fs): 1 s of
//   0.5*sin(2*pi*440*n/fs) + 0.25*sin(2*pi*1250*n/fs) + lcgNoise(seed=1, amp=0.05)
// This is the input for all librosa goldens.
inline std::vector<float> goldenSignal(double fs) {
    const std::size_t count = static_cast<std::size_t>(fs);
    std::vector<float> out;
    out.reserve(count);
    LcgNoise noise(1u, 0.05);
    for (std::size_t n = 0; n < count; ++n) {
        const double value = static_cast<double>(sine(n, fs, 440.0, 0.5, 0.0)) +
                             static_cast<double>(sine(n, fs, 1250.0, 0.25, 0.0)) +
                             static_cast<double>(noise.next());
        out.push_back(static_cast<float>(value));
    }
    return out;
}

} // namespace dsptb::test::signals
