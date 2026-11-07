// StftAnalyzer and mixToMono tests.

#include <doctest/doctest.h>

#include <dsptb/dsp/window.hpp>
#include <dsptb/dsp_common/fft.hpp>
#include <dsptb/dsp_common/stft.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using dsptb::dsp::ChannelMix;
using dsptb::dsp::RealFft;
using dsptb::dsp::StftAnalyzer;
using dsptb::dsp::StftConfig;
using dsptb::dsp::Window;
using dsptb::dsp::mixToMono;
using dsptb::test::signals::LcgNoise;

namespace {

constexpr double kFs = 48000.0;

struct Collected {
    std::vector<std::vector<float>> frames;
    std::vector<std::uint64_t> indices;
};

template <class PushFn> Collected collectAll(StftAnalyzer& analyzer, PushFn&& pushFn) {
    Collected out;
    auto onFrame = [&out](const float* data, std::size_t bins, std::uint64_t frameIndex) {
        out.frames.emplace_back(data, data + bins);
        out.indices.push_back(frameIndex);
    };
    pushFn(analyzer, onFrame);
    return out;
}

std::vector<float> sineSamples(std::size_t count, double freq, double amp) {
    std::vector<float> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(dsptb::test::signals::sine(i, kFs, freq, amp, 0.0));
    return out;
}

std::vector<float> noiseSamples(std::size_t count, std::uint32_t seed, double amp) {
    std::vector<float> out;
    out.reserve(count);
    LcgNoise noise(seed, amp);
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(noise.next());
    return out;
}

std::size_t peakBin(const std::vector<float>& frameDb) {
    return static_cast<std::size_t>(std::max_element(frameDb.begin(), frameDb.end()) - frameDb.begin());
}

} // namespace

TEST_CASE("stft: 1 kHz sine at -6 dBFS peaks at 1 kHz within tolerance (Hann and Blackman-Harris)") {
    // -6 dBFS amplitude; 1 kHz is bin 85.33 at fft 4096 / 48 kHz, so the
    // peak bin may sit either side of the exact frequency.
    const double amp = std::pow(10.0, -6.0 / 20.0);
    const std::vector<float> signal = sineSamples(4096 + 3 * 1024, 1000.0, amp);
    const double binSpacing = kFs / 4096.0;

    for (const Window window : {Window::Hann, Window::BlackmanHarris}) {
        StftConfig config;
        config.fftSize = 4096;
        config.hop = 1024;
        config.window = window;
        StftAnalyzer analyzer(config);
        const Collected got = collectAll(analyzer, [&signal](StftAnalyzer& a, auto&& cb) {
            a.push(signal.data(), signal.size(), cb);
        });
        REQUIRE(got.frames.size() == 4u);
        for (const std::vector<float>& frame : got.frames) {
            const std::size_t peak = peakBin(frame);
            CHECK(std::abs(static_cast<double>(peak) * binSpacing - 1000.0) <= binSpacing);
            const double peakDb = static_cast<double>(frame[peak]);
            if (window == Window::BlackmanHarris) {
                CHECK(std::abs(peakDb - (-6.0)) <= 0.5);
            } else {
                // keep the non-bin-centred
                // 1 kHz tone and widen the Hann tolerance to +/-0.75 dB.
                // 1 kHz is 1/3 bin off-centre, so the periodic Hann window's
                // 0.63 dB scalloping loss makes the expected value about
                // -6.63 dB. The separate bin-centred test below checks the
                // amplitude scaling tightly.
                INFO("Hann peak = ", peakDb, " dB");
                CHECK(std::abs(peakDb - (-6.0)) <= 0.75);
            }
        }
    }
}

TEST_CASE("stft: bin-centred Hann sine checks amplitude scaling within 0.1 dB") {
    // Bin 85 at fft 4096 / 48 kHz is exactly 996.09375 Hz. With no
    // scalloping loss, this isolates the 2/sum(w) amplitude correction in
    // the requirement, so the maintainer asked for a tight +/-0.1 dB check here.
    const double amp = std::pow(10.0, -6.0 / 20.0);
    const std::vector<float> signal = sineSamples(4096 + 3 * 1024, 996.09375, amp);

    StftConfig config;
    config.fftSize = 4096;
    config.hop = 1024;
    config.window = Window::Hann;
    StftAnalyzer analyzer(config);
    const Collected got = collectAll(analyzer, [&signal](StftAnalyzer& a, auto&& cb) {
        a.push(signal.data(), signal.size(), cb);
    });

    REQUIRE(got.frames.size() == 4u);
    for (const std::vector<float>& frame : got.frames) {
        const std::size_t peak = peakBin(frame);
        CHECK(peak == 85u);
        CHECK(std::abs(static_cast<double>(frame[peak]) - (-6.0)) <= 0.1);
    }
}

TEST_CASE("stft: random chunk sizes give bit-identical frames to one large push") {
    constexpr std::size_t fftSize = 1024;
    constexpr std::size_t hop = 256;
    constexpr std::size_t total = 48000;
    const std::vector<float> signal = noiseSamples(total, 99u, 0.8);

    StftConfig config;
    config.fftSize = fftSize;
    config.hop = hop;

    StftAnalyzer oneShot(config);
    const Collected whole = collectAll(oneShot, [&signal](StftAnalyzer& a, auto&& cb) {
        a.push(signal.data(), signal.size(), cb);
    });

    StftAnalyzer chunked(config);
    const Collected pieces = collectAll(chunked, [&signal](StftAnalyzer& a, auto&& cb) {
        // Deterministic pseudo-random chunk sizes in [1, 5000].
        std::uint32_t state = 123456789u;
        std::size_t pos = 0;
        while (pos < signal.size()) {
            state = 1664525u * state + 1013904223u;
            const std::size_t chunk = 1 + (state >> 8) % 5000;
            const std::size_t take = std::min(chunk, signal.size() - pos);
            a.push(signal.data() + pos, take, cb);
            pos += take;
        }
    });

    const std::size_t expectedFrames = (total - fftSize) / hop + 1;
    CHECK(whole.frames.size() == expectedFrames);
    CHECK(pieces.frames.size() == expectedFrames);
    CHECK(pieces.indices == whole.indices);
    // Exact equality: framing must not depend on chunking at all.
    CHECK(pieces.frames == whole.frames);
}

TEST_CASE("stft: frame count follows floor((N - fftSize)/hop) + 1") {
    StftConfig config;
    config.fftSize = 256;
    config.hop = 128;
    const std::vector<float> signal = noiseSamples(1024, 3u, 0.5);
    const struct {
        std::size_t pushed;
        std::size_t frames;
    } cases[] = {{0, 0}, {100, 0}, {255, 0}, {256, 1}, {257, 1}, {383, 1}, {384, 2}, {385, 2}, {1024, 7}};
    for (const auto& c : cases) {
        StftAnalyzer analyzer(config);
        std::size_t frames = 0;
        analyzer.push(signal.data(), c.pushed,
                      [&frames](const float*, std::size_t, std::uint64_t) { ++frames; });
        CHECK(frames == c.frames);
    }
}

TEST_CASE("stft: pushPower returns |X_k|^2 with no window correction") {
    constexpr std::size_t fftSize = 512;
    constexpr std::size_t hop = 256;
    const std::vector<float> signal = noiseSamples(fftSize + hop, 17u, 0.9);

    StftConfig config;
    config.fftSize = fftSize;
    config.hop = hop;
    config.window = Window::Hann;
    StftAnalyzer analyzer(config);
    const Collected got = collectAll(analyzer, [&signal](StftAnalyzer& a, auto&& cb) {
        a.pushPower(signal.data(), signal.size(), cb);
    });
    REQUIRE(got.frames.size() == 2u);

    // Reference: window the same frames by hand and run RealFft directly.
    std::vector<float> window(fftSize);
    dsptb::hannWindow(window.data(), fftSize);
    RealFft fft(fftSize);
    for (std::size_t f = 0; f < 2; ++f) {
        std::vector<float> windowed(fftSize);
        for (std::size_t i = 0; i < fftSize; ++i)
            windowed[i] = signal[f * hop + i] * window[i];
        std::vector<float> expected(fft.bins());
        fft.powerSpectrum(windowed.data(), expected.data());
        REQUIRE(got.frames[f].size() == expected.size());
        for (std::size_t k = 0; k < expected.size(); ++k)
            CHECK(got.frames[f][k] == expected[k]); // same code path: bit-identical
    }
}

TEST_CASE("stft: mixToMono Mid, Left, Right and Side formulas") {
    const float left[] = {1.0f, -0.5f, 0.25f, 2.0f};
    const float right[] = {0.5f, 0.5f, -0.75f, -2.0f};
    float out[4] = {};

    mixToMono(left, right, 4, ChannelMix::Mid, out);
    CHECK(out[0] == doctest::Approx(0.75));
    CHECK(out[1] == doctest::Approx(0.0));
    CHECK(out[2] == doctest::Approx(-0.25));
    CHECK(out[3] == doctest::Approx(0.0));

    mixToMono(left, right, 4, ChannelMix::Left, out);
    for (int i = 0; i < 4; ++i)
        CHECK(out[i] == doctest::Approx(left[i]));

    mixToMono(left, right, 4, ChannelMix::Right, out);
    for (int i = 0; i < 4; ++i)
        CHECK(out[i] == doctest::Approx(right[i]));

    mixToMono(left, right, 4, ChannelMix::Side, out);
    CHECK(out[0] == doctest::Approx(0.25));
    CHECK(out[1] == doctest::Approx(-0.5));
    CHECK(out[2] == doctest::Approx(0.5));
    CHECK(out[3] == doctest::Approx(2.0));
}

TEST_CASE("stft: bins() and binHz() follow the config") {
    StftConfig config;
    config.fftSize = 4096;
    config.hop = 1024;
    StftAnalyzer analyzer(config);
    CHECK(analyzer.bins() == 2049u);
    CHECK(analyzer.binHz(kFs, 0) == doctest::Approx(0.0));
    CHECK(analyzer.binHz(kFs, 85) == doctest::Approx(85.0 * 48000.0 / 4096.0));
    CHECK(analyzer.binHz(kFs, 2048) == doctest::Approx(24000.0));
}

TEST_CASE("stft: invalid configs throw std::invalid_argument") {
    StftConfig badFft;
    badFft.fftSize = 1000; // not a power of two
    CHECK_THROWS_AS((StftAnalyzer(badFft)), std::invalid_argument);
    badFft.fftSize = 16; // power of two, below the RealFft range
    CHECK_THROWS_AS((StftAnalyzer(badFft)), std::invalid_argument);

    StftConfig badHop;
    badHop.hop = 0;
    CHECK_THROWS_AS((StftAnalyzer(badHop)), std::invalid_argument);

    StftAnalyzer analyzer(StftConfig{});
    CHECK_THROWS_AS(analyzer.configure(badFft), std::invalid_argument);
    CHECK_THROWS_AS(analyzer.configure(badHop), std::invalid_argument);
    // A failed configure leaves the previous configuration working.
    CHECK(analyzer.bins() == 2049u);
}

TEST_CASE("stft: reset restarts framing and configure rebuilds") {
    const std::vector<float> signal = noiseSamples(8192, 21u, 0.6);
    StftConfig config;
    config.fftSize = 1024;
    config.hop = 512;
    StftAnalyzer analyzer(config);
    const Collected first = collectAll(analyzer, [&signal](StftAnalyzer& a, auto&& cb) {
        a.push(signal.data(), signal.size(), cb);
    });
    analyzer.reset();
    const Collected second = collectAll(analyzer, [&signal](StftAnalyzer& a, auto&& cb) {
        a.push(signal.data(), signal.size(), cb);
    });
    CHECK(second.frames == first.frames);
    CHECK(second.indices == first.indices);
    REQUIRE(!second.indices.empty());
    CHECK(second.indices.front() == 0u);

    StftConfig bigger;
    bigger.fftSize = 2048;
    bigger.hop = 512;
    analyzer.configure(bigger);
    CHECK(analyzer.bins() == 1025u);
    const Collected third = collectAll(analyzer, [&signal](StftAnalyzer& a, auto&& cb) {
        a.push(signal.data(), signal.size(), cb);
    });
    CHECK(third.frames.size() == (8192u - 2048u) / 512u + 1u);
}

TEST_CASE("stft: push and pushPower do not allocate") {
    StftConfig config;
    config.fftSize = 1024;
    config.hop = 256;
    StftAnalyzer analyzer(config);
    const std::vector<float> signal = noiseSamples(8192, 31u, 0.5);
    // Fixed-size sink: the callback itself must not allocate either.
    float sink[513];
    std::size_t frames = 0;
    auto onFrame = [&sink, &frames](const float* data, std::size_t bins, std::uint64_t) {
        for (std::size_t k = 0; k < bins; ++k)
            sink[k] = data[k];
        ++frames;
    };
    std::size_t allocs = 0;
    {
        dsptb::test::AllocGuard guard;
        analyzer.push(signal.data(), signal.size(), onFrame);
        analyzer.pushPower(signal.data(), signal.size(), onFrame);
        allocs = guard.count();
    }
    // push: (8192-1024)/256 + 1 = 29 frames; pushPower continues the same
    // stream, adding 8192/256 = 32 more.
    CHECK(frames == 61u);
    CHECK(allocs == 0);
    CHECK(std::isfinite(sink[0]));
}

TEST_CASE("stft: micro-benchmark 4096/1024 (report only, budget 60 us per frame)") {
    StftConfig config;
    config.fftSize = 4096;
    config.hop = 1024;
    StftAnalyzer analyzer(config);
    const std::vector<float> signal = noiseSamples(4096 + 1024 * 256, 77u, 0.8);
    auto onFrame = [](const float*, std::size_t, std::uint64_t) {};

    analyzer.push(signal.data(), signal.size(), onFrame); // warm-up
    constexpr int kRounds = 8;
    std::size_t frames = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < kRounds; ++r) {
        analyzer.reset(); // O(1); keeps every round at exactly 257 frames
        analyzer.push(signal.data(), signal.size(),
                      [&frames](const float*, std::size_t, std::uint64_t) { ++frames; });
    }
    const auto stop = std::chrono::steady_clock::now();
    const double totalUs = std::chrono::duration<double, std::micro>(stop - start).count();
    const double usPerFrame = totalUs / static_cast<double>(frames);
    std::printf("[bench] StftAnalyzer 4096/1024: %.2f us per frame over %zu frames (budget: 60 us)\n",
                usPerFrame, frames);
    CHECK(frames == static_cast<std::size_t>(kRounds) * 257u);
    CHECK(usPerFrame > 0.0);
}
