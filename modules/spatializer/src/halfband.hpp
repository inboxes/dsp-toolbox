#pragma once

// 2:1 sample-rate conversion for hosts running at 88.2 / 96 / 176.4 / 192 kHz. Steam Audio's built-in HRTF
// only exists for 24, 44.1 and 48 kHz, so the Spatializer runs at half (or a quarter of) such a host rate and
// converts at the edges. One stage is a linear-phase windowed-sinc FIR (79 taps, Kaiser beta 9, about 80 dB
// of alias rejection, -6 dB near 0.21 of the input rate: 20.5 kHz for a 96 kHz host), with a group delay of
// 39 samples at its own output rate.
//
// Everything is allocated in the constructors; process() never allocates.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dsptb::spat {

struct HalfbandFir {
    static constexpr int kHalf = 39;
    static constexpr int kTaps = 2 * kHalf + 1; // 79
    std::array<float, kTaps> h{};

    HalfbandFir() {
        constexpr double pi = 3.14159265358979323846;
        constexpr double fc = 0.214; // cutoff as a fraction of the stage's input rate
        constexpr double beta = 9.0;
        const auto i0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 60; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        double total = 0.0;
        std::array<double, kTaps> d{};
        for (int n = 0; n < kTaps; ++n) {
            const double m = n - kHalf;
            const double sinc = m == 0.0 ? 2.0 * fc : std::sin(2.0 * pi * fc * m) / (pi * m);
            const double r = m / kHalf;
            const double w = i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
            d[static_cast<size_t>(n)] = sinc * w;
            total += d[static_cast<size_t>(n)];
        }
        for (int n = 0; n < kTaps; ++n)
            h[static_cast<size_t>(n)] = static_cast<float>(d[static_cast<size_t>(n)] / total); // unity at DC
    }
};

// Keeps every second sample of the low-passed input. `nIn` is fixed at construction (even).
class Down2 {
public:
    explicit Down2(uint32_t nIn) : nIn_(nIn), buf_(HalfbandFir::kTaps - 1 + nIn, 0.0f) {}

    void reset() noexcept { std::fill(buf_.begin(), buf_.end(), 0.0f); }

    void process(const float* in, float* out) noexcept {
        constexpr int kHist = HalfbandFir::kTaps - 1;
        float* b = buf_.data();
        for (uint32_t i = 0; i < nIn_; ++i)
            b[kHist + i] = in[i];
        const float* h = fir().h.data();
        for (uint32_t j = 0; j < nIn_ / 2; ++j) {
            const float* x = b + kHist + 2 * j; // x[0] is the newest sample of the window
            float acc = 0.0f;
            for (int k = 0; k < HalfbandFir::kTaps; ++k)
                acc += h[k] * x[-k];
            out[j] = acc;
        }
        for (int i = 0; i < kHist; ++i) // the newest kHist samples are the history for the next block
            b[i] = b[nIn_ + i];
    }

    static const HalfbandFir& fir() {
        static const HalfbandFir f;
        return f;
    }

private:
    uint32_t nIn_;
    std::vector<float> buf_;
};

// Doubles the rate: zero-stuff and low-pass, done as two polyphase branches. Gain 2 compensates the stuffing.
class Up2 {
public:
    explicit Up2(uint32_t nIn) : nIn_(nIn), buf_(HalfbandFir::kHalf + 1 + nIn, 0.0f) {}

    void reset() noexcept { std::fill(buf_.begin(), buf_.end(), 0.0f); }

    void process(const float* in, float* out) noexcept {
        constexpr int kHist = HalfbandFir::kHalf + 1; // 40 inputs feed the even branch
        float* b = buf_.data();
        for (uint32_t i = 0; i < nIn_; ++i)
            b[kHist + i] = in[i];
        const float* h = Down2::fir().h.data();
        for (uint32_t n = 0; n < nIn_; ++n) {
            const float* x = b + kHist + n;
            float even = 0.0f, odd = 0.0f;
            for (int k = 0; k <= HalfbandFir::kHalf; ++k)
                even += h[2 * k] * x[-k];
            for (int k = 0; k < HalfbandFir::kHalf; ++k)
                odd += h[2 * k + 1] * x[-k];
            out[2 * n] = 2.0f * even;
            out[2 * n + 1] = 2.0f * odd;
        }
        for (int i = 0; i < kHist; ++i)
            b[i] = b[nIn_ + i];
    }

private:
    uint32_t nIn_;
    std::vector<float> buf_;
};

} // namespace dsptb::spat
