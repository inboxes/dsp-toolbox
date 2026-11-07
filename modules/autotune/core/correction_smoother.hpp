#pragma once

// Maps Strength to how much and how fast the voice is corrected. Pure logic: no SDK and no allocation.

namespace dsptb::autotune {

class CorrectionSmoother {
public:
    void prepare(float hopMs) noexcept;
    void reset() noexcept;

    // Runs once per hop. Returns the correction to apply now, in cents.
    float update(bool voiced, float errorCents, float strengthPercent) noexcept;
    float cents() const noexcept { return smoothed_; }
    float ratio() const noexcept;

private:
    float hopMs_ = 5.0f;
    float smoothed_ = 0.0f;
    bool wasVoiced_ = false;
};

} // namespace dsptb::autotune
