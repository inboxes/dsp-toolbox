#include <doctest/doctest.h>

#include <dsptb/core/duplex_bridge.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using dsptb::DuplexBridge;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr double kFreq = 1000.0;

struct SimResult {
    std::vector<float> out; // left channel of everything the playback side received
    double maxStep = 0.0;   // largest |x[n]-x[n-1]| after the priming silence
    double sineStep = 0.0;  // the ideal sine's own largest step
};

// Event-driven simulation of two independent clocks. The capture clock runs at 48000*(1+drift) and delivers
// `captureBlock` frames per callback with random arrival jitter; the playback clock is exactly 48000 and
// pulls `playBlock` frames per callback.
SimResult simulate(DuplexBridge& bridge, double driftPpm, uint32_t captureBlock, uint32_t playBlock,
                   double seconds, double jitterFraction) {
    SimResult r;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-jitterFraction, jitterFraction);

    const double capRate = kRate * (1.0 + driftPpm * 1e-6);
    const double capPeriod = captureBlock / capRate;
    const double playPeriod = playBlock / kRate;
    double tCapBase = 0.0, tCap = 0.0, tPlay = 0.0; // tCapBase is the ideal clock; tCap adds arrival jitter
    uint64_t nIn = 0;
    std::vector<float> in(captureBlock * 2), out(playBlock * 2);

    while (tPlay < seconds) {
        if (tCap <= tPlay) {
            for (uint32_t i = 0; i < captureBlock; ++i, ++nIn) {
                // The capture device samples the world at its own (slightly wrong) clock: sample n is the
                // signal at time n/capRate, so a drifting clock is a slightly time-stretched sine.
                const float v = static_cast<float>(
                    0.5 * std::sin(2.0 * kPi * kFreq * static_cast<double>(nIn) / capRate));
                in[2 * i] = in[2 * i + 1] = v;
            }
            bridge.pushCapture(in.data(), captureBlock, static_cast<int64_t>(tCap * 1e9));
            tCapBase += capPeriod; // the device clock does not random-walk: only the arrival time is jittered
            tCap = tCapBase + capPeriod * jitter(rng);
        } else {
            bridge.pull(out.data(), playBlock, static_cast<int64_t>(tPlay * 1e9));
            for (uint32_t i = 0; i < playBlock; ++i)
                r.out.push_back(out[2 * i]);
            tPlay += playPeriod;
        }
    }
    // Ignore the priming silence and the first moments while the controller settles.
    size_t start = 0;
    while (start < r.out.size() && r.out[start] == 0.0f)
        ++start;
    start += static_cast<size_t>(kRate * 0.2);
    for (size_t i = start + 1; i < r.out.size(); ++i)
        r.maxStep = std::max(r.maxStep, std::fabs(static_cast<double>(r.out[i]) - r.out[i - 1]));
    r.sineStep = 0.5 * 2.0 * std::sin(kPi * kFreq / kRate);
    return r;
}

} // namespace

TEST_CASE("DuplexBridge: no drift, output is the input delayed, no losses") {
    DuplexBridge bridge;
    bridge.prepare(8192, 128, 128, 48000, 4.0);
    const SimResult r = simulate(bridge, 0.0, 128, 128, 20.0, 0.0);
    CHECK(bridge.underflows() == 0);
    CHECK(bridge.overflowFrames() == 0);
    CHECK(bridge.overflowEvents() == 0);
    CHECK(r.maxStep <= r.sineStep * 1.05);
    CHECK(std::fabs(bridge.ratioPpm()) < 30.0f);
}

TEST_CASE("DuplexBridge: follows clock drift of +-100 ppm for 60 s without a single dropout") {
    for (double drift : {-100.0, -20.0, 20.0, 100.0}) {
        DuplexBridge bridge;
        bridge.prepare(8192, 128, 128, 48000, 4.0);
        const SimResult r = simulate(bridge, drift, 128, 128, 60.0, 0.15);
        INFO("drift ppm = " << drift);
        CHECK(bridge.underflows() == 0);
        CHECK(bridge.overflowFrames() == 0);
        CHECK(r.maxStep <=
              r.sineStep * 1.06); // continuous: no click-sized steps, only a negligible pitch trim
        CHECK(std::fabs(static_cast<double>(bridge.driftEstimatePpm()) - drift) <
              25.0); // it learned the clock difference
        CHECK(bridge.fillFrames() > 0);
        CHECK(bridge.fillFrames() < 600);
    }
}

TEST_CASE("DuplexBridge: mismatched block sizes (capture 480, playback 128) with drift stay clean") {
    DuplexBridge bridge;
    bridge.prepare(8192, 480, 128, 48000, 12.0);
    const SimResult r = simulate(bridge, 60.0, 480, 128, 40.0, 0.1);
    CHECK(bridge.underflows() == 0);
    CHECK(bridge.overflowFrames() == 0);
    CHECK(r.maxStep <= r.sineStep * 1.06);
}

TEST_CASE("control: a fixed buffer between drifting clocks DOES run dry (why the bridge exists)") {
    // A plain FIFO with no rate adjustment: playback is 100 ppm faster than capture, i.e. 4.8 frames/s.
    // Starting from ~200 frames of cushion, it is empty after about 40 s.
    dsptb::rt::SpscRing<float> fifo(16384);
    std::vector<float> in(256, 0.1f), out(256);
    double tCap = 0.0, tPlay = 0.0;
    const double capPeriod = 128 / (kRate * (1.0 - 100e-6)), playPeriod = 128 / kRate;
    uint64_t dry = 0;
    bool primed = false;
    while (tPlay < 60.0) {
        if (tCap <= tPlay) {
            fifo.write(in.data(), 256);
            tCap += capPeriod;
        } else {
            if (!primed && fifo.size() >= 400)
                primed = true;
            if (primed && fifo.read(out.data(), 256) < 256)
                ++dry;
            tPlay += playPeriod;
        }
    }
    CHECK(dry > 0);
}

TEST_CASE("DuplexBridge: a stalled capture side re-primes with silence instead of repeating samples") {
    DuplexBridge bridge;
    bridge.prepare(4096, 128, 64, 48000, 1.0, 8);
    std::vector<float> in(128 * 2, 0.5f), out(128 * 2);
    for (int i = 0; i < 4; ++i)
        bridge.pushCapture(in.data(), 128, 0);
    for (int i = 0; i < 20; ++i)
        bridge.pull(out.data(), 128, 0); // nothing more arrives: it runs dry
    CHECK(bridge.underflows() >= 1);
    for (float v : out)
        CHECK(v == 0.0f); // silence while re-priming
    for (int i = 0; i < 4; ++i)
        bridge.pushCapture(in.data(), 128, 0); // capture comes back
    bridge.pull(out.data(), 128, 0);
    bridge.pull(out.data(), 128, 0);
    CHECK(out[2 * 64] == doctest::Approx(0.5f)); // resumed
}

TEST_CASE("DuplexBridge: a huge backlog is trimmed back to the target instead of adding latency") {
    DuplexBridge bridge;
    bridge.prepare(4096, 128, 64, 48000, 1.0, 8);
    std::vector<float> in(256 * 2, 0.25f), out(64 * 2);
    for (int i = 0; i < 4; ++i)
        bridge.pushCapture(in.data(), 256, 0);
    bridge.pull(out.data(), 64, 0); // primes
    for (int i = 0; i < 16; ++i)
        bridge.pushCapture(in.data(), 256, 0); // 4000+ frames pile up
    bridge.pull(out.data(), 64, 0);
    CHECK(bridge.overflowEvents() >= 1);
    CHECK(bridge.fillFrames() <= 512);
}
