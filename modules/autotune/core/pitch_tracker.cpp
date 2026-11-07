#include "pitch_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace dsptb::autotune {

namespace {

constexpr int kMaxHoldHops = 3;
constexpr float kJumpCents = 150.0f;
// A weakening voice drops in and out of "voiced" for a few hops. Switching the shifter on and off that fast is
// heard as a buzz, so a note survives short gaps.
constexpr int kMaxGapHops = 4;
// A note starts only when two hops in a row agree, so a stray estimate never steers the shifter.
constexpr float kOnsetAgreeCents = 100.0f;

float centsBetween(float a, float b) noexcept {
    return std::fabs(1200.0f * std::log2(b / a));
}

// A tracker's usual mistake is a jump to a harmonic or a subharmonic for a few hops: an octave, but also a fifth
// or a twelfth. A sung note change lasts longer.
bool isJump(float from, float to) noexcept {
    return centsBetween(from, to) > kJumpCents;
}

// The voice range says which octave the singer is in, so an estimate an octave outside it is the right note in the
// wrong octave: a harmonic or a subharmonic.
float foldIntoRange(float hz, const PitchTrackerConfig& config) noexcept {
    for (int i = 0; i < 2 && hz > config.maxHz && hz * 0.5f >= config.minHz; ++i)
        hz *= 0.5f;
    for (int i = 0; i < 2 && hz < config.minHz && hz * 2.0f <= config.maxHz; ++i)
        hz *= 2.0f;
    return hz;
}

} // namespace

void PitchTracker::reset() noexcept {
    heldHops_ = 0;
    gapHops_ = 0;
    previous_ = 0.0f;
    pending_ = 0.0f;
}

TrackedPitch PitchTracker::update(float hz, bool detectorVoiced, float hopRmsDb,
                                  const PitchTrackerConfig& config) noexcept {
    if (hopRmsDb < config.silenceDb) {
        reset();
        return {};
    }
    if (detectorVoiced && hz > 0.0f)
        hz = foldIntoRange(hz, config);
    bool bridged = false;
    const bool usable = detectorVoiced && hz >= config.minHz && hz <= config.maxHz;
    if (!usable) {
        pending_ = 0.0f;
        if (previous_ > 0.0f && gapHops_ < kMaxGapHops) {
            ++gapHops_;
            hz = previous_; // bridge the gap with the note we had
        } else {
            reset();
            return {};
        }
        bridged = true;
    } else if (previous_ <= 0.0f) {
        if (pending_ <= 0.0f || centsBetween(pending_, hz) > kOnsetAgreeCents) {
            pending_ = hz;
            return {};
        }
        pending_ = 0.0f;
        gapHops_ = 0;
    } else {
        gapHops_ = 0;
        // No median filter: it would delay every estimate by a hop, and a late correction deepens a wobble.
        if (isJump(previous_, hz) && heldHops_ < kMaxHoldHops) {
            ++heldHops_;
            hz = previous_;
        } else {
            heldHops_ = 0;
        }
    }
    previous_ = hz;

    TrackedPitch out;
    out.voiced = true;
    out.f0Hz = hz;
    out.midi = 69.0f + 12.0f * std::log2(out.f0Hz / config.refA4);
    out.bridged = bridged;
    return out;
}

} // namespace dsptb::autotune
