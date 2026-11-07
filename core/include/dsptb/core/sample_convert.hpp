#pragma once

// Conversion between the engine's float samples and the integer/float formats an audio driver's buffers use
// (little-endian only). Real-time safe: no allocation, no locks. Used by the ASIO backend and its tests.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dsptb {

enum class WireFormat {
    Int16, // 16-bit
    Int24, // 24-bit packed in 3 bytes
    Int32, // 32-bit
    Float32,
    Float64,
    Int32In16, // 32-bit container, 16 significant bits in the low bits (sign-extended)
    Int32In18,
    Int32In20,
    Int32In24
};

inline int wireSignificantBits(WireFormat f) noexcept {
    switch (f) {
    case WireFormat::Int32In16:
        return 16;
    case WireFormat::Int32In18:
        return 18;
    case WireFormat::Int32In20:
        return 20;
    case WireFormat::Int32In24:
        return 24;
    default:
        return 32;
    }
}

namespace detail {
inline float clamp1(float v) noexcept {
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}
} // namespace detail

// Writes `frames` samples taken from `src[i * stride]` (stride 2 for an interleaved stereo bus) to `dst`.
inline void writeWire(WireFormat f, void* dst, const float* src, uint32_t frames, uint32_t stride) noexcept {
    using detail::clamp1;
    switch (f) {
    case WireFormat::Float32: {
        auto* d = static_cast<float*>(dst);
        for (uint32_t i = 0; i < frames; ++i)
            d[i] = src[static_cast<size_t>(i) * stride];
        break;
    }
    case WireFormat::Float64: {
        auto* d = static_cast<double*>(dst);
        for (uint32_t i = 0; i < frames; ++i)
            d[i] = static_cast<double>(src[static_cast<size_t>(i) * stride]);
        break;
    }
    case WireFormat::Int16: {
        auto* d = static_cast<int16_t*>(dst);
        for (uint32_t i = 0; i < frames; ++i)
            d[i] = static_cast<int16_t>(std::lrintf(clamp1(src[static_cast<size_t>(i) * stride]) * 32767.0f));
        break;
    }
    case WireFormat::Int24: {
        auto* d = static_cast<uint8_t*>(dst);
        for (uint32_t i = 0; i < frames; ++i) {
            const int32_t v =
                static_cast<int32_t>(std::lrintf(clamp1(src[static_cast<size_t>(i) * stride]) * 8388607.0f));
            d[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
            d[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            d[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        }
        break;
    }
    default: { // Int32 and the padded variants
        const double scale = static_cast<double>((1LL << (wireSignificantBits(f) - 1)) - 1);
        auto* d = static_cast<int32_t*>(dst);
        for (uint32_t i = 0; i < frames; ++i)
            d[i] = static_cast<int32_t>(
                std::llrint(static_cast<double>(clamp1(src[static_cast<size_t>(i) * stride])) * scale));
        break;
    }
    }
}

// Reads `frames` samples from `src` into `dst[i * stride]`.
inline void readWire(WireFormat f, const void* src, float* dst, uint32_t frames, uint32_t stride) noexcept {
    switch (f) {
    case WireFormat::Float32: {
        const auto* s = static_cast<const float*>(src);
        for (uint32_t i = 0; i < frames; ++i)
            dst[static_cast<size_t>(i) * stride] = s[i];
        break;
    }
    case WireFormat::Float64: {
        const auto* s = static_cast<const double*>(src);
        for (uint32_t i = 0; i < frames; ++i)
            dst[static_cast<size_t>(i) * stride] = static_cast<float>(s[i]);
        break;
    }
    case WireFormat::Int16: {
        const auto* s = static_cast<const int16_t*>(src);
        for (uint32_t i = 0; i < frames; ++i)
            dst[static_cast<size_t>(i) * stride] = static_cast<float>(s[i]) * (1.0f / 32768.0f);
        break;
    }
    case WireFormat::Int24: {
        const auto* s = static_cast<const uint8_t*>(src);
        for (uint32_t i = 0; i < frames; ++i) {
            // Put the 3 bytes in the top of an int32 and shift down: that sign-extends.
            const uint32_t u = (static_cast<uint32_t>(s[i * 3 + 0]) << 8) |
                               (static_cast<uint32_t>(s[i * 3 + 1]) << 16) |
                               (static_cast<uint32_t>(s[i * 3 + 2]) << 24);
            dst[static_cast<size_t>(i) * stride] =
                static_cast<float>(static_cast<int32_t>(u) >> 8) * (1.0f / 8388608.0f);
        }
        break;
    }
    default: { // Int32 and the padded variants
        const double scale = 1.0 / static_cast<double>(1LL << (wireSignificantBits(f) - 1));
        const auto* s = static_cast<const int32_t*>(src);
        for (uint32_t i = 0; i < frames; ++i)
            dst[static_cast<size_t>(i) * stride] = static_cast<float>(static_cast<double>(s[i]) * scale);
        break;
    }
    }
}

} // namespace dsptb
