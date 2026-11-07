#include "pitch_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace dsptb::autotune {

namespace {

constexpr int kMaxHoldHops = 3;
constexpr float kJumpCents = 150.0f;

// A tracker's usual mistake is a jump to a harmonic or a subharmonic for a few hops: an octave, but also a fifth
// or a twelfth. A sung note change lasts longer.
bool isJump(float from, float to) noexcept {
    return std::fabs(1200.0f * std::log2(to / from)) > kJumpCents;
}

} // namespace

void PitchTracker::reset() noexcept {
    heldHops_ = 0;
    previous_ = 0.0f;
}

TrackedPitch PitchTracker::update(float hz, bool detectorVoiced, float hopRmsDb,
                                  const PitchTrackerConfig& config) noexcept {
    if (hopRmsDb < config.silenceDb || !detectorVoiced || !(hz >= config.minHz && hz <= config.maxHz)) {
        reset();
        return {};
    }
    // No median filter: it would delay every estimate by a hop, and a late correction deepens a wobble.
    if (previous_ > 0.0f && isJump(previous_, hz) && heldHops_ < kMaxHoldHops) {
        ++heldHops_;
        hz = previous_;
    } else {
        heldHops_ = 0;
    }
    previous_ = hz;

    TrackedPitch out;
    out.voiced = true;
    out.f0Hz = hz;
    out.midi = 69.0f + 12.0f * std::log2(out.f0Hz / config.refA4);
    return out;
}

} // namespace dsptb::autotune
