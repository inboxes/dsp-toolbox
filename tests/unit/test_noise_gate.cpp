#include <doctest/doctest.h>

#include <dsptb/core/noise_gate.hpp>

#include <cmath>
#include <random>
#include <vector>

using dsptb::NoiseGate;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

double toDb(double v) {
    return v > 1e-12 ? 20.0 * std::log10(v) : -240.0;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
    double acc = 0.0;
    for (size_t i = from; i < to; ++i)
        acc += static_cast<double>(x[i]) * x[i];
    return std::sqrt(acc / static_cast<double>(to - from));
}

// Background noise at -60 dBFS RMS with a 1 s speech-like tone burst (-20 dBFS) from 1.0 s to 2.0 s.
struct Scene {
    std::vector<float> l, r;
    Scene() {
        const size_t n = static_cast<size_t>(4.0 * kRate);
        std::mt19937 rng(3);
        std::normal_distribution<double> noise(0.0, std::pow(10.0, -60.0 / 20.0));
        l.resize(n);
        r.resize(n);
        for (size_t i = 0; i < n; ++i) {
            double v = noise(rng);
            const double t = static_cast<double>(i) / kRate;
            if (t >= 1.0 && t < 2.0)
                v += 0.1 * std::sin(2.0 * kPi * 220.0 * t);
            l[i] = r[i] = static_cast<float>(v);
        }
    }
};

} // namespace

TEST_CASE("noise gate: silences background noise but passes the signal") {
    Scene s;
    NoiseGate gate;
    gate.prepare(kRate);
    gate.setThresholdDb(-50.0f);
    gate.process(s.l.data(), s.r.data(), static_cast<uint32_t>(s.l.size()));

    const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
    // Before the burst: gated to (near) silence. The raw noise was -60 dBFS RMS.
    CHECK(toDb(rms(s.l, at(0.2), at(0.9))) < -120.0);
    // During the burst (after a few ms of attack): the tone is intact, about -23 dBFS RMS (0.1 peak).
    CHECK(std::fabs(toDb(rms(s.l, at(1.1), at(1.9))) - toDb(0.1 / std::sqrt(2.0))) < 0.3);
    // After the burst plus hold and release: closed again.
    CHECK(toDb(rms(s.l, at(3.2), at(3.9))) < -120.0);
    // Channels stay identical (stereo link).
    CHECK(s.l == s.r);
}

TEST_CASE("noise gate: opening and closing are smooth, never a click") {
    Scene s;
    NoiseGate gate;
    gate.prepare(kRate);
    gate.setThresholdDb(-50.0f);
    gate.process(s.l.data(), s.r.data(), static_cast<uint32_t>(s.l.size()));
    // The tone's own largest step is 0.1 * 2*sin(pi*220/48000) ~ 0.0029; gain changes must not add steps
    // beyond that.
    double maxStep = 0.0;
    for (size_t i = 1; i < s.l.size(); ++i)
        maxStep = std::max(maxStep, std::fabs(static_cast<double>(s.l[i]) - s.l[i - 1]));
    CHECK(maxStep < 0.012); // the noise at -60 dB has ~0.001 steps; the gain ramps add well under a 0.1 click
}

TEST_CASE("noise gate: a signal hovering around the threshold does not chatter (hysteresis + hold)") {
    // RMS level alternates between 1.2x and 0.8x the open threshold every 5 ms (the close threshold is 0.5x).
    const float thr = std::pow(10.0f, -50.0f / 20.0f);
    std::vector<float> l(static_cast<size_t>(2.0 * kRate)), r;
    for (size_t i = 0; i < l.size(); ++i) {
        const bool high = (i / 240) % 2 == 0;
        l[i] = (high ? 1.2f : 0.8f) * thr * 1.41421356f *
               static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kRate));
    }
    r = l;
    NoiseGate gate;
    gate.prepare(kRate);
    gate.setThresholdDb(-50.0f);
    int transitions = 0;
    bool last = false;
    for (size_t i = 0; i < l.size(); i += 64) {
        gate.process(&l[i], &r[i], 64);
        if (gate.isOpen() != last) {
            ++transitions;
            last = gate.isOpen();
        }
    }
    CHECK(transitions == 1); // opened once and stayed open
}

TEST_CASE("noise gate: threshold changes take effect and reset clears state") {
    NoiseGate gate;
    gate.prepare(kRate);
    gate.setThresholdDb(-30.0f);
    std::vector<float> l(4800), r;
    for (size_t i = 0; i < l.size(); ++i)
        l[i] = 0.01f *
               static_cast<float>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kRate)); // -40 dBFS
    r = l;
    gate.process(l.data(), r.data(), static_cast<uint32_t>(l.size()));
    CHECK_FALSE(gate.isOpen()); // -40 dB is below a -30 dB threshold

    gate.setThresholdDb(-50.0f);
    for (size_t i = 0; i < l.size(); ++i)
        l[i] = r[i] =
            0.01f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kRate));
    gate.process(l.data(), r.data(), static_cast<uint32_t>(l.size()));
    CHECK(gate.isOpen());
    gate.reset();
    CHECK_FALSE(gate.isOpen());
    CHECK(gate.gain() == 0.0f);
}
