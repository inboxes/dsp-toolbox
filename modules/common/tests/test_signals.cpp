#include <doctest/doctest.h>

#include "support/signals.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>

using namespace dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr double kTestPi = 3.14159265358979323846;

double fracPart(double x) { return x - std::floor(x); }

} // namespace

TEST_CASE("signals: sine matches amp*sin(2*pi*f*n/fs + phase)") {
    // n = 0, phase 0 -> 0; phase 90 degrees -> amp.
    CHECK(sine(0, kFs, 440.0, 0.5, 0.0) == doctest::Approx(0.0));
    CHECK(sine(0, kFs, 440.0, 0.5, 90.0) == doctest::Approx(0.5));
    // Quarter of a 480 Hz period at 48 kHz is exactly 25 samples.
    CHECK(sine(25, kFs, 480.0, 0.8, 0.0) == doctest::Approx(0.8));
    CHECK(sine(50, kFs, 480.0, 0.8, 0.0) == doctest::Approx(0.0).epsilon(0.001));
    for (std::size_t n : {3u, 17u, 123u, 1000u}) {
        const double expected = 0.7 * std::sin(2.0 * kTestPi * 1000.0 * static_cast<double>(n) / kFs +
                                               30.0 * kTestPi / 180.0);
        CHECK(sine(n, kFs, 1000.0, 0.7, 30.0) == doctest::Approx(expected));
    }
}

TEST_CASE("signals: lcgNoise updates the state before each sample") {
    // Independently recompute the spec recurrence for seed 1.
    std::uint32_t state = 1u;
    LcgNoise noise(1u, 0.05);
    for (int i = 0; i < 8; ++i) {
        state = 1664525u * state + 1013904223u;
        const double expected = 0.05 * (2.0 * (static_cast<double>(state) / 4294967296.0) - 1.0);
        CHECK(noise.next() == doctest::Approx(expected));
        CHECK(noise.state() == state);
    }
    // A different seed gives a different (deterministic) sequence.
    LcgNoise other(2u, 0.05);
    std::uint32_t otherState = 1664525u * 2u + 1013904223u;
    const double expectedOther = 0.05 * (2.0 * (static_cast<double>(otherState) / 4294967296.0) - 1.0);
    CHECK(other.next() == doctest::Approx(expectedOther));
}

TEST_CASE("signals: sweepLog matches its formula at sample points") {
    const double f0 = 20.0;
    const double f1 = 20000.0;
    const double duration = 1.0;
    CHECK(sweepLog(0, kFs, f0, f1, duration, 0.9) == doctest::Approx(0.0));
    for (std::size_t n : {10u, 999u, 12000u, 47999u}) {
        const double t = static_cast<double>(n) / kFs;
        const double logRatio = std::log(f1 / f0);
        const double expected = 0.9 * std::sin(2.0 * kTestPi * f0 * duration / logRatio *
                                               (std::exp(t * logRatio / duration) - 1.0));
        CHECK(sweepLog(n, kFs, f0, f1, duration, 0.9) == doctest::Approx(expected));
    }
}

TEST_CASE("signals: clickTrain puts a single amp sample on each beat") {
    // 60 bpm at 48 kHz: one click every 48000 samples, starting at n = 0.
    CHECK(clickTrain(0, kFs, 60.0, 0.75) == doctest::Approx(0.75));
    CHECK(clickTrain(1, kFs, 60.0, 0.75) == doctest::Approx(0.0));
    CHECK(clickTrain(47999, kFs, 60.0, 0.75) == doctest::Approx(0.0));
    CHECK(clickTrain(48000, kFs, 60.0, 0.75) == doctest::Approx(0.75));
    // 120 bpm: every 24000 samples.
    CHECK(clickTrain(24000, kFs, 120.0, 0.5) == doctest::Approx(0.5));
    CHECK(clickTrain(24001, kFs, 120.0, 0.5) == doctest::Approx(0.0));
    // 110 bpm: the period is 26181.81... samples, not an integer, so clicks
    // land on rounded positions (0, 26182, 52364, ...) with no fixed stride.
    CHECK(clickTrain(0, kFs, 110.0, 0.5) == doctest::Approx(0.5));
    CHECK(clickTrain(26181, kFs, 110.0, 0.5) == doctest::Approx(0.0));
    CHECK(clickTrain(26182, kFs, 110.0, 0.5) == doctest::Approx(0.5));
    CHECK(clickTrain(26183, kFs, 110.0, 0.5) == doctest::Approx(0.0));
    CHECK(clickTrain(52364, kFs, 110.0, 0.5) == doctest::Approx(0.5));
}

TEST_CASE("signals: saw matches amp*(2*frac(f*n/fs) - 1)") {
    CHECK(saw(0, kFs, 480.0, 1.0) == doctest::Approx(-1.0));
    for (std::size_t n : {1u, 25u, 33u, 250u}) {
        const double expected = 2.0 * fracPart(440.0 * static_cast<double>(n) / kFs) - 1.0;
        CHECK(saw(n, kFs, 440.0, 1.0) == doctest::Approx(expected));
    }
    // A quarter period into a 480 Hz saw: frac = 0.25 -> -0.5 * amp.
    CHECK(saw(25, kFs, 480.0, 0.6) == doctest::Approx(-0.3));
}

TEST_CASE("signals: goldenSignal is the spec mixture for exactly one second") {
    const auto signal = goldenSignal(kFs);
    REQUIRE(signal.size() == 48000u);

    // Walk a single running noise generator and check at the chosen indices.
    const std::size_t checkPoints[] = {0u, 1u, 100u, 5000u, 47999u};
    LcgNoise noise(1u, 0.05);
    std::size_t nextCheck = 0;
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const float noiseValue = noise.next();
        if (nextCheck >= std::size(checkPoints) || n != checkPoints[nextCheck])
            continue;
        ++nextCheck;
        const double expected = static_cast<double>(sine(n, kFs, 440.0, 0.5, 0.0)) +
                                static_cast<double>(sine(n, kFs, 1250.0, 0.25, 0.0)) +
                                static_cast<double>(noiseValue);
        CHECK(signal[n] == doctest::Approx(expected));
    }
    CHECK(nextCheck == std::size(checkPoints));
}
