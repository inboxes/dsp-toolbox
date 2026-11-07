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
    float median() const noexcept;

    float history_[3] = {0.0f, 0.0f, 0.0f};
    int count_ = 0;
    int next_ = 0;
    float previous_ = 0.0f; // the last accepted f0; 0 after an unvoiced hop
    int heldHops_ = 0;      // hops an octave jump has been ignored for
};

} // namespace dsptb::autotune
