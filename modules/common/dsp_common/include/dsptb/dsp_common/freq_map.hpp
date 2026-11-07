#pragma once

// Frequency-axis mapping.
//
// Maps FFT bins onto the Spectrogram's display rows on a Linear, Log or
// Mel axis, and provides the Hz/mel and note-name helpers used for axis
// labels and the hover readout. The mel formulas match librosa
// (htk=False is the Slaney scale; HTK is available as an option), because
// The mel filterbank reuses them.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb::dsp {

enum class FreqScale : std::uint8_t { Linear, Log, Mel };

// Slaney mel scale: librosa.hz_to_mel / mel_to_hz with htk=False.
// Linear below 1000 Hz, logarithmic above (see freq_map.cpp).
double hzToMelSlaney(double hz) noexcept;
double melToHzSlaney(double mel) noexcept;

// HTK mel scale: librosa.hz_to_mel / mel_to_hz with htk=True.
double hzToMelHtk(double hz) noexcept;
double melToHzHtk(double mel) noexcept;

// Spectrum bins -> display rows (row 0 = bottom = lowest freq).
//
// Row edges are evenly spaced on the chosen scale between minHz and
// maxHz. A row's value is the max dB of the bins whose centre frequency
// falls within [lo, hi) (the top row also includes maxHz itself, so the
// Nyquist bin is not dropped). If no bin falls in the row (the row is
// narrower than the bin spacing), the dB is linearly interpolated
// between the two bins around the row centre. The index tables are
// built in the constructor, so apply() is allocation-free.
class BinToRowMap {
public:
    // fftSize: FFT size (bins = fftSize/2 + 1); sampleRate in Hz; rows:
    // number of display rows; minHz/maxHz: axis range in Hz.
    // Throws std::invalid_argument for an invalid configuration.
    BinToRowMap(std::size_t fftSize, double sampleRate, std::size_t rows, FreqScale scale, double minHz,
                double maxHz);

    // binDb has fftSize/2 + 1 values; rowDb has rows values.
    // Allocation-free, lock-free and noexcept: safe on a real-time path.
    void apply(const float* binDb, float* rowDb) const noexcept;

    // Centre frequency in Hz of a row (the midpoint on the chosen
    // scale, converted back to Hz). Strictly increasing with row.
    double rowCenterHz(std::size_t row) const noexcept;

    // 0..1 position of hz on the axis (for tick labels): 0 at minHz,
    // 1 at maxHz, clamped outside the range.
    double rowToFraction(double hz) const noexcept;

private:
    struct RowInfo {
        std::size_t firstBin = 0;   // first bin whose centre falls in the row
        std::size_t binCount = 0;   // 0 means: interpolate instead of max
        double centerHz = 0.0; // row centre in Hz (scale midpoint)
        std::size_t interpBin = 0;  // lower bin for interpolation
        double interpFrac = 0.0; // weight of interpBin + 1
    };

    FreqScale scale_;
    std::size_t rows_ = 0;
    std::size_t bins_ = 0;
    double minHz_ = 0.0;
    double maxHz_ = 0.0;
    double scaleMin_ = 0.0; // minHz on the chosen scale
    double scaleSpan_ = 0.0; // maxHz - minHz on the chosen scale
    std::vector<RowInfo> table_;
};

// Writes the nearest note name and cents deviation for hz into out
// (cap bytes, always NUL-terminated when cap > 0, never overrun),
// e.g. "A4 +3c" or "C#5 -12c". A4 = 440 Hz. Writes "--" if hz <= 0.
void noteName(double hz, char* out, std::size_t cap) noexcept;

} // namespace dsptb::dsp
