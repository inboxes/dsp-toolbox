#pragma once

#include "audio_buffer.hpp"

#include <string>

namespace dsptb {

enum class SignalType { Sine, Noise, Pink, Impulse, Sweep, Clicks, Silence };

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
};

PcmAudio generateSignal(const GenParams& params);

double dbfsToLinear(double dbfs);

} // namespace dsptb
