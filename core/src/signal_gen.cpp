#include "dsptb/core/signal_gen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

namespace dsptb {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct XorShift {
    uint64_t s;
    explicit XorShift(uint32_t seed)
        : s(0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(seed) * 0xBF58476D1CE4E5B9ull)) {
        if (s == 0)
            s = 1;
        for (int i = 0; i < 8; ++i)
            next();
    }
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uniformSigned() { // [-1, 1)
        return static_cast<double>(next() >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }
};

double rmsOf(const std::vector<double>& v) {
    if (v.empty())
        return 0.0;
    double acc = 0.0;
    for (double x : v)
        acc += x * x;
    return std::sqrt(acc / static_cast<double>(v.size()));
}

} // namespace

double dbfsToLinear(double dbfs) {
    return std::pow(10.0, dbfs / 20.0);
}

bool parseSignalType(const std::string& name, SignalType& out) {
    static const struct {
        const char* n;
        SignalType t;
    } kTypes[] = {{"sine", SignalType::Sine},      {"noise", SignalType::Noise},
                  {"pink", SignalType::Pink},      {"impulse", SignalType::Impulse},
                  {"sweep", SignalType::Sweep},    {"clicks", SignalType::Clicks},
                  {"silence", SignalType::Silence},  {"vowel", SignalType::Vowel},
                  {"chords", SignalType::Chords}};
    for (const auto& e : kTypes)
        if (name == e.n) {
            out = e.t;
            return true;
        }
    return false;
}

namespace {

struct Chord {
    int root = 0; // MIDI note in the octave from C3
    bool minor = false;
};

bool parseChord(const std::string& text, Chord& out) {
    static constexpr int kSemitones[] = {9, 11, 0, 2, 4, 5, 7}; // A to G
    if (text.empty() || text[0] < 'A' || text[0] > 'G')
        return false;
    int pc = kSemitones[text[0] - 'A'];
    size_t i = 1;
    if (i < text.size() && (text[i] == '#' || text[i] == 'b'))
        pc += text[i++] == '#' ? 1 : -1;
    out.minor = i < text.size() && text[i] == 'm';
    if (out.minor)
        ++i;
    out.root = 48 + (pc + 12) % 12;
    return i == text.size();
}

bool parseChords(const std::string& list, std::vector<Chord>& out) {
    std::stringstream parts(list);
    std::string item;
    while (std::getline(parts, item, ',')) {
        Chord chord;
        if (!parseChord(item, chord))
            return false;
        out.push_back(chord);
    }
    return !out.empty();
}

void generateVowel(const GenParams& p, double amp, double sr, std::vector<double>& mono) {
    const double nyquistLimit = 0.45 * sr;
    double phase = 0.0;
    for (size_t n = 0; n < mono.size(); ++n) {
        const double t = static_cast<double>(n) / sr;
        const double cents = p.vibratoCents * std::sin(2.0 * kPi * p.vibratoHz * t);
        const double f = p.freq * std::pow(2.0, cents / 1200.0);
        // Harmonics are counted per sample, so vibrato can't push one past the limit and alias.
        const int harmonics = std::max(1, static_cast<int>(nyquistLimit / f));
        double x = 0.0;
        for (int k = 1; k <= harmonics; ++k)
            x += std::cos(phase * k);
        mono[n] = x / harmonics;
        phase += 2.0 * kPi * f / sr;
        if (phase > 2.0 * kPi)
            phase -= 2.0 * kPi;
    }
    static constexpr double kFormants[][2] = {{700.0, 80.0}, {1220.0, 90.0}, {2600.0, 120.0}};
    for (const auto& formant : kFormants) {
        const double r = std::exp(-kPi * formant[1] / sr);
        const double a1 = 2.0 * r * std::cos(2.0 * kPi * formant[0] / sr);
        const double a2 = -r * r;
        const double g = 1.0 - a1 - a2;
        double y1 = 0.0, y2 = 0.0;
        for (double& s : mono) {
            const double y = g * s + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            s = y;
        }
    }
    double peak = 0.0;
    for (const double s : mono)
        peak = std::max(peak, std::fabs(s));
    if (peak > 0.0)
        for (double& s : mono)
            s *= amp / peak;
}

void generateChords(const std::vector<Chord>& chords, double amp, double sr, double secondsPerChord,
                    std::vector<double>& mono) {
    const size_t perChord = static_cast<size_t>(std::llround(secondsPerChord * sr));
    const size_t fade = std::min<size_t>(perChord / 2, static_cast<size_t>(0.005 * sr)); // no clicks between chords
    mono.assign(perChord * chords.size(), 0.0);
    for (size_t c = 0; c < chords.size(); ++c) {
        const int root = chords[c].root;
        const int notes[] = {root - 12, root, root + (chords[c].minor ? 3 : 4), root + 7};
        for (size_t n = 0; n < perChord; ++n) {
            const double t = static_cast<double>(n) / sr;
            const auto edge = static_cast<double>(std::min(n, perChord - 1 - n));
            const double gain = fade > 0 ? std::min(1.0, edge / static_cast<double>(fade)) : 1.0;
            double x = 0.0;
            for (const int note : notes)
                x += std::sin(2.0 * kPi * 440.0 * std::pow(2.0, (note - 69) / 12.0) * t);
            mono[c * perChord + n] = amp * gain * x;
        }
    }
}

} // namespace

bool validChordList(const std::string& chords) {
    std::vector<Chord> parsed;
    return parseChords(chords, parsed);
}

const char* signalTypeName(SignalType type) {
    switch (type) {
    case SignalType::Sine:
        return "sine";
    case SignalType::Noise:
        return "noise";
    case SignalType::Pink:
        return "pink";
    case SignalType::Impulse:
        return "impulse";
    case SignalType::Sweep:
        return "sweep";
    case SignalType::Clicks:
        return "clicks";
    case SignalType::Silence:
        return "silence";
    case SignalType::Vowel:
        return "vowel";
    case SignalType::Chords:
        return "chords";
    }
    return "?";
}

PcmAudio generateSignal(const GenParams& p) {
    PcmAudio out;
    out.channels = std::max<uint32_t>(1, p.channels);
    out.sampleRate = p.sampleRate;
    const size_t frames = static_cast<size_t>(std::llround(std::max(0.0, p.seconds) * p.sampleRate));
    const double amp = dbfsToLinear(p.dbfs);
    const double sr = static_cast<double>(p.sampleRate);

    std::vector<double> mono(frames, 0.0);
    switch (p.type) {
    case SignalType::Sine: {
        const double phase0 = p.phaseDeg * kPi / 180.0;
        for (size_t n = 0; n < frames; ++n)
            mono[n] = amp * std::sin(2.0 * kPi * p.freq * static_cast<double>(n) / sr + phase0);
        break;
    }
    case SignalType::Noise: {
        // Uniform in [-A, A] has RMS A/sqrt(3); pick A so the RMS equals the requested level.
        XorShift rng(p.seed);
        const double a = amp * std::sqrt(3.0);
        for (auto& x : mono)
            x = a * rng.uniformSigned();
        break;
    }
    case SignalType::Pink: {
        // Paul Kellet's economy pink filter on white noise, then rescaled to the exact RMS.
        XorShift rng(p.seed);
        double b0 = 0, b1 = 0, b2 = 0;
        for (auto& x : mono) {
            const double w = rng.uniformSigned();
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            x = b0 + b1 + b2 + w * 0.1848;
        }
        const double r = rmsOf(mono);
        if (r > 0.0)
            for (auto& x : mono)
                x *= amp / r;
        break;
    }
    case SignalType::Impulse: {
        const size_t at = p.seconds >= 0.2 ? static_cast<size_t>(0.1 * sr) : 0;
        if (at < frames)
            mono[at] = amp;
        break;
    }
    case SignalType::Sweep: {
        const double f0 = std::max(1.0, p.freq);
        const double f1 = std::max(1.0, p.freqEnd);
        const double T = std::max(1e-6, p.seconds);
        if (std::fabs(std::log(f1 / f0)) < 1e-9) {
            for (size_t n = 0; n < frames; ++n)
                mono[n] = amp * std::sin(2.0 * kPi * f0 * static_cast<double>(n) / sr);
        } else {
            const double k = std::log(f1 / f0);
            for (size_t n = 0; n < frames; ++n) {
                const double t = static_cast<double>(n) / sr;
                mono[n] = amp * std::sin(2.0 * kPi * f0 * T / k * (std::exp(k * t / T) - 1.0));
            }
        }
        break;
    }
    case SignalType::Clicks: {
        const double period = 60.0 / std::max(1.0, p.bpm) * sr;
        for (double pos = 0.0; pos < static_cast<double>(frames); pos += period)
            mono[static_cast<size_t>(pos)] = amp;
        break;
    }
    case SignalType::Silence:
        break;
    case SignalType::Vowel:
        generateVowel(p, amp, sr, mono);
        break;
    case SignalType::Chords: {
        std::vector<Chord> chords;
        if (parseChords(p.chords, chords))
            generateChords(chords, amp, sr, p.secondsPerChord, mono);
        else
            mono.clear();
        break;
    }
    }

    out.samples.resize(mono.size() * out.channels);
    for (size_t n = 0; n < mono.size(); ++n)
        for (uint32_t c = 0; c < out.channels; ++c)
            out.samples[n * out.channels + c] = static_cast<float>(mono[n]);
    return out;
}

} // namespace dsptb
