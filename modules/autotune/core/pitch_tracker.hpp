#pragma once

// Turns raw pitch estimates into a steady "what is being sung". Pure logic: no SDK and no allocation.

#include <cstdint>

namespace dsptb::autotune {

struct PitchTrackerConfig {
    float minHz = 80.0f;
    float maxHz = 1000.0f;
    float silenceDb = -50.0f;
    float refA4 = 440.0f;
};

struct TrackedPitch {
    bool voiced = false;
    float f0Hz = 0.0f;
    float midi = 0.0f; // fractional note number, relative to refA4
};

class PitchTracker {
public:
    TrackedPitch update(float hz, bool detectorVoiced, float hopRmsDb, const PitchTrackerConfig& config) noexcept;
    void reset() noexcept;

private:
    float previous_ = 0.0f; // the last accepted f0; 0 when no note is sounding
    float pending_ = 0.0f;  // the first estimate of a note that hasn't started yet
    int heldHops_ = 0;      // hops a jump has been ignored for
    int gapHops_ = 0;       // unusable hops bridged in the current note
};

} // namespace dsptb::autotune
