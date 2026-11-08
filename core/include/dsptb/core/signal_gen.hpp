#pragma once

#include "audio_buffer.hpp"

#include <string>
#include <vector>

namespace dsptb {

enum class SignalType { Sine, Noise, Pink, Impulse, Sweep, Clicks, Silence, Vowel, Chords, Speaker };

bool parseSignalType(const std::string& name, SignalType& out);
const char* signalTypeName(SignalType type);

// Test-signal generator behind `dsptb-cli gen`. Every channel carries the same signal.
//   sine      peak = dbfs, starting phase = phaseDeg
//   noise     white (uniform), RMS = dbfs; deterministic from `seed`
//   pink      ~-3 dB/octave, RMS = dbfs (rescaled after generation); deterministic from `seed`
//   impulse   one sample of amplitude dbfs at t = 0.1 s (or 0 if the file is shorter than 0.2 s)
//   sweep     logarithmic sine sweep freq -> freqEnd over the whole length, peak = dbfs
//   clicks    single-sample impulses of amplitude dbfs every 60/bpm seconds, starting at t = 0
//   silence   zeros
//   vowel     a sung "ah": a band-limited pulse train at freq through resonators at 700, 1220 and 2600 Hz
//             (bandwidths 80, 90, 120 Hz), or at `formants` / `bandwidths` when given, peak = dbfs, with optional
//             vibrato and a source tilt of tiltDbOct per octave
//   speaker   a synthetic talker for voice tests: single-tube formants Fi = (2i-1)*35000/(4*vtlCm), i = 1..4,
//             bandwidths 80/90/120/150 Hz; with `vowels` "mixed", five vowel patterns scale them in turn. A new
//             vowel every 400 ms (50 ms crossfades), 100 ms of silence after every 4th. f0 = freq *
//             2^((f0RangeSt/12) * sin(2*pi*0.3*t)). Breath noise at breathDb relative to the voiced source, through
//             the same resonators. Peak = dbfs; deterministic from `seed`
//   chords    sine triads with the root doubled an octave below, each tone at dbfs, for secondsPerChord each.
//             `chords` is a comma list such as "Am,F#m,C". The length comes from the list, not `seconds`.
struct GenParams {
    SignalType type = SignalType::Sine;
    double freq = 1000.0;
    double freqEnd = 20000.0;
    double dbfs = -20.0;
    double seconds = 5.0;
    double phaseDeg = 0.0;
    double bpm = 120.0;
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t seed = 1;
    double vibratoHz = 0.0;
    double vibratoCents = 0.0; // peak deviation
    std::string chords;
    double secondsPerChord = 1.0;
    std::vector<double> formants;   // vowel: Hz; empty = 700, 1220, 2600
    std::vector<double> bandwidths; // vowel: Hz, one per formant; empty = 80, 90, 120, 150 (as many as needed)
    double tiltDbOct = 0.0;         // vowel, speaker: source tilt per octave of harmonic number
    double vtlCm = 17.0;            // speaker
    double f0RangeSt = 4.0;         // speaker: peak pitch swing
    std::string vowels = "schwa";   // speaker: schwa or mixed
    double breathDb = -30.0;        // speaker: breath noise relative to the voiced source
};

// True when `chords` is a non-empty comma list of roots (A to G, optional # or b) with an optional m.
bool validChordList(const std::string& chords);

PcmAudio generateSignal(const GenParams& params);

double dbfsToLinear(double dbfs);

} // namespace dsptb
