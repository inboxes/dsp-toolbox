#include <doctest/doctest.h>

#include <dsptb/dsp_common/freq_map.hpp>

#include "support/alloc_counter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dsptb::dsp;
using dsptb::test::AllocGuard;

namespace {

// Relative comparison with an absolute floor, so an expected 0 only
// accepts an actual 0 (0 has no meaningful relative error).
bool withinRel(double actual, double expected, double relTol) {
    if (expected == 0.0)
        return actual == 0.0;
    return std::abs(actual - expected) <= relTol * std::abs(expected);
}

// Minimal extractor for the flat number arrays in the golden JSON:
// finds "key", then reads every number inside the following [ ... ].
std::vector<double> goldenArray(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    REQUIRE(keyPos != std::string::npos);
    const std::size_t open = text.find('[', keyPos);
    const std::size_t close = text.find(']', open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    std::vector<double> values;
    const char* cursor = text.c_str() + open + 1;
    const char* const end = text.c_str() + close;
    while (cursor < end) {
        char* next = nullptr;
        const double value = std::strtod(cursor, &next);
        if (next == cursor) {
            ++cursor; // separator (comma / whitespace)
            continue;
        }
        values.push_back(value);
        cursor = next;
    }
    return values;
}

std::string goldenText() {
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "golden" / "mel_conversions.json";
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot open golden file: ", path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

constexpr double kSampleRate = 48000.0;
constexpr std::size_t kFftSize = 4096;
constexpr std::size_t kBins = kFftSize / 2 + 1;

} // namespace

TEST_CASE("freq_map: Slaney and HTK mel conversions match the librosa goldens") {
    const std::string text = goldenText();
    const std::vector<double> freqs = goldenArray(text, "frequencies_hz");
    const std::vector<double> slaney = goldenArray(text, "slaney_mel");
    const std::vector<double> htk = goldenArray(text, "htk_mel");

    // The acceptance frequencies.
    REQUIRE(freqs.size() == 7);
    REQUIRE(slaney.size() == freqs.size());
    REQUIRE(htk.size() == freqs.size());
    const double expectedFreqs[] = {0.0, 100.0, 999.0, 1000.0, 1001.0, 4000.0, 20000.0};
    for (std::size_t i = 0; i < freqs.size(); ++i)
        CHECK(freqs[i] == doctest::Approx(expectedFreqs[i]));

    for (std::size_t i = 0; i < freqs.size(); ++i) {
        CAPTURE(freqs[i]);
        CHECK(withinRel(hzToMelSlaney(freqs[i]), slaney[i], 1e-6));
        CHECK(withinRel(hzToMelHtk(freqs[i]), htk[i], 1e-6));
        // The inverse maps take the golden mel values back to the Hz.
        CHECK(withinRel(melToHzSlaney(slaney[i]), freqs[i], 1e-6));
        CHECK(withinRel(melToHzHtk(htk[i]), freqs[i], 1e-6));
    }
}

TEST_CASE("freq_map: mel conversions round-trip within 1e-9 absolute") {
    const double freqs[] = {0.0, 100.0, 999.0, 1000.0, 1001.0, 4000.0, 20000.0};
    for (const double hz : freqs) {
        CAPTURE(hz);
        const double slaneyBack = melToHzSlaney(hzToMelSlaney(hz));
        const double htkBack = melToHzHtk(hzToMelHtk(hz));
        CHECK(std::abs(slaneyBack - hz) <= 1e-9);
        CHECK(std::abs(htkBack - hz) <= 1e-9);
    }
}

TEST_CASE("freq_map: linear map with rows == bins over the full range is the identity") {
    BinToRowMap map(kFftSize, kSampleRate, kBins, FreqScale::Linear, 0.0, kSampleRate / 2.0);
    std::vector<float> binDb(kBins);
    std::vector<float> rowDb(kBins, -999.0f);
    for (std::size_t i = 0; i < kBins; ++i)
        binDb[i] = static_cast<float>(static_cast<int>(i % 251) - 125) * 0.5f;
    map.apply(binDb.data(), rowDb.data());
    for (std::size_t i = 0; i < kBins; ++i) {
        CAPTURE(i);
        CHECK(rowDb[i] == doctest::Approx(binDb[i]));
    }
}

TEST_CASE("freq_map: log map puts a 1 kHz peak in the row containing 1 kHz and fills every row") {
    constexpr std::size_t kRows = 512;
    BinToRowMap map(kFftSize, kSampleRate, kRows, FreqScale::Log, 20.0, 20000.0);
    const double binHz = kSampleRate / static_cast<double>(kFftSize);

    std::vector<float> binDb(kBins, -80.0f);
    // The bin nearest 1 kHz carries the single-bin peak.
    const std::size_t peakBin = static_cast<std::size_t>(std::llround(1000.0 / binHz));
    binDb[peakBin] = 0.0f;

    std::vector<float> rowDb(kRows, -999.0f);
    map.apply(binDb.data(), rowDb.data());

    for (std::size_t row = 0; row < kRows; ++row) {
        CAPTURE(row);
        CHECK(std::isfinite(rowDb[row]));
    }

    const auto peakRow = static_cast<std::size_t>(
        std::max_element(rowDb.begin(), rowDb.end()) - rowDb.begin());
    CHECK(rowDb[peakRow] == doctest::Approx(0.0f));

    // Recompute the row's [lo, hi) independently: log edges are evenly
    // spaced, so edge(r) = 20 * (20000/20)^(r/512).
    const double lo = 20.0 * std::pow(1000.0, static_cast<double>(peakRow) / 512.0);
    const double hi = 20.0 * std::pow(1000.0, static_cast<double>(peakRow + 1) / 512.0);
    CHECK(lo <= 1000.0);
    CHECK(1000.0 < hi);
    // The peak bin's own centre frequency falls in the same row.
    const double peakBinHz = static_cast<double>(peakBin) * binHz;
    CHECK(lo <= peakBinHz);
    CHECK(peakBinHz < hi);
}

TEST_CASE("freq_map: rows narrower than a bin are interpolated monotonically between bins") {
    constexpr std::size_t kRows = 512;
    BinToRowMap map(kFftSize, kSampleRate, kRows, FreqScale::Log, 20.0, 20000.0);
    const double binHz = kSampleRate / static_cast<double>(kFftSize);

    // A ramp across bins: the interpolated value at a row centre is the
    // ramp value at that centre's fractional bin position.
    std::vector<float> binDb(kBins);
    for (std::size_t i = 0; i < kBins; ++i)
        binDb[i] = static_cast<float>(i);
    std::vector<float> rowDb(kRows, -999.0f);
    map.apply(binDb.data(), rowDb.data());

    // Row 0 spans [20, ~20.27) Hz and holds no bin centre (bins sit at
    // 11.72 and 23.44 Hz), so it must be the linear interpolation of
    // bins 1 and 2 at the row centre (the geometric mean on a log axis).
    const double lo = 20.0;
    const double hi = 20.0 * std::pow(1000.0, 1.0 / 512.0);
    const double center = std::sqrt(lo * hi);
    const double pos = center / binHz;
    const double frac = pos - std::floor(pos);
    const auto lowerBin = static_cast<std::size_t>(std::floor(pos));
    const double expected =
        (1.0 - frac) * static_cast<double>(binDb[lowerBin]) + frac * static_cast<double>(binDb[lowerBin + 1]);
    CHECK(static_cast<double>(rowDb[0]) == doctest::Approx(expected).epsilon(1e-6));

    // Along the low (interpolated) rows the output rises strictly with
    // the row centre, i.e. interpolation is monotonic between two bins.
    for (std::size_t row = 1; row < 32; ++row) {
        CAPTURE(row);
        CHECK(rowDb[row] > rowDb[row - 1]);
    }
}

TEST_CASE("freq_map: every in-range bin belongs to exactly one row in a non-trivial log map") {
    // A larger FFT at a non-power-of-two sample rate makes the log-row
    // edges land at less regular positions than the 48 kHz cases above.
    // apply()'s per-row maximum gives a direct membership test: with
    // only one bin raised, exactly one row may return that bin's value.
    // A dropped bin returns none and a double-counted bin returns two.
    constexpr std::size_t kFftSize8192 = 8192;
    constexpr std::size_t kBins8192 = kFftSize8192 / 2 + 1;
    constexpr double kSampleRate441 = 44100.0;
    constexpr double kMinHz = 20.0;
    constexpr double kMaxHz = 20000.0;
    constexpr std::size_t kRows = 512;
    BinToRowMap map(kFftSize8192, kSampleRate441, kRows, FreqScale::Log, kMinHz, kMaxHz);
    const double binSpacing = kSampleRate441 / static_cast<double>(kFftSize8192);

    std::vector<float> binDb(kBins8192, -80.0f);
    std::vector<float> rowDb(kRows, -999.0f);
    for (std::size_t bin = 0; bin < kBins8192; ++bin) {
        const double hz = static_cast<double>(bin) * binSpacing;
        if (hz < kMinHz || hz > kMaxHz)
            continue;
        std::fill(binDb.begin(), binDb.end(), -80.0f);
        binDb[bin] = 0.0f;
        map.apply(binDb.data(), rowDb.data());
        std::size_t memberships = 0;
        for (const float value : rowDb) {
            if (value == 0.0f)
                ++memberships;
        }
        CAPTURE(bin);
        CAPTURE(hz);
        CHECK(memberships == 1u);
    }
}

TEST_CASE("freq_map: a row centred exactly on a bin returns that bin's value") {
    // With 1024 linear rows over 0..24 kHz, row 0 is centred at
    // 11.71875 Hz, exactly bin 1. Its neighbours carry different values,
    // so this pins the exact-centre boundary in apply(): the result is
    // bin 1's own value, not a blend toward either side. (Bin 2 sits
    // exactly on row 0's upper edge and is excluded.)
    BinToRowMap map(kFftSize, kSampleRate, 1024, FreqScale::Linear, 0.0, kSampleRate / 2.0);
    std::vector<float> binDb(kBins, -80.0f);
    binDb[0] = 12.0f;
    binDb[1] = 34.0f;
    binDb[2] = 56.0f;
    std::vector<float> rowDb(1024, -999.0f);
    map.apply(binDb.data(), rowDb.data());
    CHECK(map.rowCenterHz(0) == doctest::Approx(kSampleRate / kFftSize));
    CHECK(rowDb[0] == doctest::Approx(34.0f));
}

TEST_CASE("freq_map: rowCenterHz is strictly increasing and rowToFraction hits 0 and 1") {
    struct Case {
        FreqScale scale;
        double minHz;
        double maxHz;
        std::size_t rows;
    };
    const Case cases[] = {
        {FreqScale::Linear, 0.0, 24000.0, 128},
        {FreqScale::Log, 20.0, 20000.0, 512},
        {FreqScale::Mel, 0.0, 20000.0, 256},
        {FreqScale::Mel, 20.0, 20000.0, 256},
    };
    for (const Case& c : cases) {
        BinToRowMap map(kFftSize, kSampleRate, c.rows, c.scale, c.minHz, c.maxHz);
        double previous = map.rowCenterHz(0);
        CHECK(previous > c.minHz);
        CHECK(previous < c.maxHz);
        for (std::size_t row = 1; row < c.rows; ++row) {
            const double center = map.rowCenterHz(row);
            CAPTURE(row);
            CHECK(center > previous);
            previous = center;
        }
        CHECK(map.rowToFraction(c.minHz) == doctest::Approx(0.0));
        CHECK(map.rowToFraction(c.maxHz) == doctest::Approx(1.0));
        // The middle frequency on the axis sits strictly inside (0, 1),
        // and out-of-range frequencies clamp to the ends.
        CHECK(map.rowToFraction((c.minHz + c.maxHz) / 2.0) > 0.0);
        CHECK(map.rowToFraction((c.minHz + c.maxHz) / 2.0) < 1.0);
        CHECK(map.rowToFraction(c.minHz - 1.0) == doctest::Approx(0.0));
        CHECK(map.rowToFraction(c.maxHz + 1000.0) == doctest::Approx(1.0));
    }
}

TEST_CASE("freq_map: noteName formats the nearest note and cents") {
    char buf[32] = {};
    noteName(440.0, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "A4 +0c") == 0);
    noteName(261.6256, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "C4 +0c") == 0);
    // 452.89 Hz is 49.99 cents above A4: the cents round to +50 and the
    // note stays A4 (ties round toward +).
    noteName(452.89, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "A4 +50c") == 0);
    // A sharpened note that falls short shows negative cents.
    const double cs5Minus12 = 440.0 * std::pow(2.0, (4.0 - 0.12) / 12.0);
    noteName(cs5Minus12, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "C#5 -12c") == 0);
    noteName(0.0, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "--") == 0);
    noteName(-100.0, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "--") == 0);
}

TEST_CASE("freq_map: noteName cents stay within [-50, 50] across a sweep") {
    // Sweep in 1-cent steps through an octave centred on A4. This crosses
    // every nearest-note boundary in that octave; the offsets produced run
    // -50..+49 (an exact boundary hit reports as the next note's -50 rather
    // than +50 — the +50 endpoint itself is covered by the 452.89 Hz case
    // above).
    char buf[32] = {};
    for (int offsetCents = -600; offsetCents <= 600; ++offsetCents) {
        const double hz = 440.0 * std::pow(2.0, static_cast<double>(offsetCents) / 1200.0);
        noteName(hz, buf, sizeof(buf));
        const char* const centsText = std::strrchr(buf, ' ');
        REQUIRE(centsText != nullptr);
        const long cents = std::strtol(centsText + 1, nullptr, 10);
        CAPTURE(offsetCents);
        CAPTURE(buf);
        CHECK(cents >= -50);
        CHECK(cents <= 50);
    }
}

TEST_CASE("freq_map: noteName never overruns the buffer") {
    // cap = 4 leaves room for 3 chars + NUL; the rest of the buffer
    // must be untouched.
    char buf[8];
    std::memset(buf, 'X', sizeof(buf));
    noteName(440.0, buf, 4);
    CHECK(buf[3] == '\0');
    CHECK(std::strlen(buf) <= 3);
    CHECK(std::strcmp(buf, "A4 ") == 0);
    for (std::size_t i = 4; i < sizeof(buf); ++i)
        CHECK(buf[i] == 'X');

    char tiny[1] = {'X'};
    noteName(440.0, tiny, sizeof(tiny));
    CHECK(tiny[0] == '\0');
    // cap = 0 and a null buffer are safe no-ops.
    noteName(440.0, buf, 0);
    noteName(440.0, nullptr, 8);
}

TEST_CASE("freq_map: apply() makes no allocation") {
    const BinToRowMap linear(kFftSize, kSampleRate, 256, FreqScale::Linear, 0.0, 24000.0);
    const BinToRowMap logMap(kFftSize, kSampleRate, 512, FreqScale::Log, 20.0, 20000.0);
    const BinToRowMap mel(kFftSize, kSampleRate, 256, FreqScale::Mel, 0.0, 20000.0);
    std::vector<float> binDb(kBins, -42.0f);
    std::vector<float> rowDb(512, 0.0f);
    {
        AllocGuard guard;
        linear.apply(binDb.data(), rowDb.data());
        logMap.apply(binDb.data(), rowDb.data());
        mel.apply(binDb.data(), rowDb.data());
        CHECK(guard.count() == 0);
    }
}

TEST_CASE("freq_map: invalid configurations throw invalid_argument") {
    CHECK_THROWS_AS(BinToRowMap(0, kSampleRate, 16, FreqScale::Linear, 0.0, 24000.0), std::invalid_argument);
    CHECK_THROWS_AS(BinToRowMap(kFftSize, 0.0, 16, FreqScale::Linear, 0.0, 24000.0), std::invalid_argument);
    CHECK_THROWS_AS(BinToRowMap(kFftSize, kSampleRate, 0, FreqScale::Linear, 0.0, 24000.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(BinToRowMap(kFftSize, kSampleRate, 16, FreqScale::Linear, 24000.0, 24000.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(BinToRowMap(kFftSize, kSampleRate, 16, FreqScale::Linear, -1.0, 24000.0),
                    std::invalid_argument);
    // A log axis cannot start at 0 Hz.
    CHECK_THROWS_AS(BinToRowMap(kFftSize, kSampleRate, 16, FreqScale::Log, 0.0, 20000.0),
                    std::invalid_argument);
}
