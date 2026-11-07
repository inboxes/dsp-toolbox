#include <dsptb/dsp_common/tuning.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

constexpr std::array<const char*, 12> kNames = {"C",  "C#", "D",  "D#", "E",  "F",
                                                "F#", "G",  "G#", "A",  "A#", "B"};

// Floor division, because C++ '/' truncates toward zero and would put negative MIDI notes in the wrong
// octave.
constexpr long long positiveMod12(long long midi) noexcept {
    return ((midi % 12) + 12) % 12;
}

} // namespace

Tuning::Tuning(double a4) : a4Hz(a4) {
    if (!(a4 >= kMinA4Hz && a4 <= kMaxA4Hz))
        throw std::invalid_argument("Tuning: A4 must be in [415, 466] Hz");
}

Tuning Tuning::fromOffsetCents(double cents) noexcept {
    Tuning t;
    if (!std::isnan(cents))
        t.a4Hz = std::clamp(440.0 * std::pow(2.0, cents / 1200.0), kMinA4Hz, kMaxA4Hz);
    return t;
}

double Tuning::offsetCents() const noexcept {
    return 1200.0 * std::log2(a4Hz / 440.0);
}

double hzToMidi(double hz, const Tuning& t) noexcept {
    if (!(hz > 0.0))
        return -std::numeric_limits<double>::infinity();
    return 69.0 + 12.0 * std::log2(hz / t.a4Hz);
}

double midiToHz(double midi, const Tuning& t) noexcept {
    return t.a4Hz * std::pow(2.0, (midi - 69.0) / 12.0);
}

double centsBetween(double hzA, double hzB) noexcept {
    if (!(hzA > 0.0) || !(hzB > 0.0))
        return 0.0;
    return 1200.0 * std::log2(hzA / hzB);
}

NoteInfo nearestNote(double hz, const Tuning& t) noexcept {
    NoteInfo info;
    if (!(hz > 0.0) || !std::isfinite(hz))
        return info;
    double m = hzToMidi(hz, t);
    // A note exactly halfway can come back a few ulps low from the logarithm. Without this it would round
    // down and report +50, which is outside the documented range.
    const double tie = std::floor(m) + 0.5;
    if (std::abs(m - tie) < 1e-9)
        m = tie;
    const double nearest = std::floor(m + 0.5);
    const auto midi = static_cast<long long>(nearest);
    const long long pc = positiveMod12(midi);
    info.midi = static_cast<int>(midi);
    info.pitchClass = static_cast<int>(pc);
    info.octave = static_cast<int>((midi - pc) / 12 - 1);
    info.cents = static_cast<float>(100.0 * (m - nearest));
    info.valid = true;
    return info;
}

const char* pitchClassName(int pitchClass) noexcept {
    if (pitchClass < 0 || pitchClass > 11)
        return "?";
    return kNames[static_cast<std::size_t>(pitchClass)];
}

void noteName(double hz, const Tuning& t, char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap == 0)
        return;
    if (!(hz > 0.0) || !std::isfinite(hz)) {
        // snprintf truncates safely and always NUL-terminates (cap > 0).
        std::snprintf(out, cap, "%s", "--");
        return;
    }
    const double m = hzToMidi(hz, t);
    // Round half toward +inf (floor(x + 0.5)), not away from zero, so a value exactly between two
    // notes or cents goes to the higher one and a cents tie such as +49.5c displays as +50c.
    const double rounded = std::floor(m + 0.5);
    const auto midi = static_cast<long long>(rounded);
    const auto cents = static_cast<long long>(std::floor(100.0 * (m - rounded) + 0.5));
    const long long pc = positiveMod12(midi);
    const long long octave = (midi - pc) / 12 - 1;
    // The sign is part of the number: "%+lld" prints +0 / -12 / +50.
    std::snprintf(out, cap, "%s%lld %+lldc", kNames[static_cast<std::size_t>(pc)], octave, cents);
}

} // namespace dsptb::dsp
