#include "pitch_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace dsptb::autotune {

namespace {

constexpr int kMaxOctaveHoldHops = 3;
constexpr float kOctaveToleranceCents = 50.0f;

bool isOctaveJump(float from, float to) noexcept {
    const float cents = 1200.0f * std::log2(to / from);
    return std::fabs(std::fabs(cents) - 1200.0f) <= kOctaveToleranceCents;
}

} // namespace

void PitchTracker::reset() noexcept {
    count_ = next_ = heldHops_ = 0;
    previous_ = 0.0f;
}

float PitchTracker::median() const noexcept {
    if (count_ == 1)
        return history_[0];
    if (count_ == 2)
        return 0.5f * (history_[0] + history_[1]);
    return std::max(std::min(history_[0], history_[1]), std::min(std::max(history_[0], history_[1]), history_[2]));
}

TrackedPitch PitchTracker::update(float hz, bool detectorVoiced, float hopRmsDb,
                                  const PitchTrackerConfig& config) noexcept {
    if (hopRmsDb < config.silenceDb || !detectorVoiced || !(hz >= config.minHz && hz <= config.maxHz)) {
        reset();
        return {};
    }
    // A pitch tracker's usual mistake is a jump of one octave for a few hops. A real one lasts longer.
    if (previous_ > 0.0f && isOctaveJump(previous_, hz)) {
        if (heldHops_ < kMaxOctaveHoldHops) {
            ++heldHops_;
            hz = previous_;
        } else {
            count_ = next_ = 0; // the new octave is real, and the old values would only slow it down
            heldHops_ = 0;
        }
    } else {
        heldHops_ = 0;
    }
    previous_ = hz;
    history_[next_] = hz;
    next_ = (next_ + 1) % 3;
    count_ = std::min(count_ + 1, 3);

    TrackedPitch out;
    out.voiced = true;
    out.f0Hz = median();
    out.midi = 69.0f + 12.0f * std::log2(out.f0Hz / config.refA4);
    return out;
}

} // namespace dsptb::autotune
