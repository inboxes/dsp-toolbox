#pragma once

// Decides which note the singer should be on. Pure logic: no SDK and no allocation.

namespace dsptb::autotune {

enum class Scale : int { Major = 0, Minor = 1, Chromatic = 2 };

struct Key {
    int root = 0; // pitch class: 0 = C ... 11 = B
    Scale scale = Scale::Chromatic;
    bool operator==(const Key&) const = default;
};

// What key detection last found on the loaded file.
struct DetectedKey {
    bool valid = false;
    bool confident = false;
    int root = 0;
    Scale scale = Scale::Major;
};

// Manual uses the parameters. Otherwise a confident detection decides, and anything less stays Chromatic, because
// correcting to a wrong key sounds far worse than correcting to the nearest note.
Key effectiveKey(bool manual, int keyParam, Scale scaleParam, const DetectedKey& detected) noexcept;

// True when a note, as a MIDI number, belongs to the key.
bool inKey(int midiNote, const Key& key) noexcept;

// How much of the correction to apply, 0 to 1. Near the target note the answer is clear and the weight is 1. Towards
// the halfway point to the next note in the key the singer may mean either, or be bending on purpose, so the weight
// falls to 0 there. Higher Strength keeps full weight further out, and full Strength always corrects fully.
float correctionWeight(float midi, int target, const Key& key, float strengthPercent) noexcept;

class TargetPicker {
public:
    static constexpr int kNoTarget = -1;

    // The MIDI note to correct to, or kNoTarget on an unvoiced hop.
    int pick(float midi, bool voiced, const Key& key, float noteHoldCents) noexcept;
    void reset() noexcept;

private:
    int current_ = kNoTarget;
    Key key_{};
};

} // namespace dsptb::autotune
