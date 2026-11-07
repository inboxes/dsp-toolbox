#include <doctest/doctest.h>

#include <dsptb/dsp_common/fft.hpp>
#include <dsptb/dsp_common/mel.hpp>

#include "support/alloc_counter.hpp"
#include "support/signals.hpp"

#include <dsptb/dsp/window.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;
namespace sig = dsptb::test::signals;

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kFftSize = 2048;
constexpr std::size_t kBins = kFftSize / 2 + 1;
constexpr std::size_t kHop = 512;
constexpr std::size_t kNMels = 40;
constexpr std::size_t kNMfcc = 13;

std::string goldenText(const char* fileName) {
    const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "golden" / fileName;
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot open golden file: ", path.string());
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Minimal JSON readers for the golden files' fixed shapes. findKey locates
// `"key"` at or after `from`; readArray parses the number array that
// follows it (the golden writer emits flat arrays with no nesting).
std::size_t findKey(const std::string& text, const char* key, std::size_t from = 0) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t pos = text.find(needle, from);
    REQUIRE(pos != std::string::npos);
    return pos;
}

double readScalar(const std::string& text, const char* key, std::size_t from = 0) {
    const std::size_t pos = findKey(text, key, from);
    const std::size_t colon = text.find(':', pos);
    REQUIRE(colon != std::string::npos);
    return std::stod(text.substr(colon + 1));
}

std::vector<double> readArray(const std::string& text, std::size_t keyPos) {
    const std::size_t open = text.find('[', keyPos);
    REQUIRE(open != std::string::npos);
    // Match brackets so nested arrays (per-frame logmel/mfcc) read whole.
    std::size_t close = open;
    int depth = 0;
    for (; close < text.size(); ++close) {
        if (text[close] == '[')
            ++depth;
        else if (text[close] == ']' && --depth == 0)
            break;
    }
    REQUIRE(close < text.size());
    std::vector<double> values;
    const char* cursor = text.c_str() + open + 1;
    const char* const end = text.c_str() + close;
    while (cursor < end) {
        char* next = nullptr;
        const double value = std::strtod(cursor, &next);
        if (next == cursor) {
            ++cursor; // separator
        } else {
            values.push_back(value);
            cursor = next;
        }
    }
    return values;
}

// One sparse filterbank config from mel_filterbank.json.
struct GoldenBank {
    bool htk = false;
    MelNorm norm = MelNorm::Slaney;
    std::vector<std::vector<double>> dense; // nMels x bins, zeros filled in
};

std::vector<GoldenBank> readFilterbankGolden(const std::string& text) {
    const std::size_t bins = static_cast<std::size_t>(readScalar(text, "bins"));
    const std::size_t nMels = static_cast<std::size_t>(readScalar(text, "n_mels"));
    std::vector<GoldenBank> banks;
    std::size_t cursor = findKey(text, "configs");
    for (int c = 0; c < 3; ++c) {
        GoldenBank bank;
        const std::size_t htkPos = findKey(text, "htk", cursor);
        const std::size_t colon = text.find(':', htkPos);
        REQUIRE(colon != std::string::npos);
        bank.htk = text[colon + 2] == 't'; // "htk": true / false
        const std::size_t normPos = findKey(text, "norm", htkPos);
        const std::size_t normColon = text.find(':', normPos);
        REQUIRE(normColon != std::string::npos);
        // "norm": "slaney" or "norm": null — norm is a per-config field now
        // that the golden holds a third (htk=true, slaney) config; it can no
        // longer be inferred from htk alone.
        bank.norm = text[normColon + 2] == '"' ? MelNorm::Slaney : MelNorm::None;
        bank.dense.assign(nMels, std::vector<double>(bins, 0.0));
        cursor = findKey(text, "rows", htkPos);
        for (std::size_t row = 0; row < nMels; ++row) {
            const std::size_t firstPos = findKey(text, "first", cursor);
            const auto first = static_cast<std::size_t>(readScalar(text, "first", cursor));
            const std::vector<double> values = readArray(text, findKey(text, "values", firstPos));
            REQUIRE(first + values.size() <= bins);
            for (std::size_t j = 0; j < values.size(); ++j)
                bank.dense[row][first + j] = values[j];
            cursor = firstPos + 1;
        }
        banks.push_back(std::move(bank));
    }
    return banks;
}

// Power spectra of the golden signal, framed exactly like the golden
// pipeline: periodic Hann (the SDK window — the same 0.5 - 0.5*cos(2*pi*
// i/N) formula as librosa's get_window('hann'), which is periodic), no
// centring, hop 512, |FFT|^2 via RealFft::powerSpectrum.
std::vector<std::vector<float>> goldenPowerFrames(const std::vector<float>& signal) {
    RealFft fft(kFftSize);
    std::vector<float> window(kFftSize);
    dsptb::hannWindow(window.data(), kFftSize);
    const std::size_t frames = 1 + (signal.size() - kFftSize) / kHop;
    std::vector<std::vector<float>> out;
    out.reserve(frames);
    std::vector<float> frame(kFftSize);
    std::vector<float> power(kBins);
    for (std::size_t t = 0; t < frames; ++t) {
        for (std::size_t i = 0; i < kFftSize; ++i)
            frame[i] = signal[t * kHop + i] * window[i];
        fft.powerSpectrum(frame.data(), power.data());
        out.push_back(power);
    }
    return out;
}

} // namespace

TEST_CASE("mel: config validation") {
    const auto makeBank = [](const MelConfig& c) { return MelFilterbank(c); };
    MelConfig cfg;
    CHECK_NOTHROW(makeBank(cfg));
    MelConfig bad = cfg;
    bad.fftSize = 1000;
    CHECK_THROWS_AS(makeBank(bad), std::invalid_argument);
    bad = cfg;
    bad.nMels = 0;
    CHECK_THROWS_AS(makeBank(bad), std::invalid_argument);
    bad = cfg;
    bad.sampleRate = 0.0;
    CHECK_THROWS_AS(makeBank(bad), std::invalid_argument);
    bad = cfg;
    bad.fmin = 8000.0;
    bad.fmax = 4000.0;
    CHECK_THROWS_AS(makeBank(bad), std::invalid_argument);
    bad = cfg;
    bad.fmax = 30000.0; // above fs/2
    CHECK_THROWS_AS(makeBank(bad), std::invalid_argument);
    CHECK_THROWS_AS(Mfcc(cfg, 0), std::invalid_argument);
    CHECK_THROWS_AS(Mfcc(cfg, cfg.nMels + 1), std::invalid_argument);
    CHECK_NOTHROW(Mfcc(cfg, cfg.nMels));
}

TEST_CASE("mel: golden signal head matches the C++ goldenSignal to 1e-7") {
    const std::string text = goldenText("mfcc_golden.json");
    const std::vector<double> head = readArray(text, findKey(text, "signal_first32"));
    REQUIRE(head.size() == 32);
    const std::vector<float> signal = sig::goldenSignal(kFs);
    double worst = 0.0;
    for (std::size_t i = 0; i < 32; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(signal[i]) - head[i]));
    CHECK(worst <= 1e-7);
}

TEST_CASE("mel: filterbank weights match librosa goldens within 1e-6") {
    const std::string text = goldenText("mel_filterbank.json");
    const std::vector<GoldenBank> banks = readFilterbankGolden(text);
    REQUIRE(banks.size() == 3);
    for (const GoldenBank& bank : banks) {
        MelConfig cfg;
        cfg.htk = bank.htk;
        cfg.norm = bank.norm;
        const MelFilterbank fb(cfg);
        REQUIRE(fb.nMels() == kNMels);
        REQUIRE(fb.bins() == kBins);
        double worst = 0.0;
        for (std::size_t m = 0; m < kNMels; ++m)
            for (std::size_t k = 0; k < kBins; ++k)
                worst = std::max(worst, std::abs(static_cast<double>(fb.weights()[m * kBins + k]) - bank.dense[m][k]));
        CHECK(worst <= 1e-6);
    }
}

TEST_CASE("mel: an empty filter row yields 0 and log-mel -100 dB") {
    // With 40 mels over a 32-point FFT the lowest bands collapse onto the
    // same edge, so many rows are empty (librosa warns "Empty filters
    // detected" for the same config). Which rows are exactly zero differs
    // between librosa's float32 edges and the C++ double edges — a few
    // low rows carry slivers of ~1e-5 in the C++ build — so the test finds
    // the empty rows from the C++ weights themselves rather than pinning
    // librosa's row indices, and asserts the empty-row behaviour on those.
    MelConfig cfg;
    cfg.fftSize = 32;
    cfg.nMels = 40;
    const MelFilterbank fb(cfg);
    REQUIRE(fb.bins() == 17);
    std::vector<std::size_t> emptyRows;
    for (std::size_t m = 0; m < fb.nMels(); ++m) {
        bool allZero = true;
        for (std::size_t k = 0; k < fb.bins(); ++k)
            allZero = allZero && fb.weights()[m * fb.bins() + k] == 0.0f;
        if (allZero)
            emptyRows.push_back(m);
    }
    REQUIRE(!emptyRows.empty());
    std::vector<float> power(fb.bins(), 1.0f);
    std::vector<float> mel(fb.nMels(), 0.0f);
    fb.apply(power.data(), mel.data());
    Mfcc mfcc(cfg, 13); // logMel is non-const (it reuses scratch buffers)
    std::vector<float> logMel(fb.nMels(), 0.0f);
    mfcc.logMel(mel.data(), logMel.data());
    for (const std::size_t m : emptyRows) {
        CHECK(mel[m] == 0.0f);
        CHECK(logMel[m] == doctest::Approx(-100.0));
    }
}

TEST_CASE("mel: log-mel and MFCC match librosa goldens on every frame") {
    const std::string text = goldenText("mfcc_golden.json");
    const auto frames = static_cast<std::size_t>(readScalar(text, "frames"));
    const std::vector<double> logmelGolden = readArray(text, findKey(text, "logmel"));
    const std::vector<double> mfccGolden = readArray(text, findKey(text, "mfcc"));
    REQUIRE(logmelGolden.size() == frames * kNMels);
    REQUIRE(mfccGolden.size() == frames * kNMfcc);

    const std::vector<float> signal = sig::goldenSignal(kFs);
    const std::vector<std::vector<float>> power = goldenPowerFrames(signal);
    REQUIRE(power.size() == frames);

    Mfcc mfcc{ MelConfig{} };
    std::vector<float> logmelOut(kNMels);
    std::vector<float> mfccOut(kNMfcc);
    double worstLogmel = 0.0; // as a fraction of the allowed tolerance
    double worstMfcc = 0.0;
    for (std::size_t t = 0; t < frames; ++t) {
        mfcc.logMel(power[t].data(), logmelOut.data());
        mfcc.compute(power[t].data(), mfccOut.data());
        for (std::size_t m = 0; m < kNMels; ++m) {
            const double golden = logmelGolden[t * kNMels + m];
            const double tol = 1e-3 * std::max(1.0, std::abs(golden));
            worstLogmel = std::max(worstLogmel, std::abs(static_cast<double>(logmelOut[m]) - golden) / tol);
        }
        for (std::size_t j = 0; j < kNMfcc; ++j) {
            const double golden = mfccGolden[t * kNMfcc + j];
            const double tol = 1e-3 * std::max(1.0, std::abs(golden));
            worstMfcc = std::max(worstMfcc, std::abs(static_cast<double>(mfccOut[j]) - golden) / tol);
        }
    }
    // <= 1.0 means every value satisfied |delta| <= 1e-3 * max(1, |golden|).
    CHECK(worstLogmel <= 1.0);
    CHECK(worstMfcc <= 1.0);
}

TEST_CASE("mel: silence floors at -100 dB log-mel and a finite MFCC") {
    // power_to_db with amin=1e-10: an all-zero power spectrum gives
    // 10*log10(1e-10) = -100 in every band; the DCT of a constant vector
    // is (-100 * sqrt(nMels), 0, 0, ...).
    Mfcc mfcc{ MelConfig{} };
    const std::vector<float> zeros(kBins, 0.0f);
    std::vector<float> logmelOut(kNMels);
    std::vector<float> mfccOut(kNMfcc);
    mfcc.logMel(zeros.data(), logmelOut.data());
    for (const float v : logmelOut)
        CHECK(v == doctest::Approx(-100.0));
    mfcc.compute(zeros.data(), mfccOut.data());
    CHECK(mfccOut[0] == doctest::Approx(-100.0 * std::sqrt(static_cast<double>(kNMels))));
    for (std::size_t j = 1; j < kNMfcc; ++j)
        CHECK(std::abs(mfccOut[j]) < 1e-3f);
}

TEST_CASE("mel: no allocation in apply, compute or logMel") {
    MelFilterbank fb{ MelConfig{} };
    Mfcc mfcc{ MelConfig{} };
    const std::vector<float> power(kBins, 0.25f);
    std::vector<float> melOut(kNMels);
    std::vector<float> logmelOut(kNMels);
    std::vector<float> mfccOut(kNMfcc);
    // Warm up outside the guard, then measure.
    fb.apply(power.data(), melOut.data());
    mfcc.compute(power.data(), mfccOut.data());
    mfcc.logMel(power.data(), logmelOut.data());
    AllocGuard guard;
    fb.apply(power.data(), melOut.data());
    mfcc.compute(power.data(), mfccOut.data());
    mfcc.logMel(power.data(), logmelOut.data());
    CHECK(guard.count() == 0);
}

TEST_CASE("mel: micro-benchmark (report only)") {
    const std::vector<float> signal = sig::goldenSignal(kFs);
    const std::vector<std::vector<float>> power = goldenPowerFrames(signal);
    Mfcc mfcc{ MelConfig{} };
    std::vector<float> out(kNMfcc);
    constexpr int kWarmup = 200;
    constexpr int kBatchIterations = 2000;
    constexpr int kBatches = 7;
    for (int i = 0; i < kWarmup; ++i)
        mfcc.compute(power[0].data(), out.data());
    double best = std::numeric_limits<double>::max();
    for (int batch = 0; batch < kBatches; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kBatchIterations; ++i)
            mfcc.compute(power[static_cast<std::size_t>(i) % power.size()].data(), out.data());
        const auto stop = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::micro>(stop - start).count() / kBatchIterations);
    }
    std::printf("[bench] Mfcc budget (spec S6): <= 40 us per frame (2048 FFT, 40 mels)\n");
    std::printf("[bench] Mfcc::compute: %.3f us/frame\n", best);
    CHECK(best > 0.0);
    CHECK(best < 2000.0); // loose sanity bound only; the budget is report-only
}
