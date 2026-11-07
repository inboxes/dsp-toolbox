#pragma once

// KeyDetector: musical key estimation with the Krumhansl–Schmuckler
// method.
//
// References: C. Krumhansl, "Cognitive Foundations of Musical Pitch"
// (Oxford University Press, 1990); S. Pauws, "Musical Key Extraction
// from Audio", ISMIR 2004. The key profiles are Krumhansl & Kessler
// (1982).

#include <dsptb/dsp_common/pitch_spectrum.hpp>
#include <dsptb/dsp_common/stft.hpp>
#include <dsptb/dsp_common/tuning.hpp>
#include <dsptb/dsp_common/tuning_estimator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dsptb::dsp {

enum class KeyMode : std::uint8_t { Major, Minor };

struct KeyConfig {
    // fftSize and hop are values at 48 kHz: the constructor scales both
    // by the sample-rate factor described on KeyDetector, so the
    // frequency resolution (Hz per bin) is the same at every sample
    // rate. Both are validated as configured, before scaling.
    std::size_t fftSize = 8192; // STFT frame size at 48 kHz; power of two in [32, 65536]
    std::size_t hop = 4096;     // samples between frame starts at 48 kHz; must be >= 1
    double minHz = 55.0;        // lowest peak frequency folded into the chroma
    double maxHz = 1760.0;      // highest peak frequency folded into the chroma
    // Empty = estimate the recording's tuning (a recording tuned away from 440 flips every pitch-class
    // decision near the halfway point). Set it to analyse with a fixed tuning.
    std::optional<Tuning> tuning;
};

struct KeyResult {
    bool valid = false; // false when too little signal was analysed to decide (see KeyDetector)
    int root = 0;       // pitch class of the tonic: 0 = C, 1 = C#, ..., 11 = B
    KeyMode mode = KeyMode::Major;
    float correlation = 0.0f; // Pearson r of the winning profile, -1..1
    float margin = 0.0f;      // winner r minus the best other r, ignoring the winner's relative key
    bool confident = false;   // valid && correlation >= 0.6 && margin >= 0.05
    float tuningOffsetCents = 0.0f; // the tuning used, relative to A4 = 440
    bool tuningEstimated = false;   // true when the offset came from the recording, not the config
};

namespace detail {

// One spectral peak of a single STFT frame, refined by parabolic
// interpolation on the log-magnitude.
struct FramePeak {
    double frequencyHz = 0.0; // interpolated peak frequency
    double magnitude = 0.0;   // interpolated peak magnitude, on the S_k = sqrt(P_k) scale
};

// Internal: peak extraction on one frame, kept for its tests. KeyDetector
// now gets its peaks from PitchSpectrum. Not part of the public API; no
// stability guarantee.
//
// Finds the peaks of one frame from its power spectrum `power` (the
// `bins` values of |X_k|^2 delivered by StftAnalyzer::pushPower): a bin
// contributes only when its magnitude S_k = sqrt(P_k) is a strict local
// maximum; the peak's frequency and magnitude are refined by parabolic
// interpolation on the log-magnitude; peaks whose interpolated
// magnitude is below 1% of the frame's largest peak, or whose
// interpolated frequency lies outside [minHz, maxHz], are dropped.
// `magnitudes` is caller-owned scratch of at least `bins` doubles.
// Surviving peaks are written to `peaks` in ascending bin order, at
// most `peakCapacity` of them, and the number written is returned.
// Strict local maxima are never adjacent, so capacity bins/2 + 1 can
// never truncate. Never allocates.
std::size_t extractFramePeaks(const float* power, std::size_t bins, double binHz, double minHz,
                              double maxHz, double* magnitudes, FramePeak* peaks,
                              std::size_t peakCapacity) noexcept;

} // namespace detail

// Streaming key estimator. Audio is fed with `push` in chunks of any
// size; each completed STFT frame goes through PitchSpectrum at 10-cent
// resolution and is folded into a 120-bin pitch-class accumulator. With an
// estimated tuning, `result()` rotates the accumulator by the tuning offset
// in 10-cent steps, so it stays cheap and independent of chunk sizes with
// no second pass over the audio; the leftover error of at most 5 cents is
// absorbed by the cos^2 weighting. It then folds to 12 classes and
// correlates the chroma against the 24 Krumhansl–Kessler key profiles
// (12 roots x major/minor) and reports the best match. `result()` may be
// called at any time (the consumer calls it every 2 s while audio is
// pushed, and once at the end of a file).
//
// Framing reuses StftAnalyzer::pushPower with a Hann window;
//
// The configured fftSize and hop are values at 48 kHz: the constructor multiplies both by
// s = 2^round(log2(sampleRate / 48000)) — x1 at 44.1 and 48 kHz, x2 at
// 96 kHz, x4 at 192 kHz — so the frequency resolution in Hz per bin is
// the same at every sample rate. (At a fixed size in samples the bin
// width doubles from 48 to 96 kHz, which left close low tones such as
// C3 and Eb3 only ~2 bins apart, their Hann main lobes overlapping and
// biasing any 3-point interpolation.) The scaling is an exact integer
// shift; when downscaling (sample rates below ~34 kHz) a configured
// hop that is not divisible by the downscale factor truncates, and a
// scaled hop of 0 is rejected by the StftAnalyzer's own validation
// (std::invalid_argument). The configured values are
// validated as given, before scaling; the scaled size must still land
// in the StftAnalyzer's [32, 65536] range, so an extreme configured
// size combined with a high sample rate can throw std::invalid_argument
// from the scaled size.
//
// valid is false when fewer than 2 frames were analysed or the total
// accumulated chroma is below 1e-6 (e.g. silence); the other fields are
// then zero/default and confident is false.
//
// All memory is allocated by the constructor. `push`, `chroma`,
// `result` and `reset` are noexcept and never allocate, lock or throw,
// so they are safe on a real-time thread.
//
// Thread safety: one instance is a single stateful stream; it must not
// be used concurrently from several threads.
class KeyDetector {
public:
    // Throws std::invalid_argument for an invalid config: non-positive
    // sample rate, minHz <= 0 or minHz >= maxHz, a configured fftSize
    // that is not a power of two in [32, 65536], a configured hop of 0,
    // or a scaled fftSize (see the class comment) outside the
    // StftAnalyzer's [32, 65536] range.
    KeyDetector(double sampleRate, const KeyConfig& config = {});

    // Clears the accumulated chroma and restarts framing (the same
    // state as a fresh instance).
    void reset() noexcept;

    // Feeds n mono samples; any chunk sizes.
    void push(const float* mono, std::size_t n) noexcept;

    // Writes the accumulated chroma, sum-normalised (all zeros when
    // nothing has been accumulated).
    void chroma(float out[12]) const noexcept;

    // The current best key estimate; can be called at any time.
    KeyResult result() const noexcept;

private:
    // Validates the configured values as given (fftSize a power of two
    // in [32, 65536], hop >= 1) and returns the StftConfig with fftSize
    // and hop scaled by the sample-rate factor from the class comment.
    static StftConfig effectiveStftConfig(double sampleRate, const KeyConfig& config);

    double sampleRate_;
    KeyConfig config_;
    StftConfig effective_; // config_ with fftSize/hop scaled for sampleRate_
    StftAnalyzer stft_;
    PitchSpectrum spectrum_;
    TuningEstimator estimator_;
    std::array<float, 120> frameClasses_{};  // one frame, 10 bins per semitone
    std::array<double, 120> accumulated_{};  // all frames, at the analysis tuning
    std::uint64_t frames_ = 0;               // STFT frames analysed since construction/reset

    std::array<double, 12> foldedChroma(const TuningEstimate& estimate) const noexcept;
};

} // namespace dsptb::dsp
