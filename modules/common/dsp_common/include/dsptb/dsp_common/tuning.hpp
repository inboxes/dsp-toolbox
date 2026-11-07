#pragma once

// Pitch in semitones and cents, relative to a reference A4.
//
// MIDI numbers: 69 = A4, 60 = C4 (middle C). octave = floor(midi / 12) - 1,
// pitchClass = midi mod 12 (0 = C, 9 = A), always 0..11. Cents between two
// frequencies are 1200*log2(a / b). The nearest note rounds ties upward, so
// a note's offset is in [-50, +50) cents.
//
// Everything except the Tuning constructor is noexcept, allocation-free and
// safe on a real-time thread.

#include <cstddef>

namespace dsptb::dsp {

struct Tuning {
    static constexpr double kMinA4Hz = 415.0;
    static constexpr double kMaxA4Hz = 466.0;

    double a4Hz = 440.0; // reference frequency of A4, in [kMinA4Hz, kMaxA4Hz]

    Tuning() = default;
    // Throws std::invalid_argument outside [kMinA4Hz, kMaxA4Hz].
    Tuning(double a4);

    // 440 * 2^(cents/1200), clamped to the valid range. Never throws for a
    // finite or non-finite input: NaN gives the default 440 Hz.
    static Tuning fromOffsetCents(double cents) noexcept;
    // 1200 * log2(a4Hz / 440).
    double offsetCents() const noexcept;
};

// Fractional MIDI note, 69 + 12*log2(hz / a4Hz). hz <= 0 or NaN returns -inf.
double hzToMidi(double hz, const Tuning& t = {}) noexcept;
double midiToHz(double midi, const Tuning& t = {}) noexcept;
// 1200 * log2(hzA / hzB); 0 if either is not positive.
double centsBetween(double hzA, double hzB) noexcept;

struct NoteInfo {
    int midi = 0;       // nearest note
    int pitchClass = 0; // 0 = C ... 11 = B
    int octave = 0;     // C4 = middle C
    float cents = 0.0f; // offset from the nearest note, [-50, +50)
    bool valid = false; // false if hz <= 0 or not finite
};

NoteInfo nearestNote(double hz, const Tuning& t = {}) noexcept;

// "C", "C#", ... "B" (sharps only); "?" outside 0..11.
const char* pitchClassName(int pitchClass) noexcept;

// Writes e.g. "A4 +3c" or "C#5 -12c" using the given tuning; "--" if hz isn't
// valid. Always NUL-terminated when cap > 0, and never overruns.
void noteName(double hz, const Tuning& t, char* out, std::size_t cap) noexcept;

} // namespace dsptb::dsp
