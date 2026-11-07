#pragma once

#include <cmath>
#include <cstddef>

namespace dsptb {

// Periodic windows (for spectral analysis): w[i] for i in [0, n).
inline void hannWindow(float* w, size_t n) noexcept {
    const double k = 6.283185307179586476925286766559 / static_cast<double>(n);
    for (size_t i = 0; i < n; ++i)
        w[i] = static_cast<float>(0.5 - 0.5 * std::cos(k * static_cast<double>(i)));
}

inline void blackmanHarrisWindow(float* w, size_t n) noexcept {
    const double k = 6.283185307179586476925286766559 / static_cast<double>(n);
    for (size_t i = 0; i < n; ++i) {
        const double x = k * static_cast<double>(i);
        w[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) -
                                  0.01168 * std::cos(3 * x));
    }
}

} // namespace dsptb
