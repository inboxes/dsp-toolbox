#pragma once

// SpectralFluxOnset: causal streaming onset detection.
//
// The detector runs a Hann-windowed magnitude STFT, compresses each bin with
// C_k = log(1 + gamma * |X_k|), and takes the novelty of a frame as the mean
// half-wave-rectified increase of C_k over the previous frame (spectral flux
// on log-compressed magnitudes). A frame is an onset when its novelty is a
// strict local peak above an adaptive threshold built from a causal median
// of the recent novelty history. References: Bello et al., "A tutorial on
// onset detection in music signals", IEEE TSAP 2005; Boeck, Krebs & Schedl,
// "Evaluating the online capabilities of onset detection methods", ISMIR
// 2012 (the adaptive-threshold / peak-picking scheme this follows).

#include <dsptb/dsp_common/fft.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsptb::dsp {

struct OnsetConfig {
    std::size_t fftSize = 1024;     // power of two in [32, 65536], as for RealFft
    std::size_t hop = 256;          // samples between frame starts; must be >= 1
    float compression = 100.f;      // gamma in C_k = log(1 + gamma * |X_k|); must be > 0
    std::size_t medianFrames = 11;  // M: novelty history length for the threshold median, in [1, 63]
    float lambda = 1.5f;            // threshold = delta + lambda * median(history)
    float delta = 0.02f;            // absolute floor of the adaptive threshold
    double minIntervalMs = 50.0;    // minimum time between two reported onsets, milliseconds
};

struct OnsetEvent {
    std::uint64_t frameIndex; // frame whose start the onset is reported at
    double timeSeconds;       // (frameIndex * hop + fftSize/2) / sampleRate — the frame centre
    float strength;           // the novelty value of the onset frame
};

// Streaming onset detector over a mono signal. Samples are fed with `push`
// in chunks of any size; framing depends only on the sample stream, never on
// the chunk sizes. Frame f covers the pushed samples [f*hop, f*hop+fftSize)
// — no centring or padding, exactly as StftAnalyzer — so the
// first frame completes only after fftSize samples have been pushed.
//
// Detection is strictly causal except for the one frame of look-ahead the
// spec's peak-picking rule declares: an onset at frame t is confirmed once
// frame t+1 has been analysed, using only frames up to t+1. Together with
// the frame length itself, that is the whole algorithmic delay, and
// `latencyFrames()` reports it exactly.
//
// Stream start: the median history is empty for frame 0, so its threshold
// is δ alone (the median of no values is 0). A loud first frame can
// therefore fire a frame-0 onset at any λ. This is kept deliberately —
// requiring history before any frame could fire would change the §4.7
// formula — and it is why the acceptance tests tolerate an onset in the
// first 100 ms of a stream.
//
// All memory is allocated by the constructor. `push`, `setSensitivity` and
// `reset` are noexcept and never allocate, lock or throw, so they are safe
// on a real-time thread (the callbacks run on the caller's thread and must
// uphold the same rules).
//
// Thread safety: one instance is a single stateful stream; it must not be
// used concurrently from several threads.
class SpectralFluxOnset {
public:
    // Throws std::invalid_argument for an invalid config (see OnsetConfig)
    // or a non-positive sample rate.
    SpectralFluxOnset(double sampleRate, const OnsetConfig& config);

    // Feeds n mono samples. onNovelty(float value, uint64_t frame) is called
    // once per completed frame, in frame order. onOnset(const OnsetEvent&)
    // is called for each detected onset, in frame order; an onset for frame
    // t is reported while frame t+1 is being processed (the declared
    // one-frame look-ahead), so within one push an onset callback always
    // follows the novelty callback of the frame that confirmed it. The
    // event's timeSeconds is the centre time of its frame, per the spec
    // (frame centre, not frame start).
    template <class N, class O>
    void push(const float* mono, std::size_t n, N&& onNovelty, O&& onOnset) noexcept {
        std::size_t i = 0;
        while (i < n) {
            if (skip_ > 0) {
                // hop > fftSize: samples between two frames belong to no frame.
                const std::size_t take = skip_ < n - i ? skip_ : n - i;
                skip_ -= take;
                i += take;
                continue;
            }
            const std::size_t space = config_.fftSize - fill_;
            const std::size_t take = space < n - i ? space : n - i;
            std::memcpy(frame_.data() + fill_, mono + i, take * sizeof(float));
            fill_ += take;
            i += take;
            if (fill_ == config_.fftSize) {
                float novelty = 0.0f;
                OnsetEvent event{};
                const bool fired = processFrame(novelty, event);
                onNovelty(novelty, frameIndex_);
                if (fired)
                    onOnset(event);
                ++frameIndex_;
                advanceFrame();
            }
        }
    }

    // Adjusts the adaptive-threshold parameters for frames analysed from
    // now on (lambda and delta of OnsetConfig). A candidate frame whose
    // threshold was already computed keeps that threshold.
    void setSensitivity(float lambda, float delta) noexcept {
        lambda_ = lambda;
        delta_ = delta;
    }

    // Detection latency in samples: the frame length plus the one hop of
    // look-ahead the peak-picking rule needs. An event reported for frame t
    // is emitted exactly when frame t+1 completes, i.e. this many samples
    // after frame t's start sample entered the stream.
    std::size_t latencyFrames() const noexcept { return config_.fftSize + config_.hop; }

    // Drops all buffered samples and detector state and restarts framing at
    // frame index 0. The sensitivity set with setSensitivity is kept.
    void reset() noexcept;

private:
    // Analyses the completed frame in frame_ (its index is frameIndex_):
    // writes that frame's novelty value, and if the previous frame is
    // confirmed as an onset, fills `event` and returns true.
    bool processFrame(float& noveltyOut, OnsetEvent& eventOut) noexcept;
    // Median of the novelty history ring (0 when the history is empty).
    double historyMedian() noexcept;
    // Slides frame_ to the next frame start after a frame was analysed.
    void advanceFrame() noexcept;

    static constexpr std::size_t kMaxMedianFrames = 63; // M <= 63

    OnsetConfig config_{};
    double sampleRate_ = 0.0;
    double minIntervalFrames_ = 0.0; // minIntervalMs expressed in hops
    float lambda_ = 0.0f;
    float delta_ = 0.0f;

    RealFft fft_;
    std::vector<float> window_;
    std::vector<float> frame_;
    std::vector<float> windowed_;
    std::vector<float> magnitudes_; // |X_k| of the current frame
    std::vector<float> prevCompressed_; // C_k of the previous frame

    std::size_t fill_ = 0; // valid samples currently held in frame_
    std::size_t skip_ = 0; // samples still to drop between frames (hop > fftSize only)
    std::uint64_t frameIndex_ = 0; // frame currently being filled

    // Novelty history for the causal median: a fixed-size ring holding the
    // most recent min(frameIndex_, M) novelty values, oldest at histStart_.
    std::array<double, kMaxMedianFrames> history_{};
    std::size_t histStart_ = 0;
    std::size_t histSize_ = 0;

    // Peak-picking state for the candidate frame (frameIndex_ - 1).
    double prevNovelty_ = 0.0; // novelty of the candidate frame
    double prevPrevNovelty_ = 0.0; // novelty of the frame before the candidate
    double prevThreshold_ = 0.0; // threshold computed for the candidate frame
    std::uint64_t lastOnsetFrame_ = 0;
    bool hasLastOnset_ = false;
};

} // namespace dsptb::dsp
