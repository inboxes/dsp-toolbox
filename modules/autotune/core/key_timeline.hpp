#pragma once

// Turns key estimates for overlapping windows of a song into one key per window, so the correction can follow key
// changes. Pure logic: no SDK.

#include "target_picker.hpp"

#include <cstddef>
#include <vector>

namespace dsptb::autotune {

struct KeyWindow {
    bool valid = false;
    bool confident = false;
    int root = 0;
    Scale scale = Scale::Major; // Major or Minor
};

// Two keys with the same notes (G major and E minor) correct the same way, so they count as one.
int sameNotesId(int root, Scale scale) noexcept;

// One resolved key per window. Only confident windows vote, and a new set of notes takes over only when two windows
// in a row agree on it, so one doubtful window can't flip the key. The change starts at the first of the two.
// Windows before the first confident one take its key. With no confident window at all, every entry is invalid.
std::vector<DetectedKey> resolveKeyTimeline(const std::vector<KeyWindow>& windows);

// The window whose centre is nearest `seconds`, for windows `lengthSeconds` long starting every `hopSeconds`.
size_t keyWindowAt(double seconds, double lengthSeconds, double hopSeconds, size_t count) noexcept;

} // namespace dsptb::autotune
