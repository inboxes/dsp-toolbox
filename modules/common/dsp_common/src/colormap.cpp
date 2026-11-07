#include <dsptb/dsp_common/colormap.hpp>

// Generated Magma / Viridis / Inferno tables. The file opens
// namespace dsptb::dsp::detail itself, so it is included at file scope.
#include "colormap_data.inl"

namespace dsptb::dsp {
namespace {

// Grey is linear by definition, so its table is built at compile
// time instead of being generated: entry i packs (i, i, i, 255).
constexpr std::array<std::uint32_t, 256> makeGreyLut() noexcept {
    std::array<std::uint32_t, 256> lut{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        lut[i] = i | (i << 8) | (i << 16) | (0xFFu << 24);
    }
    return lut;
}

constexpr std::array<std::uint32_t, 256> kGreyLut = makeGreyLut();

// Nearest LUT index for a normalised value. NaN fails every comparison, so
// the first branch maps it to entry 0 without a separate isnan check.
std::size_t lutIndex(float t01) noexcept {
    if (!(t01 > 0.0f)) {
        return 0;
    }
    if (t01 >= 1.0f) {
        return 255;
    }
    // Nearest entry: t = i/255 is entry i, so round rather than truncate.
    // The value is positive here, so the truncating cast rounds half up.
    return static_cast<std::size_t>(t01 * 255.0f + 0.5f);
}

} // namespace

const std::array<std::uint32_t, 256>& colormapLut(Colormap map) noexcept {
    switch (map) {
    case Colormap::Magma:
        return detail::kMagmaLut;
    case Colormap::Viridis:
        return detail::kViridisLut;
    case Colormap::Inferno:
        return detail::kInfernoLut;
    case Colormap::Grey:
        return kGreyLut;
    }
    return kGreyLut; // unreachable for valid enum values; keeps -Wreturn-type quiet
}

std::uint32_t colormapRgba(Colormap map, float t01) noexcept {
    return colormapLut(map)[lutIndex(t01)];
}

void dbToRgba(const float* db, std::size_t n, float minDb, float maxDb, Colormap map,
              std::uint32_t* out) noexcept {
    if (db == nullptr || out == nullptr) {
        return;
    }
    const std::array<std::uint32_t, 256>& lut = colormapLut(map);
    if (!(maxDb > minDb)) {
        // Degenerate range: step at maxDb (NaN still maps to entry 0).
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = (db[i] >= maxDb) ? lut[255] : lut[0];
        }
        return;
    }
    const float scale = 1.0f / (maxDb - minDb);
    for (std::size_t i = 0; i < n; ++i) {
        // NaN propagates through the multiply and lutIndex maps it to 0;
        // values outside [minDb, maxDb] clamp in lutIndex.
        out[i] = lut[lutIndex((db[i] - minDb) * scale)];
    }
}

} // namespace dsptb::dsp
