#pragma once

// Musical test signals: chords and key progressions at any tuning, with optional harmonics.

#include "signals.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace dsptb::test::music {

inline double midiHz(int midi, double a4Hz = 440.0) noexcept {
    return a4Hz * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
}

// One chord: root, third (major or minor), fifth, and the root an octave lower, each a tone at amplitude
// 0.1 with `harmonics` partials at amplitude 0.1/h. Lasts `seconds`, appended to `out`.
inline void appendChord(std::vector<float>& out, double fs, int rootMidi, bool majorChord, double seconds,
                        double a4Hz = 440.0, int harmonics = 1) {
    const auto count = static_cast<std::size_t>(std::llround(seconds * fs));
    const double notes[4] = {midiHz(rootMidi, a4Hz), midiHz(rootMidi + (majorChord ? 4 : 3), a4Hz),
                             midiHz(rootMidi + 7, a4Hz), midiHz(rootMidi - 12, a4Hz)};
    for (std::size_t n = 0; n < count; ++n) {
        double value = 0.0;
        for (const double f : notes)
            for (int h = 1; h <= harmonics; ++h)
                value += static_cast<double>(signals::sine(n, fs, f * h, 0.1 / h, 0.0));
        out.push_back(static_cast<float>(value));
    }
}

// The full progression for one key: I-IV-V-I (major) or i-iv-V-i with a major V (minor), each chord 1 s,
// the whole sequence played twice (8 s).
inline std::vector<float> progression(double fs, int rootMidi, bool minorKey, double a4Hz = 440.0,
                                      int harmonics = 1) {
    std::vector<float> out;
    for (int rep = 0; rep < 2; ++rep) {
        appendChord(out, fs, rootMidi, !minorKey, 1.0, a4Hz, harmonics);     // I or i
        appendChord(out, fs, rootMidi + 5, !minorKey, 1.0, a4Hz, harmonics); // IV or iv
        appendChord(out, fs, rootMidi + 7, true, 1.0, a4Hz, harmonics);      // V (always major)
        appendChord(out, fs, rootMidi, !minorKey, 1.0, a4Hz, harmonics);     // I or i
    }
    return out;
}

} // namespace dsptb::test::music
