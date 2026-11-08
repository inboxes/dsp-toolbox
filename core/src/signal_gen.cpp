#include "dsptb/core/signal_gen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
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
                  {"silence", SignalType::Silence},  {"vowel", SignalType::Vowel}, {"speaker", SignalType::Speaker},
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

constexpr double kDefaultBandwidths[] = {80.0, 90.0, 120.0, 150.0};

double bandwidthFor(const std::vector<double>& given, size_t i) {
    if (i < given.size())
        return given[i];
    return kDefaultBandwidths[std::min<size_t>(i, std::size(kDefaultBandwidths) - 1)];
}

// A two-pole resonator with unit gain at DC, run in place.
struct Resonator {
    double a1 = 0.0, a2 = 0.0, g = 1.0, y1 = 0.0, y2 = 0.0;
    Resonator(double hz, double bw, double sr) {
        const double r = std::exp(-kPi * bw / sr);
        a1 = 2.0 * r * std::cos(2.0 * kPi * hz / sr);
        a2 = -r * r;
        g = 1.0 - a1 - a2;
    }
    double step(double x) {
        const double y = g * x + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// Band-limited pulse train following f0(t), each harmonic k weighted by k^(tilt/6.02) (tilt dB per octave).
// Harmonics are counted per sample, so a pitch change can't push one past the limit and alias.
template <class F0>
void pulseTrain(F0 f0At, double tiltDbOct, double sr, std::vector<double>& mono) {
    const double nyquistLimit = 0.45 * sr;
    const double expo = tiltDbOct / (20.0 * std::log10(2.0));
    std::vector<double> weight(1, 0.0);
    double phase = 0.0;
    for (size_t n = 0; n < mono.size(); ++n) {
        const double f = f0At(static_cast<double>(n) / sr);
        const int harmonics = std::max(1, static_cast<int>(nyquistLimit / f));
        while (static_cast<int>(weight.size()) <= harmonics)
            weight.push_back(std::pow(static_cast<double>(weight.size()), expo));
        double x = 0.0, norm = 0.0;
        for (int k = 1; k <= harmonics; ++k) {
            x += weight[static_cast<size_t>(k)] * std::cos(phase * k);
            norm += weight[static_cast<size_t>(k)];
        }
        mono[n] = x / norm;
        phase += 2.0 * kPi * f / sr;
        if (phase > 2.0 * kPi)
            phase -= 2.0 * kPi;
    }
}

void normalisePeak(double amp, std::vector<double>& mono) {
    double peak = 0.0;
    for (const double s : mono)
        peak = std::max(peak, std::fabs(s));
    if (peak > 0.0)
        for (double& s : mono)
            s *= amp / peak;
}

void generateVowel(const GenParams& p, double amp, double sr, std::vector<double>& mono) {
    pulseTrain(
        [&](double t) {
            const double cents = p.vibratoCents * std::sin(2.0 * kPi * p.vibratoHz * t);
            return p.freq * std::pow(2.0, cents / 1200.0);
        },
        p.tiltDbOct, sr, mono);
    const std::vector<double> formants =
        p.formants.empty() ? std::vector<double>{700.0, 1220.0, 2600.0} : p.formants;
    for (size_t i = 0; i < formants.size(); ++i) {
        Resonator res(formants[i], bandwidthFor(p.bandwidths, i), sr);
        for (double& s : mono)
            s = res.step(s);
    }
    normalisePeak(amp, mono);
}

// gen speaker: see signal_gen.hpp.
void generateSpeaker(const GenParams& p, double amp, double sr, std::vector<double>& mono) {
    const size_t frames = mono.size();
    std::vector<double> source(frames);
    pulseTrain(
        [&](double t) { return p.freq * std::pow(2.0, p.f0RangeSt / 12.0 * std::sin(2.0 * kPi * 0.3 * t)); },
        p.tiltDbOct, sr, source);
    double rms = 0.0;
    for (const double s : source)
        rms += s * s;
    rms = std::sqrt(rms / std::max<size_t>(1, frames));
    // Breath: uniform noise scaled to breathDb below the voiced source's RMS (uniform RMS = 1/sqrt(3)).
    uint32_t state = p.seed * 747796405u + 2891336453u;
    const double breath = rms * std::pow(10.0, p.breathDb / 20.0) * std::sqrt(3.0);
    for (double& s : source) {
        state = state * 1664525u + 1013904223u;
        s += breath * (static_cast<double>(state) / 2147483648.0 - 1.0);
    }

    // "mixed" cycles /a/ /i/ /u/ /e/ /o/, each a factor on the schwa formants.
    static constexpr double kMixed[5][4] = {
        {1.4, 0.8, 1.0, 1.0}, {0.55, 1.45, 1.2, 1.0}, {0.6, 0.6, 1.0, 1.0}, {0.9, 1.25, 1.05, 1.0}, {1.0, 0.7, 1.0, 1.0},
    };
    const bool mixed = p.vowels == "mixed";
    const size_t patterns = mixed ? 5 : 1;
    // Every pattern filters the whole source with its own resonators, so crossfading them is click-free.
    std::vector<std::vector<double>> filtered(patterns, source);
    for (size_t v = 0; v < patterns; ++v)
        for (int i = 0; i < 4; ++i) {
            const double schwa = (2.0 * (i + 1) - 1.0) * 35000.0 / (4.0 * p.vtlCm);
            const double hz = std::min(schwa * (mixed ? kMixed[v][i] : 1.0), 0.45 * sr);
            Resonator res(hz, kDefaultBandwidths[i], sr);
            for (double& s : filtered[v])
                s = res.step(s);
        }

    constexpr double kVowel = 0.4, kFade = 0.05, kGap = 0.1, kEdge = 0.01;
    constexpr double kCycle = 4.0 * kVowel + kGap;
    for (size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / sr;
        const double cycle = std::floor(t / kCycle);
        const double pos = t - cycle * kCycle;
        if (pos >= 4.0 * kVowel) {
            mono[n] = 0.0;
            continue;
        }
        const int inCycle = static_cast<int>(pos / kVowel);
        const double local = pos - inCycle * kVowel;
        const size_t vowel = (static_cast<size_t>(cycle) * 4 + static_cast<size_t>(inCycle)) % patterns;
        const size_t previous = (static_cast<size_t>(cycle) * 4 + static_cast<size_t>(inCycle) + patterns - 1) % patterns;
        // Crossfade from the previous vowel at each change inside a cycle; fade in and out around the gaps.
        const double w = (inCycle > 0 && local < kFade) ? local / kFade : 1.0;
        double s = w * filtered[vowel][n] + (1.0 - w) * filtered[previous][n];
        const double edge = std::min(pos, 4.0 * kVowel - pos);
        if (edge < kEdge)
            s *= edge / kEdge;
        mono[n] = s;
    }
    normalisePeak(amp, mono);
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
    case SignalType::Speaker:
        return "speaker";
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
    case SignalType::Speaker:
        generateSpeaker(p, amp, sr, mono);
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
