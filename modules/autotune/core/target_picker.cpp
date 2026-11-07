#include "target_picker.hpp"

#include <cmath>
#include <limits>

namespace dsptb::autotune {

namespace {

constexpr bool kMajor[12] = {true, false, true, false, true, true, false, true, false, true, false, true};
constexpr bool kMinor[12] = {true, false, true, true, false, true, false, true, true, false, true, false};

// Nearest note in the key. Searching upward with a strict comparison keeps the lower note on a tie.
int nearestInKey(float midi, const Key& key) noexcept {
    const int centre = static_cast<int>(std::lround(midi));
    int best = centre;
    float bestDistance = std::numeric_limits<float>::max();
    for (int t = centre - 3; t <= centre + 3; ++t) {
        const float distance = std::fabs(midi - static_cast<float>(t));
        if (inKey(t, key) && distance < bestDistance) {
            best = t;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace

Key effectiveKey(bool manual, int keyParam, Scale scaleParam, const DetectedKey& detected) noexcept {
    if (manual)
        return {keyParam, scaleParam};
    if (detected.valid && detected.confident)
        return {detected.root, detected.scale};
    return {0, Scale::Chromatic};
}

bool inKey(int midiNote, const Key& key) noexcept {
    if (key.scale == Scale::Chromatic)
        return true;
    const int degree = ((midiNote - key.root) % 12 + 12) % 12;
    return key.scale == Scale::Major ? kMajor[degree] : kMinor[degree];
}

void TargetPicker::reset() noexcept {
    current_ = kNoTarget;
}

int TargetPicker::pick(float midi, bool voiced, const Key& key, float noteHoldCents) noexcept {
    if (!(key == key_)) {
        key_ = key;
        current_ = kNoTarget;
    }
    if (!voiced) {
        current_ = kNoTarget;
        return kNoTarget;
    }
    // Hold: stay on the current note until the singer is clearly past the midpoint, so the target doesn't flicker
    // between two notes.
    if (current_ != kNoTarget &&
        std::fabs(midi - static_cast<float>(current_)) < 0.5f + noteHoldCents / 100.0f)
        return current_;
    current_ = nearestInKey(midi, key);
    return current_;
}

} // namespace dsptb::autotune
