#pragma once

// SpectralFeatureExtractor: per-frame spectral and temporal features
//.
//
// The definitions match librosa (feature.spectral_centroid,
// spectral_bandwidth, spectral_rolloff, spectral_flatness and
// zero_crossing_rate) so the extractor can be golden-tested against it.

#include <dsptb/dsp_common/fft.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsptb::dsp {

// One frame of features. Frequencies are in Hz; flatness, flux, zcr and
// rms are dimensionless (rms is the linear RMS amplitude of the frame).
struct SpectralFeatures {
    float centroidHz; // spectral centroid: magnitude-weighted mean frequency
    float spreadHz;   // spectral spread (bandwidth, p = 2) around the centroid
    float rolloffHz;  // frequency below which rolloffPct of the magnitude lies
    float flatness;   // spectral flatness in [0, 1]: 1 for white noise / silence
    float flux;       // spectral flux vs the previous frame's magnitude; 0 if none
    float zcr;        // zero-crossing rate: sign changes per sample, in [0, 1)
    float rms;        // RMS of the unwindowed frame
};

// Streaming spectral-feature extractor over a mono signal. Definitions use
// the magnitude S_k and power P_k = S_k^2 of the Hann-windowed frame, with
// bin frequency f_k = k*sampleRate/fftSize (spec §4.9):
//   centroid = sum f_k S_k / sum S_k
//   spread   = sqrt(sum (f_k - centroid)^2 S_k / sum S_k)
//   rolloff  = smallest f_k with sum_{j<=k} S_j >= rolloffPct * sum S_j
//   flatness = exp(mean ln max(amin, P_k)) / mean max(amin, P_k), amin = 1e-10
//   flux     = sqrt(sum (S_k(t) - S_k(t-1))^2) / K, K = number of bins,
//              and 0 when there is no previous frame
//   zcr      = sign changes between consecutive samples / frameSize,
//              where sign(0) counts as positive
//   rms      = sqrt(mean x^2) of the unwindowed frame
// On silence (all S_k = 0): centroid, spread and rolloff are 0, flatness
// is 1 and flux is 0.
//
// rolloffPct is stored as a float and widened to double for the rolloff
// threshold, so the default 0.85f becomes 0.85000002 in double. It matches
// the golden exactly, but librosa's 0.85 could in principle differ on a
// frame whose cumulative sum lands almost exactly on the threshold.
//
// Samples are fed with `push` in chunks of any size; the callback fires
// once per completed hop with the features of one frame. Frame f covers
// the pushed samples [f*hop, f*hop + fftSize) — no centring or padding,
// exactly as StftAnalyzer — so the first frame fires only after fftSize
// samples have been pushed. Framing depends only on the sample stream,
// never on the chunk sizes it arrived in. `analyzeFrame` runs the same
// computation on one caller-supplied frame, with an optional
// caller-supplied previous magnitude for the flux; it does not touch the
// stream state (previous magnitude, frame index), so the two may be
// interleaved freely.
//
// All memory is allocated by the constructor. `push`, `analyzeFrame` and
// `reset` are noexcept and never allocate, lock or throw, so they are
// safe on a real-time thread (the callback runs on the caller's thread
// and must uphold the same rules).
//
// Thread safety: one instance is a single stateful stream; it must not be
// used concurrently from several threads.
class SpectralFeatureExtractor {
public:
    // Throws std::invalid_argument for a non-positive sample rate, an
    // fftSize RealFft rejects (not a power of two in [32, 65536]),
    // hop == 0, or a rolloffPct outside (0, 1].
    SpectralFeatureExtractor(double sampleRate, std::size_t fftSize = 2048, std::size_t hop = 512,
                             float rolloffPct = 0.85f);

    // Feeds n mono samples. onFrame(const SpectralFeatures&, uint64_t
    // frame) is called once per completed frame, in frame order.
    template <class F> void push(const float* mono, std::size_t n, F&& onFrame) noexcept {
        std::size_t i = 0;
        while (i < n) {
            if (skip_ > 0) {
                // hop > fftSize: samples between two frames belong to no frame.
                const std::size_t take = skip_ < n - i ? skip_ : n - i;
                skip_ -= take;
                i += take;
                continue;
            }
            const std::size_t space = fftSize_ - fill_;
            const std::size_t take = space < n - i ? space : n - i;
            std::memcpy(frame_.data() + fill_, mono + i, take * sizeof(float));
            fill_ += take;
            i += take;
            if (fill_ == fftSize_) {
                const SpectralFeatures features =
                    computeFeatures(frame_.data(), hasPrev_ ? prevMag_.data() : nullptr);
                // The magnitude computeFeatures just produced becomes the
                // previous magnitude for the next frame (element copy into
                // the preallocated buffer — no allocation).
                std::memcpy(prevMag_.data(), mag_.data(), bins() * sizeof(float));
                hasPrev_ = true;
                onFrame(features, frameIndex_);
                ++frameIndex_;
                advanceFrame();
            }
        }
    }

    // Computes the features of one frame of fftSize raw (unwindowed)
    // samples. prevMag, when non-null, is the previous frame's magnitude
    // spectrum of bins() values (as produced by RealFft on the
    // Hann-windowed frame); the flux is 0 when it is null.
    SpectralFeatures analyzeFrame(const float* frame, const float* prevMag) noexcept {
        return computeFeatures(frame, prevMag);
    }

    // Drops all buffered samples and stream state and restarts framing at
    // frame index 0; the next frame's flux is 0 again.
    void reset() noexcept;

    // Number of spectrum bins per frame: fftSize/2 + 1.
    std::size_t bins() const noexcept { return fft_.bins(); }

private:
    // Windows `frame`, computes its magnitude into mag_ and reduces it to
    // features; prevMag is the previous magnitude for the flux, or null.
    SpectralFeatures computeFeatures(const float* frame, const float* prevMag) noexcept;
    // Slides frame_ to the next frame start after a frame was emitted.
    void advanceFrame() noexcept;

    double sampleRate_;
    std::size_t fftSize_;
    std::size_t hop_;
    float rolloffPct_;
    RealFft fft_;
    std::vector<float> window_;
    std::vector<float> frame_;
    std::vector<float> windowed_;
    std::vector<float> mag_;
    std::vector<float> prevMag_;
    bool hasPrev_ = false;
    std::size_t fill_ = 0; // valid samples currently held in frame_
    std::size_t skip_ = 0; // samples still to drop between frames (hop > fftSize only)
    std::uint64_t frameIndex_ = 0;
};

} // namespace dsptb::dsp
