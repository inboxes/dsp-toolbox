#include <doctest/doctest.h>

#include <dsptb/dsp_common/colormap.hpp>

#include "support/alloc_counter.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

using dsptb::dsp::Colormap;
using dsptb::dsp::colormapLut;
using dsptb::dsp::colormapRgba;
using dsptb::dsp::dbToRgba;
using dsptb::test::AllocGuard;

namespace {

std::uint32_t redOf(std::uint32_t rgba) noexcept { return rgba & 0xFFu; }
std::uint32_t greenOf(std::uint32_t rgba) noexcept { return (rgba >> 8) & 0xFFu; }
std::uint32_t blueOf(std::uint32_t rgba) noexcept { return (rgba >> 16) & 0xFFu; }
std::uint32_t alphaOf(std::uint32_t rgba) noexcept { return (rgba >> 24) & 0xFFu; }

void checkRgb(std::uint32_t rgba, std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    CHECK(redOf(rgba) == r);
    CHECK(greenOf(rgba) == g);
    CHECK(blueOf(rgba) == b);
    CHECK(alphaOf(rgba) == 255u);
}

// CIE L* (CIE 1976 lightness, D65) from a packed sRGB entry: linearise the
// sRGB channels, take relative luminance Y, then the L* cube-root curve.
double cieLightness(std::uint32_t rgba) noexcept {
    const auto linearise = [](std::uint32_t channel) noexcept {
        const double c = static_cast<double>(channel) / 255.0;
        return (c <= 0.04045) ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double y = 0.2126 * linearise(redOf(rgba)) + 0.7152 * linearise(greenOf(rgba)) +
                     0.0722 * linearise(blueOf(rgba));
    constexpr double kDelta = 6.0 / 29.0;
    if (y > kDelta * kDelta * kDelta) {
        return 116.0 * std::cbrt(y) - 16.0;
    }
    return (24389.0 / 27.0) * y;
}

constexpr std::array<Colormap, 4> kAllMaps = {Colormap::Magma, Colormap::Viridis, Colormap::Inferno,
                                              Colormap::Grey};

} // namespace

TEST_CASE("colormap LUT spot checks match matplotlib (round(255*c))") {
    // These match the generated tables
    // exactly, so no correction was needed.
    checkRgb(colormapLut(Colormap::Magma)[0], 0, 0, 4);
    checkRgb(colormapLut(Colormap::Magma)[255], 252, 253, 191);
    checkRgb(colormapLut(Colormap::Viridis)[0], 68, 1, 84);
    checkRgb(colormapLut(Colormap::Viridis)[255], 253, 231, 37);
    checkRgb(colormapLut(Colormap::Inferno)[0], 0, 0, 4);
    checkRgb(colormapLut(Colormap::Inferno)[255], 252, 255, 164);
}

TEST_CASE("colormap LUTs have 256 opaque entries and stable identity") {
    for (const Colormap map : kAllMaps) {
        const std::array<std::uint32_t, 256>& lut = colormapLut(map);
        CHECK(lut.size() == 256u);
        // Same table object on every call: callers may hold the reference.
        CHECK(&colormapLut(map) == &lut);
        for (const std::uint32_t entry : lut) {
            CHECK(alphaOf(entry) == 255u);
        }
    }
}

TEST_CASE("grey colormap is linear: grey[i] = (i, i, i)") {
    const std::array<std::uint32_t, 256>& grey = colormapLut(Colormap::Grey);
    for (std::uint32_t i = 0; i < 256; ++i) {
        checkRgb(grey[i], i, i, i);
    }
}

TEST_CASE("packing is R | G<<8 | B<<16 | A<<24 (bytes R, G, B, A in memory)") {
    std::array<std::uint8_t, 4> bytes{};

    const std::uint32_t white = colormapRgba(Colormap::Grey, 1.0f);
    std::memcpy(bytes.data(), &white, 4);
    CHECK(bytes[0] == 0xFF);
    CHECK(bytes[1] == 0xFF);
    CHECK(bytes[2] == 0xFF);
    CHECK(bytes[3] == 0xFF);

    const std::uint32_t magma0 = colormapRgba(Colormap::Magma, 0.0f);
    std::memcpy(bytes.data(), &magma0, 4);
    CHECK(bytes[0] == 0x00);
    CHECK(bytes[1] == 0x00);
    CHECK(bytes[2] == 0x04);
    CHECK(bytes[3] == 0xFF);
}

TEST_CASE("colormapRgba clamps t and returns the nearest LUT entry") {
    for (const Colormap map : kAllMaps) {
        const std::array<std::uint32_t, 256>& lut = colormapLut(map);
        CHECK(colormapRgba(map, 0.0f) == lut[0]);
        CHECK(colormapRgba(map, 1.0f) == lut[255]);
        CHECK(colormapRgba(map, -3.0f) == lut[0]);
        CHECK(colormapRgba(map, 7.5f) == lut[255]);
        CHECK(colormapRgba(map, std::numeric_limits<float>::quiet_NaN()) == lut[0]);
        CHECK(colormapRgba(map, std::numeric_limits<float>::infinity()) == lut[255]);
        CHECK(colormapRgba(map, -std::numeric_limits<float>::infinity()) == lut[0]);
        // Entry i sits exactly at t = i/255, so sampling there returns it.
        for (std::size_t i = 0; i < 256; ++i) {
            CHECK(colormapRgba(map, static_cast<float>(i) / 255.0f) == lut[i]);
        }
    }
}

TEST_CASE("CIE L* of the RGBA8 tables is monotonic up to 8-bit quantisation") {
    // Maintainer ruling on issue #9: the strict L* criterion was written for
    // the continuous data and does not survive round(255*c) quantisation
    // (Viridis gains byte-identical neighbours — entries 112/113 are both
    // (38,130,142) and 129/130 are both (32,146,140) — so no function of the
    // entry bytes can strictly increase there). Strict increase is therefore
    // asserted on the unquantised float source in gen_colormaps.py. For the
    // committed RGBA8 tables this test asserts what is true of the 8-bit
    // data, without hard-coding the duplicate pairs: L* never drops by more
    // than 0.05 between adjacent entries, and it rises strictly over wider
    // spans (every 4 entries).
    for (const Colormap map : {Colormap::Magma, Colormap::Viridis, Colormap::Inferno}) {
        const std::array<std::uint32_t, 256>& lut = colormapLut(map);
        std::array<double, 256> lightness{};
        for (std::size_t i = 0; i < 256; ++i)
            lightness[i] = cieLightness(lut[i]);
        for (std::size_t i = 1; i < 256; ++i)
            CHECK(lightness[i] > lightness[i - 1] - 0.05);
        for (std::size_t i = 0; i + 4 < 256; ++i)
            CHECK(lightness[i + 4] > lightness[i]);
    }
}

TEST_CASE("dbToRgba clamps to the table ends and maps NaN to entry 0") {
    const float minDb = -100.0f;
    const float maxDb = 0.0f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 8> db = {-200.0f, -100.0f, -50.0f, 0.0f, 50.0f, nan,
                                     std::numeric_limits<float>::infinity(),
                                     -std::numeric_limits<float>::infinity()};
    for (const Colormap map : kAllMaps) {
        const std::array<std::uint32_t, 256>& lut = colormapLut(map);
        std::array<std::uint32_t, 8> out{};
        dbToRgba(db.data(), db.size(), minDb, maxDb, map, out.data());
        CHECK(out[0] == lut[0]);   // below the range
        CHECK(out[1] == lut[0]);   // exactly minDb
        CHECK(out[2] == lut[128]); // midpoint: round(0.5 * 255) = 128
        CHECK(out[3] == lut[255]); // exactly maxDb
        CHECK(out[4] == lut[255]); // above the range
        CHECK(out[5] == lut[0]);   // NaN
        CHECK(out[6] == lut[255]); // +inf
        CHECK(out[7] == lut[0]);   // -inf
        // The helper agrees with colormapRgba on the normalised value.
        CHECK(out[2] == colormapRgba(map, 0.5f));
    }
}

TEST_CASE("dbToRgba handles empty input and null pointers without effect") {
    std::array<std::uint32_t, 1> out = {0xDEADBEEFu};
    dbToRgba(nullptr, 0, -100.0f, 0.0f, Colormap::Magma, out.data());
    CHECK(out[0] == 0xDEADBEEFu);
    const std::array<float, 1> db = {-50.0f};
    dbToRgba(db.data(), db.size(), -100.0f, 0.0f, Colormap::Magma, nullptr);
    dbToRgba(db.data(), 0, -100.0f, 0.0f, Colormap::Magma, out.data());
    CHECK(out[0] == 0xDEADBEEFu);
}

TEST_CASE("colormap accessors allocate nothing (real-time safe)") {
    std::array<float, 512> db{};
    std::array<std::uint32_t, 512> out{};
    for (std::size_t i = 0; i < db.size(); ++i) {
        db[i] = -100.0f + static_cast<float>(i) * 0.25f;
    }
    AllocGuard guard;
    for (const Colormap map : kAllMaps) {
        const std::array<std::uint32_t, 256>& lut = colormapLut(map);
        CHECK(lut.size() == 256u);
        CHECK(colormapRgba(map, 0.25f) == lut[64]);
        dbToRgba(db.data(), db.size(), -100.0f, 0.0f, map, out.data());
    }
    CHECK(guard.count() == 0);
    CHECK(out[0] == colormapLut(Colormap::Grey)[0]);
    CHECK(out[511] == colormapLut(Colormap::Grey)[255]);
}
