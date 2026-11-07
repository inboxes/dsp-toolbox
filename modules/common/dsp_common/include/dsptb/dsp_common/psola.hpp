#pragma once

// PsolaShifter: real-time pitch shifter for a single voice.
//
// Two modes. Live is pitch-synchronous resampling with about 1 ms of fixed
// delay (the singer hears this one; formants move with the pitch, which is
// acceptable over the module's ±300 cents working range). Quality is
// TD-PSOLA and keeps the formants in place, at about one period of minHz
// of delay.
//
// References (papers only; no GPL code was read or copied):
//   Live:    K. Lent, "An efficient method for pitch shifting digitally
//            sampled sounds", Computer Music Journal 13(4), 1989
//            (cycle-based "wavecycle" resampling).
//   Quality: E. Moulines & F. Charpentier, "Pitch-synchronous waveform
//            processing techniques for text-to-speech synthesis using
//            diphones", Speech Communication 9, 1990 (TD-PSOLA);
//            Barbancho et al., PSOLA for singing voice.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb::dsp {

enum class PsolaMode : std::uint8_t { Live, Quality };

struct PsolaConfig {
    double minHz = 80.0;      // lowest f0 the shifter is configured for, Hz
    double maxHz = 1000.0;    // highest f0, Hz; must be > minHz
    std::size_t maxBlock = 4096; // largest block process() is called with
    double crossfadeMs = 5.0; // voiced <-> unvoiced crossfade length, ms
    PsolaMode mode = PsolaMode::Live;
};

// Streaming pitch shifter over a mono signal. process() is called with
// blocks of up to config.maxBlock samples; f0Hz and ratio are taken as
// constant for one call, so callers split blocks where the control
// changes. f0Hz <= 0 means unvoiced. ratio is clamped to [0.5, 2] and a
// voiced f0Hz is clamped to [minHz, maxHz] (the configured range; the
// period used internally is fs / clamped f0). The clamp is not a gate:
// an out-of-range estimate is not treated as unvoiced, it is spliced on
// the clamped period — a 70 Hz voice with minHz = 80 is spliced on an
// 80 Hz period. Callers that know their pitch estimate is outside
// [minHz, maxHz] (or otherwise untrustworthy) should pass f0Hz <= 0
// (unvoiced) for those blocks instead; gating is the caller's job.
//
// Behaviour common to both modes:
// - Unvoiced: the output is the input delayed by exactly latencyFrames().
// - Voiced <-> unvoiced changes crossfade over crossfadeMs between the
//   shifted path and the delayed input, so there are no clicks.
//
// Live mode: a read head moves through the input history at speed `ratio`
// and reads with 4-point Hermite interpolation. Its delay behind the
// newest input is D = L + e with L = latencyFrames() fixed; e wanders in
// [0, T0] (T0 = fs/f0) as whole periods are skipped (shift up) or repeated
// (shift down), each jump crossfaded between the old and the new read
// position over min(T0/2, 2 ms) with a raised cosine. The optional
// +-2-sample best-match refinement is NOT used: jumping by
// exactly one period already lands on the same point of the waveform.
// When the input becomes unvoiced (or ratio returns to 1), e is returned
// to 0 with one crossfaded jump, so the ratio = 1 and unvoiced outputs
// are the input delayed by exactly L. One safety clamp beyond the spec
// text: a Hermite read position is never allowed closer than 2 samples
// to the newest input sample (the old read position of a jump crossfade
// would otherwise overtake the write head). It engages roughly when
// (ratio - 1) * (jump crossfade length) > latencyFrames() — around
// ratio 1.5 and up at low/mid f0 — and never in the Autotune range
// (ratio 0.84–1.19).
//
// Quality mode: TD-PSOLA on the delayed timeline. Analysis marks are one
// period apart, each snapped to the largest positive sample within
// +-0.25*T0 of the expected position; the expected positions form a
// fixed grid one period apart (not re-centred on the snapped marks), so
// every window sees the same point of the waveform cycle and the snap
// offset is consistent from mark to mark. Synthesis marks are spaced
// T0/ratio; each takes
// the nearest analysis mark whose grain has fully arrived (the spec's
// "nearest analysis mark that is already known"), cuts a grain of 2*T0
// samples centred on it, Hann-windows it and overlap-adds it at the
// synthesis mark. The output is normalised by the accumulated window
// sum; where that sum is below 1e-3 the delayed input sample is used.
// Two refinements keep the ratio = 1 output identical to the delayed
// input (the -40 dB identity test): the synthesis positions
// walk the analysis marks themselves (a fractional mark index advances
// by 1/ratio per grain and positions interpolate between adjacent
// marks), so at ratio = 1 every synthesis mark coincides with an
// analysis mark exactly and each grain contributes exactly the delayed
// input; and the walk is anchored at the second finalised mark (the
// first search window is truncated by the voicing start and its mark
// can sit up to half a period off the phase all later marks agree on).
// Grain samples are read with linear interpolation at the real
// synthesis position, so placement is sub-sample accurate.
// latencyFrames() is ceil(fs/minHz) — one period of minHz, the
// spec's aim (the hard bound is ceil(1.25*fs/minHz)).
//
// All memory is allocated by the constructor. process() and reset() are
// noexcept and never allocate, lock or throw, so they are safe on a
// real-time thread. in == out is allowed.
//
// Thread safety: one instance is a single stateful stream; it must not
// be used concurrently from several threads.
class PsolaShifter {
public:
    // Throws std::invalid_argument for a non-positive sample rate,
    // minHz <= 0, maxHz <= minHz, maxBlock == 0 or crossfadeMs < 0.
    PsolaShifter(double sampleRate, const PsolaConfig& config);

    // Constant delay of the processor, in frames. Live: ceil(0.001*fs)+4.
    // Quality: ceil(fs/minHz).
    std::size_t latencyFrames() const noexcept { return latency_; }

    // Shifts n mono samples (n <= config.maxBlock). f0Hz <= 0 (or NaN)
    // means unvoiced; ratio is clamped to [0.5, 2].
    void process(const float* in, float* out, std::size_t n, float f0Hz, float ratio) noexcept;

    // Returns the shifter to its freshly constructed state.
    void reset() noexcept;

private:
    // One input sample in, one output sample out. `voiced`/`t0`/`ratio`
    // are the per-call control values (t0 in samples, unused unvoiced).
    float step(float x, bool voiced, double t0, double ratio) noexcept;
    // History read at an absolute input index; 0 outside the stream.
    float histAt(std::int64_t index) const noexcept;
    // 4-point Hermite (Catmull-Rom) read at a fractional input position.
    float readHermite(double pos) const noexcept;

    // ---- Live ----
    float liveSample() noexcept;
    void liveAdvance(bool voiced, double t0, double ratio) noexcept;
    void startJump(double toPos, double fromSpeed, double t0) noexcept;
    int jumpLength(double t0) const noexcept;

    // ---- Quality ----
    float qualitySample(bool voiced, double t0, double ratio) noexcept;
    void qualityRestartVoiced(double t0) noexcept;
    void qualityFinaliseMarks(double t0) noexcept;
    void qualityProcessDueGrains(double t0, double ratio) noexcept;
    void qualityAddGrain(double synthPos, double t0) noexcept;
    std::int64_t qualityNearestMark(double synthPos, double t0) const noexcept;
    // Analysis mark by absolute index (0-based within the voiced segment).
    bool markAtAbs(std::int64_t absIndex, std::int64_t& out) const noexcept;
    // Synthesis position for fractional mark index mu: interpolates the
    // analysis mark positions (extrapolating with the nominal period
    // past the last finalised mark).
    double qualitySynthPos(double mu, double t0) const noexcept;

    void initState() noexcept;

    double sampleRate_;
    PsolaConfig config_;
    std::size_t latency_ = 0;
    std::size_t crossfadeSamples_ = 0; // crossfadeMs in samples (0 = instant)

    // Input history ring, indexed by absolute sample index & mask.
    std::vector<float> hist_;
    std::size_t histMask_ = 0;
    std::int64_t inCount_ = 0; // samples consumed (= samples produced)

    // Voiced <-> unvoiced crossfade gain: 1 = shifted path, 0 = delayed in.
    double voiceGain_ = 0.0;
    bool voicedTarget_ = false; // target of the most recent process() call

    // ---- Live state ----
    double readPos_ = 0.0;     // read head position (input index, fractional)
    double jumpFromPos_ = 0.0; // old trajectory while a jump crossfade runs
    double jumpFromSpeed_ = 1.0;
    double readSpeed_ = 1.0;   // speed the read head advanced at last sample
    int jumpPos_ = 0;          // samples into the current jump crossfade
    int jumpLen_ = 0;          // 0 = no jump in progress
    double lastT0_ = 0.0;      // most recent voiced period, for return jumps

    // ---- Quality state ----
    std::vector<double> olaAcc_;   // overlap-add accumulator, by output index
    std::vector<double> olaWeight_; // accumulated Hann window sum
    std::size_t olaMask_ = 0;
    std::vector<std::int64_t> marks_; // ring of analysis mark positions
    std::size_t marksHead_ = 0;
    std::size_t marksCount_ = 0;
    double nextMarkExpected_ = 0.0; // next analysis mark, expected position
    double nextSynth_ = 0.0;        // next synthesis mark (content timeline)
    double synthMu_ = 0.0;          // fractional analysis-mark index of nextSynth_
    std::int64_t totalMarks_ = 0;   // marks finalised in this voiced segment
    std::int64_t marksAbsBase_ = 0; // absolute index of marks_[marksHead_]
    bool qualityRunning_ = false;   // marks/synthesis active (voiced)
    bool synthAnchorPending_ = false; // synthesis grid not yet anchored at a mark
    double qualityT0_ = 0.0;        // most recent voiced period (fade-out tail)
};

} // namespace dsptb::dsp
