// KeyDetector tests. The "progression" test signal: each chord is root, third, fifth and the root an
// octave lower, each at amplitude 0.1, built with sine() from
// tests/support/signals.hpp. Major keys use I-IV-V-I, minor keys use
// i-iv-V-i (major V), each played twice; roots range from C3 to B3.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/key.hpp>

#include "support/alloc_counter.hpp"
#include "support/music.hpp"
#include "support/signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;
namespace sig = dsptb::test::signals;

static_assert(noexcept(std::declval<KeyDetector&>().push(nullptr, 0)));
static_assert(noexcept(std::declval<const KeyDetector&>().result()));
static_assert(noexcept(std::declval<KeyDetector&>().reset()));

namespace {

constexpr double kFs = 48000.0;
constexpr int kC3 = 48; // MIDI note number of C3; pitch class = midi % 12, C = 0

using dsptb::test::music::appendChord;
using dsptb::test::music::midiHz;
using dsptb::test::music::progression;

KeyResult detect(double fs, const std::vector<float>& signal, const KeyConfig& config = {}) {
    KeyDetector det(fs, config);
    det.push(signal.data(), signal.size());
    return det.result();
}

// Test-local Pearson correlation and profiles, written independently of
// key.cpp so the margin test is a genuine hand computation from the
// measured chroma, not a call into the code under test.
constexpr double kMajorRef[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr double kMinorRef[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double pearsonRef(const double chroma[12], const double profile[12], int root) {
    double meanX = 0.0;
    double meanY = 0.0;
    for (int pc = 0; pc < 12; ++pc) {
        meanX += chroma[pc];
        meanY += profile[(pc - root + 12) % 12];
    }
    meanX /= 12.0;
    meanY /= 12.0;
    double sxy = 0.0;
    double sxx = 0.0;
    double syy = 0.0;
    for (int pc = 0; pc < 12; ++pc) {
        const double dx = chroma[pc] - meanX;
        const double dy = profile[(pc - root + 12) % 12] - meanY;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }
    return sxy / std::sqrt(sxx * syy);
}

} // namespace

TEST_CASE("key: invalid configurations are rejected") {
    CHECK_NOTHROW(KeyDetector{kFs});
    KeyConfig bad;
    bad.minHz = 1000.0;
    bad.maxHz = 1000.0; // minHz >= maxHz
    CHECK_THROWS_AS(KeyDetector(kFs, bad), std::invalid_argument);
    bad.minHz = 2000.0;
    bad.maxHz = 100.0; // minHz > maxHz
    CHECK_THROWS_AS(KeyDetector(kFs, bad), std::invalid_argument);
    bad = KeyConfig{};
    bad.fftSize = 1000; // not a power of two
    CHECK_THROWS_AS(KeyDetector(kFs, bad), std::invalid_argument);
    bad = KeyConfig{};
    bad.hop = 0;
    CHECK_THROWS_AS(KeyDetector(kFs, bad), std::invalid_argument);
    CHECK_THROWS_AS(KeyDetector(0.0), std::invalid_argument);
    CHECK_THROWS_AS(KeyDetector(-48000.0), std::invalid_argument);
}

TEST_CASE("key: silence is invalid, and fewer than 2 frames is invalid") {
    KeyDetector det(kFs);
    const std::vector<float> silence(48000, 0.0f);
    det.push(silence.data(), silence.size());
    CHECK(!det.result().valid);
    CHECK(!det.result().confident);
    float chroma[12];
    det.chroma(chroma);
    for (const float value : chroma)
        CHECK(value == 0.0f);

    // One full frame (8192 samples) of a loud sine is still < 2 frames.
    KeyDetector det2(kFs);
    std::vector<float> one(8192);
    for (std::size_t n = 0; n < one.size(); ++n)
        one[n] = sig::sine(n, kFs, 440.0, 0.5, 0.0);
    det2.push(one.data(), one.size());
    CHECK(!det2.result().valid);
    // One more hop completes the second frame: now the answer is valid.
    det2.push(one.data(), 4096);
    CHECK(det2.result().valid);
}

TEST_CASE("key: all 12 major progressions are detected exactly and confident") {
    for (int root = 0; root < 12; ++root) {
        INFO("major root (pitch class) = ", root);
        const std::vector<float> signal = progression(kFs, kC3 + root, false);
        const KeyResult r = detect(kFs, signal);
        CHECK(r.valid);
        CHECK(r.root == root);
        CHECK(r.mode == KeyMode::Major);
        CHECK(r.confident);
    }
}

TEST_CASE("key: minor progressions — at least 10 of 12 exact, misses are the relative major, all confident") {
    int exact = 0;
    for (int root = 0; root < 12; ++root) {
        INFO("minor root (pitch class) = ", root);
        const std::vector<float> signal = progression(kFs, kC3 + root, true);
        const KeyResult r = detect(kFs, signal);
        CHECK(r.valid);
        CHECK(r.confident); // all 12 must be confident, exact or not
        if (r.root == root && r.mode == KeyMode::Minor) {
            ++exact;
        } else {
            // The only tolerated miss: the relative major.
            std::printf("[key] minor root %d missed as root %d %s (r=%.3f, margin=%.3f)\n", root, r.root,
                        r.mode == KeyMode::Major ? "major" : "minor", static_cast<double>(r.correlation),
                        static_cast<double>(r.margin));
            CHECK(r.mode == KeyMode::Major);
            CHECK(r.root == (root + 9) % 12);
        }
    }
    std::printf("[key] minor progressions exact: %d/12\n", exact);
    CHECK(exact >= 10);
}

TEST_CASE("key: the margin ignores the relative key (C major, hand-computed)") {
    KeyDetector det(kFs);
    const std::vector<float> signal = progression(kFs, kC3, false);
    det.push(signal.data(), signal.size());
    const KeyResult r = det.result();
    REQUIRE(r.valid);
    REQUIRE(r.root == 0);
    REQUIRE(r.mode == KeyMode::Major);

    float chromaF[12];
    det.chroma(chromaF);
    double chroma[12];
    for (int pc = 0; pc < 12; ++pc)
        chroma[pc] = static_cast<double>(chromaF[pc]);

    // Hand-computed scores from the measured chroma.
    const double rC = pearsonRef(chroma, kMajorRef, 0);  // the winner
    const double rAm = pearsonRef(chroma, kMinorRef, 9); // its relative key
    double bestOther = -2.0;                             // best excluding C major AND A minor
    for (int root = 0; root < 12; ++root) {
        if (root != 0)
            bestOther = std::max(bestOther, pearsonRef(chroma, kMajorRef, root));
        if (root != 9)
            bestOther = std::max(bestOther, pearsonRef(chroma, kMinorRef, root));
    }
    // The margin with A minor INCLUDED in the "other" pool: the issue's
    // criterion is that the A-minor score does not reduce the margin,
    // i.e. this must equal the margin with A minor excluded. (Measured
    // with the peak-aware chroma: rAm = 0.5268 is below the best
    // non-relative 0.6311, so A minor is not the runner-up for this
    // signal and cannot reduce the margin either way — the equality
    // assertions are the criterion.)
    double bestOtherIncl = bestOther;
    bestOtherIncl = std::max(bestOtherIncl, rAm);
    std::printf("[key] C major: r(C)=%.4f, r(A minor)=%.4f, best non-relative=%.4f, margin=%.4f "
                "(margin if A minor counted: %.4f)\n",
                rC, rAm, bestOther, static_cast<double>(r.margin), rC - bestOtherIncl);
    CHECK(r.correlation == doctest::Approx(static_cast<float>(rC)).epsilon(1e-4));
    CHECK(r.margin == doctest::Approx(static_cast<float>(rC - bestOther)).epsilon(1e-4));
    CHECK(static_cast<float>(rC - bestOtherIncl) ==
          doctest::Approx(static_cast<float>(rC - bestOther)).epsilon(1e-4));
}

TEST_CASE("key: lcgNoise for 10 s is not confident") {
    std::vector<float> signal(480000);
    sig::LcgNoise noise(1234u, 0.5);
    for (float& sample : signal)
        sample = noise.next();
    const KeyResult r = detect(kFs, signal);
    std::printf("[key] lcgNoise 10 s: valid=%d, root=%d, r=%.3f, margin=%.4f, confident=%d\n",
                r.valid ? 1 : 0, r.root, static_cast<double>(r.correlation),
                static_cast<double>(r.margin), r.confident ? 1 : 0);
    CHECK(!r.confident);
}

TEST_CASE("key: one sustained A-major triad for 5 s gives root A, mode major") {
    std::vector<float> signal;
    appendChord(signal, kFs, 57, true, 5.0); // A3 major triad (MIDI 57 = A)
    const KeyResult r = detect(kFs, signal);
    CHECK(r.valid);
    CHECK(r.root == 9);
    CHECK(r.mode == KeyMode::Major);
}

TEST_CASE("key: at 96 kHz the Eb3 peak of the C-minor chord is estimated within 2 Hz") {
    // with fftSize scaled x2 at 96 kHz
    // (16384, hop 8192), C3 and Eb3 sit ~4.2 bins apart instead of
    // ~2.1, so the 3-point log-parabola estimate of Eb3 in the C-minor
    // tonic chord must land within 2 Hz of 155.56 Hz in every frame.
    // The signal is a stationary sum of sines, so every frame's
    // magnitude spectrum is phase-independent and no frame — frame 0
    // included — is special. The framing below replicates the
    // detector's scaled 96 kHz framing and runs the production peak
    // extraction (detail::extractFramePeaks) on each frame.
    const double fs = 96000.0;
    std::vector<float> chord;
    appendChord(chord, fs, kC3, false, 1.0); // C minor tonic: C3, Eb3, G3, C2

    StftAnalyzer stft(StftConfig{16384, 8192, Window::Hann});
    std::vector<double> magnitudes(stft.bins());
    std::vector<detail::FramePeak> peaks(stft.bins() / 2 + 1);
    const double binHz = fs / 16384.0;
    int frames = 0;
    bool foundEveryFrame = true;
    double worst = 0.0;
    stft.pushPower(chord.data(), chord.size(),
                   [&](const float* power, std::size_t bins, std::uint64_t) noexcept {
                       ++frames;
                       const std::size_t count = detail::extractFramePeaks(
                           power, bins, binHz, 55.0, 1760.0, magnitudes.data(), peaks.data(),
                           peaks.size());
                       double nearest = 1e30;
                       for (std::size_t i = 0; i < count; ++i)
                           nearest = std::min(nearest, std::abs(peaks[i].frequencyHz - 155.56));
                       if (nearest > 20.0)
                           foundEveryFrame = false; // no peak anywhere near Eb3 this frame
                       worst = std::max(worst, nearest);
                   });
    std::printf("[key] Eb3 at 96 kHz: %d frames, worst |estimate - 155.56 Hz| = %.3f Hz\n", frames,
                worst);
    CHECK(frames >= 5);
    CHECK(foundEveryFrame);
    CHECK(worst <= 2.0);
}

TEST_CASE("key: sample rates 44.1, 48 and 96 kHz give the same answers") {
    for (const double fs : {44100.0, 48000.0, 96000.0}) {
        INFO("fs = ", fs);
        const KeyResult cMaj = detect(fs, progression(fs, kC3, false));
        CHECK(cMaj.root == 0);
        CHECK(cMaj.mode == KeyMode::Major);
        const KeyResult gMaj = detect(fs, progression(fs, kC3 + 7, false));
        CHECK(gMaj.root == 7);
        CHECK(gMaj.mode == KeyMode::Major);
        // C minor — the case the rate scaling fixes — is exact and
        // confident at every rate.
        const KeyResult cMin = detect(fs, progression(fs, kC3, true));
        CHECK(cMin.root == 0);
        CHECK(cMin.mode == KeyMode::Minor);
        CHECK(cMin.confident);
    }
    // A minor progression: the answer must be identical at all three
    // rates (whatever it is), not rate-dependent.
    const KeyResult a44 = detect(44100.0, progression(44100.0, kC3 + 9, true));
    const KeyResult a48 = detect(48000.0, progression(48000.0, kC3 + 9, true));
    const KeyResult a96 = detect(96000.0, progression(96000.0, kC3 + 9, true));
    CHECK(a44.root == a48.root);
    CHECK(a44.mode == a48.mode);
    CHECK(a96.root == a48.root);
    CHECK(a96.mode == a48.mode);
    std::printf("[key] A-minor progression answer at 44.1/48/96 kHz: root %d %s\n", a48.root,
                a48.mode == KeyMode::Major ? "major" : "minor");
}

TEST_CASE("key: full 24x3 matrix — every progression exact and confident at 44.1, 48 and 96 kHz") {
    // The maintainer asked for the full matrix as committed CHECKs
    //: the per-rate case above samples only a few keys, and
    // the 72-run matrix was originally verified as a one-off probe.
    // This case pins every coordinate — including C minor at 96 kHz,
    // the failure the rate scaling fixes — against regression.
    for (const double fs : {44100.0, 48000.0, 96000.0}) {
        for (int root = 0; root < 12; ++root) {
            for (const bool minorKey : {false, true}) {
                INFO("fs = ", fs, ", root (pitch class) = ", root,
                     ", mode = ", minorKey ? "minor" : "major");
                const KeyResult r = detect(fs, progression(fs, kC3 + root, minorKey));
                CHECK(r.valid);
                CHECK(r.root == root);
                CHECK(r.mode == (minorKey ? KeyMode::Minor : KeyMode::Major));
                CHECK(r.confident);
            }
        }
    }
}

TEST_CASE("key: random chunk sizes give the same chroma as one push; result() between pushes") {
    const std::vector<float> signal = progression(kFs, kC3 + 4, false); // E major
    KeyDetector ref(kFs);
    ref.push(signal.data(), signal.size());
    float chromaRef[12];
    ref.chroma(chromaRef);
    const KeyResult refResult = ref.result();

    KeyDetector det(kFs);
    std::uint32_t state = 987654321u;
    std::size_t pos = 0;
    int pushes = 0;
    while (pos < signal.size()) {
        state = 1664525u * state + 1013904223u;
        const std::size_t chunk = 1 + (state >> 8) % 10000; // 1..10000
        const std::size_t take = std::min(chunk, signal.size() - pos);
        det.push(signal.data() + pos, take);
        pos += take;
        if (++pushes % 7 == 0)
            (void)det.result(); // callable between pushes at any time
    }
    float chromaChunked[12];
    det.chroma(chromaChunked);
    double worst = 0.0;
    for (int pc = 0; pc < 12; ++pc) {
        const double diff = std::abs(static_cast<double>(chromaChunked[pc]) - static_cast<double>(chromaRef[pc]));
        const double tol = 1e-6 * static_cast<double>(chromaRef[pc]) + 1e-9;
        worst = std::max(worst, diff);
        CHECK(diff <= tol);
    }
    std::printf("[key] chunked vs one push: worst chroma difference %.3g (%d pushes)\n", worst, pushes);
    const KeyResult r = det.result();
    CHECK(r.root == refResult.root);
    CHECK(r.mode == refResult.mode);
    CHECK(r.correlation == doctest::Approx(refResult.correlation).epsilon(1e-5));
}

TEST_CASE("key: reset clears everything (same answers as a fresh instance)") {
    const std::vector<float> signal = progression(kFs, kC3 + 2, false); // D major
    KeyDetector det(kFs);
    det.push(signal.data(), signal.size());
    REQUIRE(det.result().valid);
    det.reset();
    float chroma[12];
    det.chroma(chroma);
    for (const float value : chroma)
        CHECK(value == 0.0f);
    CHECK(!det.result().valid);
    det.push(signal.data(), signal.size());
    const KeyResult after = det.result();
    const KeyResult fresh = detect(kFs, signal);
    CHECK(after.root == fresh.root);
    CHECK(after.mode == fresh.mode);
    CHECK(after.correlation == doctest::Approx(fresh.correlation).epsilon(1e-6));
    CHECK(after.margin == doctest::Approx(fresh.margin).epsilon(1e-6));
}

TEST_CASE("key: no allocation in push, chroma or result") {
    const std::vector<float> signal = progression(kFs, kC3, false);
    KeyDetector det(kFs);
    det.push(signal.data(), 48000); // warm up: framing state is live
    float chroma[12];
    AllocGuard guard;
    det.push(signal.data() + 48000, signal.size() - 48000);
    det.chroma(chroma);
    const KeyResult r = det.result();
    CHECK(guard.count() == 0);
    CHECK(r.valid);
}

TEST_CASE("key: micro-benchmark — push 3 minutes of 48 kHz mono (report only)") {
    // 3 minutes built by tiling the 8 s C-major progression.
    const std::vector<float> tile = progression(kFs, kC3, false);
    std::vector<float> signal;
    signal.reserve(3 * 60 * 48000);
    while (signal.size() < 3 * 60 * 48000)
        signal.insert(signal.end(), tile.begin(), tile.end());
    signal.resize(3 * 60 * 48000);
    KeyDetector det(kFs);
    const auto start = std::chrono::steady_clock::now();
    det.push(signal.data(), signal.size());
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    std::printf("[bench] KeyDetector push 3 min of 48 kHz mono: %.3f s (budget: <= 1 s)\n", seconds);
    CHECK(det.result().valid);
}

TEST_CASE("key: micro-benchmark — push 3 minutes of 96 kHz mono (report only)") {
    // At 96 kHz the scaled hop doubles too, so there are the same
    // number of frames per second of audio, each with an FFT twice
    // the size: the measured cost is ~2.1x the 48 kHz benchmark
    // (0.545 s vs 0.263 s for 3 minutes on this machine).
    const double fs = 96000.0;
    const std::vector<float> tile = progression(fs, kC3, false);
    std::vector<float> signal;
    const std::size_t total = 3 * 60 * 96000;
    signal.reserve(total);
    while (signal.size() < total)
        signal.insert(signal.end(), tile.begin(), tile.end());
    signal.resize(total);
    KeyDetector det(fs);
    const auto start = std::chrono::steady_clock::now();
    det.push(signal.data(), signal.size());
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    std::printf("[bench] KeyDetector push 3 min of 96 kHz mono: %.3f s (report only)\n", seconds);
    CHECK(det.result().valid);
}

// ---- estimated tuning ----

namespace {

struct TunedScore {
    int failures = 0;
    double worstOffsetError = 0.0;
};

TunedScore scoreAllKeys(double a4, const KeyConfig& config) {
    TunedScore score;
    for (int root = 0; root < 12; ++root) {
        for (const bool minor : {false, true}) {
            const KeyResult r = detect(kFs, progression(kFs, kC3 + root, minor, a4), config);
            const bool right = r.valid && r.confident && r.root == root &&
                               r.mode == (minor ? KeyMode::Minor : KeyMode::Major);
            if (!right)
                ++score.failures;
            const double truth = 1200.0 * std::log2(a4 / 440.0);
            score.worstOffsetError =
                std::max(score.worstOffsetError, std::abs(static_cast<double>(r.tuningOffsetCents) - truth));
        }
    }
    return score;
}

} // namespace

TEST_CASE("KeyDetector finds all 24 keys when the recording isn't at A = 440") {
    for (const double a4 : {452.0, 428.0}) {
        const TunedScore score = scoreAllKeys(a4, KeyConfig{});
        CAPTURE(a4);
        CHECK(score.failures == 0);
        CHECK(score.worstOffsetError <= 2.0);
    }
}

TEST_CASE("A fixed tuning of 440 fails on an off-tune recording, so the test above proves the estimator") {
    KeyConfig fixed;
    fixed.tuning = Tuning{440.0};
    CHECK(scoreAllKeys(452.0, fixed).failures > 0);

    const KeyResult r = detect(kFs, progression(kFs, kC3, false, 452.0), fixed);
    CHECK_FALSE(r.tuningEstimated);
    CHECK(r.tuningOffsetCents == 0.0f);
}

TEST_CASE("A fixed tuning that matches the recording finds the key") {
    KeyConfig fixed;
    fixed.tuning = Tuning{452.0};
    const KeyResult r = detect(kFs, progression(kFs, kC3 + 5, true, 452.0), fixed);
    CHECK(r.confident);
    CHECK(r.root == 5);
    CHECK(r.mode == KeyMode::Minor);
    CHECK_FALSE(r.tuningEstimated);
    CHECK(r.tuningOffsetCents == doctest::Approx(Tuning{452.0}.offsetCents()).epsilon(1e-4));
}
