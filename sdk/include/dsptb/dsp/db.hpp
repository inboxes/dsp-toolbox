#pragma once

#include <algorithm>
#include <cmath>

namespace dsptb {

inline float dbToGain(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}
// Silence maps to `floorDb` instead of -inf.
inline float gainToDb(float gain, float floorDb = -120.0f) noexcept {
    return gain > 1e-12f ? std::max(floorDb, 20.0f * std::log10(gain)) : floorDb;
}

} // namespace dsptb
