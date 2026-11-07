#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dsptb {

// A basic stereo noise gate for the microphone (real-time safe: no allocation, no locks).
//
// It measures the short-term RMS of the louder channel (power smoothed with 4 ms attack and 12 ms release) so
// that single noise spikes (random noise has peaks about 10 dB above its RMS) do not open it and steady
// background noise sits at its true level, below the close threshold. The gate OPENS when that level rises
// above the threshold and CLOSES when it has stayed below (threshold - hysteresis) for the hold time. The
// gain follows open/closed with short smoothing (a few ms to open, tens of ms to close), so there are no
// clicks. There is no look-ahead: the first couple of milliseconds of a loud onset are slightly softened.
class NoiseGate {
public:
    static constexpr float kDefaultThresholdDb = -50.0f;

    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate;
        detectorAttack_ = coefficient(0.004);
        detectorRelease_ = coefficient(0.012);
        gainAttack_ = coefficient(0.002);
        gainRelease_ = coefficient(0.080);
        holdSamples_ = static_cast<uint32_t>(0.150 * sampleRate);
        reset();
    }

    void reset() noexcept {
        power_ = 0.0f;
        envelope_ = 0.0f;
        gain_ = 0.0f;
        open_ = false;
        hold_ = 0;
    }

    void setThresholdDb(float db) noexcept {
        openThreshold_ = std::pow(10.0f, db / 20.0f);
        closeThreshold_ = openThreshold_ * 0.5f; // 6 dB of hysteresis stops chattering around the threshold
    }

    // Processes n frames in place.
    void process(float* l, float* r, uint32_t n) noexcept {
        for (uint32_t i = 0; i < n; ++i) {
            const float x2 = std::max(l[i] * l[i], r[i] * r[i]);
            const float c = x2 > power_ ? detectorAttack_ : detectorRelease_;
            power_ = x2 + (power_ - x2) * c;
            envelope_ = std::sqrt(power_);

            if (!open_) {
                if (envelope_ > openThreshold_) {
                    open_ = true;
                    hold_ = holdSamples_;
                }
            } else if (envelope_ > closeThreshold_) {
                hold_ = holdSamples_; // still loud enough: keep it open
            } else if (hold_ > 0) {
                --hold_;
            } else {
                open_ = false;
            }

            const float target = open_ ? 1.0f : 0.0f;
            const float coeff = open_ ? gainAttack_ : gainRelease_;
            gain_ += (target - gain_) * (1.0f - coeff);
            if (gain_ > 0.9999f && open_)
                gain_ = 1.0f;
            else if (gain_ < 1e-5f && !open_)
                gain_ = 0.0f;

            l[i] *= gain_;
            r[i] *= gain_;
        }
    }

    bool isOpen() const noexcept { return open_; }
    float gain() const noexcept { return gain_; }

private:
    // One-pole coefficient for a time constant in seconds: x += (1 - c) * (target - x).
    float coefficient(double seconds) const noexcept {
        return seconds > 0.0 ? static_cast<float>(std::exp(-1.0 / (seconds * sampleRate_))) : 0.0f;
    }

    double sampleRate_ = 48000.0;
    float detectorAttack_ = 0.98f;
    float detectorRelease_ = 0.999f;
    float gainAttack_ = 0.99f;
    float gainRelease_ = 0.999f;
    uint32_t holdSamples_ = 7200;
    float openThreshold_ = 0.00316f; // -50 dB
    float closeThreshold_ = 0.00158f;

    float power_ = 0.0f;
    float envelope_ = 0.0f;
    float gain_ = 0.0f;
    bool open_ = false;
    uint32_t hold_ = 0;
};

} // namespace dsptb
