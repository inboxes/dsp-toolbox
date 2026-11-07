#include <dsptb/dsp_common/freq_map.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace dsptb::dsp {

namespace {

// --- Mel scales -------------------------------------------------------------
// Slaney scale (librosa hz_to_mel, htk=False): linear below 1000 Hz with
// a spacing of 200/3 Hz per mel, logarithmic above with a step of
// log(6.4)/27 per mel, joined at 1000 Hz = 15 mel.
constexpr double kMelFSp = 200.0 / 3.0;
constexpr double kMelMinLogHz = 1000.0;
constexpr double kMelMinLogMel = 15.0; // 1000 / (200/3)

double melLogStep() noexcept {
    // log(6.4)/27, exactly as librosa forms it.
    return std::log(6.4) / 27.0;
}

// --- Scale helpers ----------------------------------------------------------
// A scale is a monotone map Hz -> axis coordinate and back. Row edges are
// evenly spaced in axis coordinates (spec §4.5).

double hzToScale(FreqScale scale, double hz) noexcept {
    switch (scale) {
    case FreqScale::Linear:
        return hz;
    case FreqScale::Log:
        return std::log(hz);
    case FreqScale::Mel:
        return hzToMelSlaney(hz);
    }
    return hz;
}

double scaleToHz(FreqScale scale, double coord) noexcept {
    switch (scale) {
    case FreqScale::Linear:
        return coord;
    case FreqScale::Log:
        return std::exp(coord);
    case FreqScale::Mel:
        return melToHzSlaney(coord);
    }
    return coord;
}

bool isValidScale(FreqScale scale) noexcept {
    return scale == FreqScale::Linear || scale == FreqScale::Log || scale == FreqScale::Mel;
}

} // namespace

double hzToMelSlaney(double hz) noexcept {
    if (hz < kMelMinLogHz)
        return hz / kMelFSp;
    return kMelMinLogMel + std::log(hz / kMelMinLogHz) / melLogStep();
}

double melToHzSlaney(double mel) noexcept {
    if (mel < kMelMinLogMel)
        return mel * kMelFSp;
    return kMelMinLogHz * std::exp(melLogStep() * (mel - kMelMinLogMel));
}

double hzToMelHtk(double hz) noexcept {
    // HTK scale (librosa hz_to_mel, htk=True): 2595 * log10(1 + hz/700).
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}

double melToHzHtk(double mel) noexcept {
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

BinToRowMap::BinToRowMap(std::size_t fftSize, double sampleRate, std::size_t rows, FreqScale scale,
                         double minHz, double maxHz)
    : scale_(scale), rows_(rows), minHz_(minHz), maxHz_(maxHz) {
    if (fftSize < 2)
        throw std::invalid_argument("BinToRowMap: fftSize must be at least 2");
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        throw std::invalid_argument("BinToRowMap: sampleRate must be positive and finite");
    if (rows == 0)
        throw std::invalid_argument("BinToRowMap: rows must be at least 1");
    if (!isValidScale(scale))
        throw std::invalid_argument("BinToRowMap: unknown FreqScale");
    if (!std::isfinite(minHz) || !std::isfinite(maxHz) || minHz < 0.0 || maxHz <= minHz)
        throw std::invalid_argument("BinToRowMap: require finite 0 <= minHz < maxHz");
    // log(0) has no finite axis coordinate, so a log axis cannot start at 0.
    if (scale == FreqScale::Log && minHz <= 0.0)
        throw std::invalid_argument("BinToRowMap: Log scale requires minHz > 0");

    bins_ = fftSize / 2 + 1;
    const double binSpacing = sampleRate / static_cast<double>(fftSize);
    scaleMin_ = hzToScale(scale, minHz);
    scaleSpan_ = hzToScale(scale, maxHz) - scaleMin_;
    if (!(scaleSpan_ > 0.0) || !std::isfinite(scaleSpan_))
        throw std::invalid_argument("BinToRowMap: axis range collapses on the chosen scale");

    table_.resize(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        const double fracLo = static_cast<double>(row) / static_cast<double>(rows);
        const double fracHi = static_cast<double>(row + 1) / static_cast<double>(rows);
        const double loHz = scaleToHz(scale, scaleMin_ + scaleSpan_ * fracLo);
        const double hiHz = scaleToHz(scale, scaleMin_ + scaleSpan_ * fracHi);
        const double centerHz = scaleToHz(scale, scaleMin_ + scaleSpan_ * (fracLo + fracHi) * 0.5);

        RowInfo info;
        info.centerHz = centerHz;

        // First bin whose centre is at or above loHz. The division gives
        // the index directly; the two short walks repair any rounding at
        // an exact edge so [lo, hi) membership is decided on the Hz
        // values themselves, not on the rounded quotient.
        std::size_t first = 0;
        if (loHz > 0.0) {
            const double approx = std::ceil(loHz / binSpacing);
            first = approx >= static_cast<double>(bins_) ? bins_ : static_cast<std::size_t>(approx);
        }
        while (first > 0 && static_cast<double>(first - 1) * binSpacing >= loHz)
            --first;
        while (first < bins_ && static_cast<double>(first) * binSpacing < loHz)
            ++first;

        // Count the bins in [lo, hi). The top row also takes a bin
        // exactly at hiHz (== maxHz): otherwise the Nyquist bin, which
        // sits exactly on maxHz for a full-range axis, would be dropped,
        // and a rows == bins linear map would not be the identity.
        const bool isTopRow = (row + 1 == rows);
        std::size_t count = 0;
        while (first + count < bins_) {
            const double binHz = static_cast<double>(first + count) * binSpacing;
            if (binHz < hiHz || (isTopRow && binHz <= hiHz))
                ++count;
            else
                break;
        }
        info.firstBin = first;
        info.binCount = count;

        // Interpolation data for rows with no bin: the fractional bin
        // position of the row centre, split into bin + fraction.
        const double pos = std::clamp(centerHz / binSpacing, 0.0, static_cast<double>(bins_ - 1));
        const std::size_t lower = static_cast<std::size_t>(std::floor(pos));
        info.interpBin = lower;
        info.interpFrac = (lower + 1 < bins_) ? (pos - static_cast<double>(lower)) : 0.0;

        table_[row] = info;
    }
}

void BinToRowMap::apply(const float* binDb, float* rowDb) const noexcept {
    if (binDb == nullptr || rowDb == nullptr)
        return;
    for (std::size_t row = 0; row < rows_; ++row) {
        const RowInfo& info = table_[row];
        if (info.binCount > 0) {
            float best = binDb[info.firstBin];
            for (std::size_t i = 1; i < info.binCount; ++i)
                best = std::max(best, binDb[info.firstBin + i]);
            rowDb[row] = best;
        } else if (info.interpBin + 1 < bins_) {
            // Row narrower than a bin: interpolate the dB linearly
            // between the two bins around the row centre (spec §4.5).
            const double frac = info.interpFrac;
            const double lo = static_cast<double>(binDb[info.interpBin]);
            const double hi = static_cast<double>(binDb[info.interpBin + 1]);
            rowDb[row] = static_cast<float>(lo + frac * (hi - lo));
        } else {
            rowDb[row] = binDb[bins_ - 1];
        }
    }
}

double BinToRowMap::rowCenterHz(std::size_t row) const noexcept {
    if (table_.empty())
        return 0.0;
    // Out-of-range rows clamp to the nearest real row rather than
    // reading past the table (this function is noexcept).
    return table_[std::min(row, table_.size() - 1)].centerHz;
}

double BinToRowMap::rowToFraction(double hz) const noexcept {
    if (!(hz > minHz_))
        return 0.0;
    if (hz >= maxHz_)
        return 1.0;
    const double frac = (hzToScale(scale_, hz) - scaleMin_) / scaleSpan_;
    return std::clamp(frac, 0.0, 1.0);
}

void noteName(double hz, char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap == 0)
        return;
    if (!(hz > 0.0) || !std::isfinite(hz)) {
        // snprintf truncates safely and always NUL-terminates (cap > 0).
        std::snprintf(out, cap, "%s", "--");
        return;
    }

    // Semitones from A4 (MIDI 69 at 440 Hz), as a real number.
    const double midiFloat = 69.0 + 12.0 * std::log2(hz / 440.0);
    // Round half toward +inf (floor(x + 0.5)), not away from zero, so a
    // value exactly between two notes/cents goes to the higher one and
    // a cents tie such as +49.5c displays as +50c.
    const double midiRounded = std::floor(midiFloat + 0.5);
    const long long midi = static_cast<long long>(midiRounded);
    const double centsFloat = 100.0 * (midiFloat - midiRounded);
    const long long cents = static_cast<long long>(std::floor(centsFloat + 0.5));

    static constexpr std::array<const char*, 12> kNoteNames = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    // Floor division: C++ '/' truncates toward zero, which would put
    // negative MIDI notes in the wrong octave.
    const long long noteIndex = ((midi % 12) + 12) % 12;
    const long long octave = (midi - noteIndex) / 12 - 1;

    // The sign is part of the number: "%+lld" prints +0 / -12 / +50.
    std::snprintf(out, cap, "%s%lld %+lldc", kNoteNames[static_cast<std::size_t>(noteIndex)], octave,
                  cents);
}

} // namespace dsptb::dsp
