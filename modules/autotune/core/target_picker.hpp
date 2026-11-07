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
