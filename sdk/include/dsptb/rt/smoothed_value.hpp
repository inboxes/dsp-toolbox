#pragma once

#include <cmath>

namespace dsptb::rt {

// Per-sample parameter smoothing. The time comes from a parameter's `smoothing_ms`.
// RT-safe: no allocation, no locks. Not thread-safe: use it on the audio thread only.
class SmoothedValue {
public:
    enum class Kind { Linear, OnePole };

    // Jumps to `initial` with no ramp. rampMs <= 0 means "no smoothing" (setTarget jumps).
    void reset(double sampleRate, double rampMs, float initial, Kind kind = Kind::Linear) noexcept {
        kind_ = kind;
        rampSamples_ = rampMs > 0.0 ? rampMs * 0.001 * sampleRate : 0.0;
        // One-pole: time constant = rampMs, so ~63 % of the way after rampMs.
        coeff_ = rampSamples_ > 0.0 ? static_cast<float>(1.0 - std::exp(-1.0 / rampSamples_)) : 1.0f;
        current_ = target_ = initial;
        step_ = 0.0f;
        remaining_ = 0;
    }

    void setTarget(float target) noexcept {
        if (target == target_)
            return;
        target_ = target;
        if (rampSamples_ <= 0.0) {
            current_ = target_;
            remaining_ = 0;
            return;
        }
        if (kind_ == Kind::Linear) {
            remaining_ = static_cast<int>(rampSamples_) > 0 ? static_cast<int>(rampSamples_) : 1;
            step_ = (target_ - current_) / static_cast<float>(remaining_);
        }
    }

    // Advances one sample and returns the value. Once settled it returns exactly the target.
    float next() noexcept {
        if (kind_ == Kind::Linear) {
            if (remaining_ > 0) {
                current_ += step_;
                if (--remaining_ == 0)
                    current_ = target_; // land exactly, no float drift
            }
        } else if (current_ != target_) {
            current_ += (target_ - current_) * coeff_;
            if (std::fabs(target_ - current_) < 1e-5f)
                current_ = target_; // below -100 dB: snap (float cannot resolve the tail)
        }
        return current_;
    }

    // Fills a block with successive values.
    void fill(float* out, int n) noexcept {
        for (int i = 0; i < n; ++i)
            out[i] = next();
    }

    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return current_ != target_; }

private:
    Kind kind_ = Kind::Linear;
    double rampSamples_ = 0.0;
    float coeff_ = 1.0f;
    float current_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 0.0f;
    int remaining_ = 0;
};

} // namespace dsptb::rt
