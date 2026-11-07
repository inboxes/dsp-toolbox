#pragma once

// LoudnessMeter and LevelMeter.
//
// LoudnessMeter wraps libebur128 1.2.6 (MIT) for EBU R128 / ITU-R BS.1770-4
// loudness: momentary (400 ms), short-term (3 s), integrated (gated),
// loudness range (EBU Tech 3342) and per-channel true / sample peaks.
// LevelMeter is a small per-channel peak + RMS meter with bar-meter
// ballistics (peak hold, then a linear-in-dB fall) and a clip flag.
//
// Thread safety: a meter instance is not thread-safe. Feed it and read it
// from one thread at a time (the Level Meter module feeds from analyze()
// and reads through a triple buffer).

#include <cstddef>
#include <vector>

namespace dsptb::dsp {

// EBU R128 loudness meter backed by libebur128.
//
// The libebur128 state is created with
// EBUR128_MODE_M | S | I | LRA | TRUE_PEAK | SAMPLE_PEAK | HISTOGRAM.
// HISTOGRAM is required: without it libebur128 keeps a growing
// list of 400 ms blocks and allocates inside add_frames, so a live meter
// would grow without bound. Histogram mode uses constant memory.
//
// All getters return LUFS / LU / dBTP / dBFS as documented per function.
// A level with no energy (silence, or nothing processed yet) is reported
// as -infinity, never NaN: libebur128 returns -HUGE_VAL for those and this
// wrapper maps it to -std::numeric_limits<double>::infinity().
class LoudnessMeter {
public:
    // sampleRate in Hz (> 0), channels >= 1, maxBlock >= 1 frames: the
    // largest chunk process() interleaves at a time (bounds the scratch
    // buffer, not the caller's block size). Throws std::invalid_argument
    // for invalid configuration and std::bad_alloc if libebur128 cannot
    // create its state.
    LoudnessMeter(double sampleRate, unsigned channels = 2, std::size_t maxBlock = 4096);
    ~LoudnessMeter();

    LoudnessMeter(const LoudnessMeter&) = delete;
    LoudnessMeter& operator=(const LoudnessMeter&) = delete;
    LoudnessMeter(LoudnessMeter&& other) noexcept;
    LoudnessMeter& operator=(LoudnessMeter&& other) noexcept;

    // Feed planar audio (planar[ch] has `frames` samples). Any frame count
    // is accepted; larger calls are chunked internally into maxBlock-sized
    // pieces and give the same results as one call. Real-time safe: no
    // allocation, locking, throwing or I/O after construction.
    //
    // Precondition: every planar[ch] entry for ch < channels must be
    // non-null. LoudnessMeter feeds a fixed channel set to libebur128, so
    // it cannot skip a channel (unlike LevelMeter, which processes its
    // channels independently and skips null entries).
    void process(const float* const* planar, std::size_t frames) noexcept;

    // Momentary loudness (400 ms window) in LUFS, -inf for silence.
    double momentaryLufs() const noexcept;
    // Short-term loudness (3 s window) in LUFS, -inf for silence.
    double shortTermLufs() const noexcept;
    // Integrated (gated) loudness in LUFS, -inf for silence.
    double integratedLufs() const noexcept;
    // Loudness range (EBU Tech 3342) in LU; 0 when it cannot be computed
    // yet (for example before any short-term window exists).
    double loudnessRangeLu() const noexcept;
    // Maximum true peak (4x oversampled by libebur128) since construction
    // or reset(), in dBTP, for channel `ch`; -inf if the channel is out of
    // range or no non-zero sample has been seen.
    double truePeakDbtp(unsigned ch) const noexcept;
    // Maximum sample peak since construction or reset(), in dBFS, for
    // channel `ch`; -inf under the same conditions as truePeakDbtp().
    double samplePeakDbfs(unsigned ch) const noexcept;

    // Restart the measurement (integrated, LRA and both peak maxima).
    // libebur128 1.2.6 has no in-place reset, so this destroys and
    // re-creates its state: reset() ALLOCATES, may throw std::bad_alloc
    // and is NOT real-time safe. Call
    // it from the UI thread only, never from process() contexts.
    void reset();

private:
    double sampleRate_;
    unsigned channels_;
    std::size_t maxBlock_;
    // ebur128_state*, held as void* so this public header does not include
    // <ebur128.h> (the dependency stays PRIVATE to the library).
    // Mutable because the libebur128 getters take a non-const state.
    mutable void* state_ = nullptr;
    // Preallocated interleaved scratch, maxBlock_ * channels_ floats.
    std::vector<float> scratch_;
};

// Per-channel peak and RMS meter for bar displays.
//
// Peak ballistics: the displayed peak snaps up instantly to any larger
// sample, holds for peakHoldS seconds after the last sample that reached
// it, then falls linearly at fallDbPerS dB per second (in the dB domain)
// until new audio overtakes it. Ballistics are evaluated per process()
// call from the block peak and the block duration. The hold restarts at
// the full peakHoldS for any block containing a new peak, so with large
// blocks the hold can overshoot by up to one block.
//
// RMS: a moving average of x^2 over the last rmsWindowMs, held in a
// per-channel ring buffer with a double running sum. The sum is recomputed
// exactly from the ring every 2^16 samples per channel, which bounds the
// running-sum drift. Before the window has filled, the average
// covers the samples seen so far.
//
// Levels are in dBFS (20*log10 for peak amplitude, 10*log10 for mean
// power). A channel that has seen only silence reports -infinity.
class LevelMeter {
public:
    // sampleRate in Hz (> 0), channels >= 1, rmsWindowMs > 0,
    // peakHoldS >= 0, fallDbPerS >= 0. Throws std::invalid_argument for
    // invalid configuration.
    LevelMeter(double sampleRate, unsigned channels, double rmsWindowMs = 300, double peakHoldS = 2,
               double fallDbPerS = 20);

    // Feed planar audio (planar[ch] has `frames` samples). A null
    // planar[ch] entry is skipped: LevelMeter's channels are independent
    // (unlike LoudnessMeter's fixed libebur128 channel set). Real-time
    // safe: no allocation, locking, throwing or I/O after construction.
    void process(const float* const* planar, std::size_t frames) noexcept;

    // Displayed peak in dBFS, with hold + fall applied; -inf for a channel
    // that has seen only silence (or an out-of-range channel).
    float peakDbfs(unsigned ch) const noexcept;
    // Moving-window RMS in dBFS; -inf for silence / out-of-range channel.
    // Note: after loud audio decays into digital silence, the running sum
    // can retain a tiny positive residual (around -170 dBFS) until the
    // next exact recompute replaces it; harmless for a bar meter.
    float rmsDbfs(unsigned ch) const noexcept;
    // True if any |x| >= 1.0 was seen on channel `ch` since clearClip()
    // (or construction). False for an out-of-range channel.
    bool clipped(unsigned ch) const noexcept;
    // Clear the clip flag on every channel.
    void clearClip() noexcept;

    // Change the RMS window. Reallocates the ring buffers and restarts
    // the RMS history from silence, so this is NOT real-time safe (hence
    // no noexcept); peaks and clip flags are kept. Throws
    // std::invalid_argument for a non-positive window.
    void setRmsWindowMs(double rmsWindowMs);
    // Change the peak hold time in seconds (negative values clamp to 0).
    void setPeakHoldS(double peakHoldS) noexcept;

private:
    // Recompute one channel's sum exactly from the first `valid` entries
    // of its ring (every 2^16 samples) to stop double-accumulator
    // drift. Returns the fresh sum; the caller owns the running-sum state.
    double recomputeSum(unsigned ch, std::size_t valid) const noexcept;
    // Advance one channel's peak hold/fall by one processed block.
    void updatePeak(unsigned ch, double blockPeakLinear, double blockSeconds) noexcept;

    double sampleRate_;
    unsigned channels_;
    double peakHoldS_;
    double fallDbPerS_;
    std::size_t windowSamples_ = 0;
    // Per-channel RMS rings holding the last windowSamples_ samples.
    std::vector<std::vector<float>> rings_;
    std::vector<double> sums_;              // running sum of x^2 per channel
    std::vector<std::size_t> writePos_;     // next ring slot per channel
    std::vector<std::size_t> validCounts_;  // min(samples seen, window) per channel
    std::vector<std::size_t> sinceRecompute_;
    std::vector<double> peakDb_;         // displayed peak in dBFS (-inf = none)
    std::vector<double> holdRemaining_;  // seconds of hold left per channel
    std::vector<char> clipped_;          // char, not vector<bool> bit-proxy
};

} // namespace dsptb::dsp
