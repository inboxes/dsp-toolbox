#include "correction_smoother.hpp"

#include <algorithm>
#include <cmath>

namespace dsptb::autotune {

namespace {

constexpr float kLimitCents = 300.0f;
constexpr float kUnvoicedTauMs = 20.0f;
constexpr float kMaxTauMs = 200.0f;
constexpr float kFullAmountAt = 0.6f; // full correction from this Strength up

} // namespace

void CorrectionSmoother::prepare(float hopMs) noexcept {
    hopMs_ = hopMs;
    reset();
}

void CorrectionSmoother::reset() noexcept {
    smoothed_ = 0.0f;
    wasVoiced_ = false;
}

float CorrectionSmoother::ratio() const noexcept {
    return std::pow(2.0f, smoothed_ / 1200.0f);
}

float CorrectionSmoother::update(bool voiced, float errorCents, float strengthPercent) noexcept {
    if (!voiced) {
        // Never hold an old shift through a consonant or a breath.
        smoothed_ *= std::exp(-hopMs_ / kUnvoicedTauMs);
        wasVoiced_ = false;
        return smoothed_;
    }
    if (!wasVoiced_)
        smoothed_ = 0.0f; // a note's attack starts natural; at full Strength the next line snaps anyway
    wasVoiced_ = true;

    const float s = std::clamp(strengthPercent / 100.0f, 0.0f, 1.0f);
    const float amount = std::min(s / kFullAmountAt, 1.0f);
    // A slow smoother can't follow 5-7 Hz vibrato, so low Strength corrects the average pitch and keeps the wobble.
    const float tauMs = kMaxTauMs * (1.0f - s) * (1.0f - s);
    const float alpha = tauMs <= 0.0f ? 1.0f : 1.0f - std::exp(-hopMs_ / tauMs);
    smoothed_ += alpha * (amount * errorCents - smoothed_);
    smoothed_ = std::clamp(smoothed_, -kLimitCents, kLimitCents);
    return smoothed_;
}

} // namespace dsptb::autotune
