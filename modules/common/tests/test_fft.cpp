// RealFft tests.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/fft.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using dsptb::dsp::RealFft;
using dsptb::test::signals::LcgNoise;

namespace {

std::vector<float> noiseSamples(std::size_t n, std::uint32_t seed, double amp) {
    std::vector<float> out;
    out.reserve(n);
    LcgNoise noise(seed, amp);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(noise.next());
    return out;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return worst;
}

} // namespace

TEST_CASE("fft: sizes 32..65536 (powers of two) are accepted") {
    for (std::size_t n = 32; n <= 65536; n *= 2) {
        RealFft fft(n);
        CHECK(fft.size() == n);
        CHECK(fft.bins() == n / 2 + 1);
    }
}

TEST_CASE("fft: other sizes throw std::invalid_argument") {
    const std::size_t bad[] = {0,  1,   2,    3,    16,   24,    31,    33,     48,
                               63, 100, 1000, 4095, 8191, 65535, 65537, 131072, 1u << 20};
    for (const std::size_t n : bad)
        CHECK_THROWS_AS((RealFft(n)), std::invalid_argument);
}

TEST_CASE("fft: round trip inverse(forward(x)) == x within 1e-5") {
    for (const std::size_t n : {64u, 1024u, 8192u}) {
        RealFft fft(n);
        const std::vector<float> x = noiseSamples(n, 12345u, 1.0);
        std::vector<std::complex<float>> spectrum(fft.bins());
        std::vector<float> back(n);
        fft.forward(x.data(), spectrum.data());
        fft.inverse(spectrum.data(), back.data());
        const double err = maxAbsDiff(x, back);
        INFO("n = ", n, ", max abs round-trip error = ", err);
        CHECK(err < 1e-5);
    }
}

TEST_CASE("fft: impulse at n = 0 gives a flat magnitude of 1.0 in every bin") {
    constexpr std::size_t n = 256;
    RealFft fft(n);
    std::vector<float> x(n, 0.0f);
    x[0] = 1.0f;
    std::vector<std::complex<float>> spectrum(fft.bins());
    fft.forward(x.data(), spectrum.data());
    for (std::size_t k = 0; k < fft.bins(); ++k) {
        CHECK(std::abs(spectrum[k]) == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(spectrum[k].imag() == doctest::Approx(0.0).epsilon(1e-5));
    }
    std::vector<float> mag(fft.bins());
    fft.magnitudeSpectrum(x.data(), mag.data());
    for (std::size_t k = 0; k < fft.bins(); ++k)
        CHECK(mag[k] == doctest::Approx(1.0).epsilon(1e-5));
}

TEST_CASE("fft: DC input of 1.0 gives |X_0| = n and ~0 elsewhere") {
    constexpr std::size_t n = 1024;
    RealFft fft(n);
    const std::vector<float> x(n, 1.0f);
    std::vector<std::complex<float>> spectrum(fft.bins());
    fft.forward(x.data(), spectrum.data());
    CHECK(std::abs(spectrum[0]) == doctest::Approx(static_cast<double>(n)).epsilon(1e-4));
    CHECK(spectrum[0].imag() == doctest::Approx(0.0));
    for (std::size_t k = 1; k < fft.bins(); ++k)
        CHECK(std::abs(spectrum[k]) < 0.01f);
}

TEST_CASE("fft: sine on exact bin k peaks at k with |X_k| = n/2") {
    constexpr std::size_t n = 1024;
    constexpr std::size_t kBin = 64;
    RealFft fft(n);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = dsptb::test::signals::sine(i, static_cast<double>(n), static_cast<double>(kBin), 1.0, 0.0);
    std::vector<std::complex<float>> spectrum(fft.bins());
    fft.forward(x.data(), spectrum.data());
    // A unit sine is (exp(+i..) - exp(-i..)) / 2i, so X_k = -i * n/2.
    CHECK(std::abs(spectrum[kBin]) == doctest::Approx(static_cast<double>(n) / 2).epsilon(1e-3));
    CHECK(spectrum[kBin].real() == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(spectrum[kBin].imag() == doctest::Approx(-static_cast<double>(n) / 2).epsilon(1e-3));
    for (std::size_t k = 0; k < fft.bins(); ++k) {
        if (k != kBin)
            CHECK(std::abs(spectrum[k]) < 0.01f);
    }
}

TEST_CASE("fft: alternating +-1 input lands only in the Nyquist bin") {
    constexpr std::size_t n = 512;
    RealFft fft(n);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    std::vector<std::complex<float>> spectrum(fft.bins());
    fft.forward(x.data(), spectrum.data());
    CHECK(spectrum[n / 2].real() == doctest::Approx(static_cast<double>(n)).epsilon(1e-4));
    CHECK(spectrum[n / 2].imag() == doctest::Approx(0.0));
    for (std::size_t k = 0; k < n / 2; ++k)
        CHECK(std::abs(spectrum[k]) < 0.01f);
}

TEST_CASE("fft: matches a naive O(n^2) DFT within 1e-3 relative at n = 256") {
    constexpr std::size_t n = 256;
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    RealFft fft(n);
    const std::vector<float> x = noiseSamples(n, 777u, 1.0);
    std::vector<std::complex<float>> spectrum(fft.bins());
    fft.forward(x.data(), spectrum.data());

    // Test-only reference DFT, in double, straight from the definition.
    double maxMag = 0.0;
    double maxErr = 0.0;
    for (std::size_t k = 0; k < fft.bins(); ++k) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double angle = -kTwoPi * static_cast<double>(k * i) / static_cast<double>(n);
            re += static_cast<double>(x[i]) * std::cos(angle);
            im += static_cast<double>(x[i]) * std::sin(angle);
        }
        maxMag = std::max(maxMag, std::hypot(re, im));
        maxErr = std::max(maxErr, std::abs(static_cast<double>(spectrum[k].real()) - re));
        maxErr = std::max(maxErr, std::abs(static_cast<double>(spectrum[k].imag()) - im));
    }
    INFO("max |X| = ", maxMag, ", max abs error vs naive DFT = ", maxErr);
    CHECK(maxErr <= 1e-3 * maxMag);
}

TEST_CASE("fft: powerSpectrum and magnitudeSpectrum are raw and consistent") {
    constexpr std::size_t n = 1024;
    RealFft fft(n);
    const std::vector<float> x(n, 1.0f); // DC: power[0] must be n^2
    std::vector<float> power(fft.bins());
    std::vector<float> mag(fft.bins());
    fft.powerSpectrum(x.data(), power.data());
    fft.magnitudeSpectrum(x.data(), mag.data());
    CHECK(power[0] == doctest::Approx(static_cast<double>(n) * static_cast<double>(n)).epsilon(1e-4));
    CHECK(mag[0] == doctest::Approx(static_cast<double>(n)).epsilon(1e-4));
    for (std::size_t k = 1; k < fft.bins(); ++k) {
        CHECK(power[k] < 1e-4f);
        CHECK(mag[k] < 0.01f);
    }

    const std::vector<float> noise = noiseSamples(n, 42u, 0.7);
    std::vector<std::complex<float>> spectrum(fft.bins());
    fft.forward(noise.data(), spectrum.data());
    fft.powerSpectrum(noise.data(), power.data());
    fft.magnitudeSpectrum(noise.data(), mag.data());
    for (std::size_t k = 0; k < fft.bins(); ++k) {
        const double expected = std::norm(spectrum[k]);
        CHECK(static_cast<double>(power[k]) == doctest::Approx(expected).epsilon(1e-4));
        CHECK(static_cast<double>(mag[k]) == doctest::Approx(std::sqrt(expected)).epsilon(1e-4));
    }
}

TEST_CASE("fft: unaligned input and output pointers are accepted") {
    constexpr std::size_t n = 128;
    RealFft fft(n);
    const std::vector<float> x = noiseSamples(n, 5u, 1.0);
    std::vector<float> paddedIn(n + 1);
    std::vector<std::complex<float>> paddedOut(fft.bins() + 1);
    std::vector<float> paddedBack(n + 1);
    std::memcpy(paddedIn.data() + 1, x.data(), n * sizeof(float));
    fft.forward(paddedIn.data() + 1, paddedOut.data() + 1);
    fft.inverse(paddedOut.data() + 1, paddedBack.data() + 1);
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(paddedBack[i + 1]) - static_cast<double>(x[i])));
    CHECK(worst < 1e-5);
}

TEST_CASE("fft: forward, powerSpectrum, magnitudeSpectrum and inverse do not allocate") {
    constexpr std::size_t n = 1024;
    RealFft fft(n);
    const std::vector<float> x = noiseSamples(n, 9u, 1.0);
    std::vector<std::complex<float>> spectrum(fft.bins());
    std::vector<float> power(fft.bins());
    std::vector<float> mag(fft.bins());
    std::vector<float> back(n);
    std::size_t allocs = 0;
    {
        dsptb::test::AllocGuard guard;
        fft.forward(x.data(), spectrum.data());
        fft.powerSpectrum(x.data(), power.data());
        fft.magnitudeSpectrum(x.data(), mag.data());
        fft.inverse(spectrum.data(), back.data());
        allocs = guard.count();
    }
    CHECK(allocs == 0);
}
