#pragma once

// YinPitch: streaming YIN fundamental-frequency (f0) estimator
//.
//
// A. de Cheveigné & H. Kawahara, "YIN, a fundamental frequency estimator
// for speech and music", JASA 111(4), 2002. The difference function is
// computed with an FFT (through RealFft); see pitch.cpp for the identity
// used.

#include <dsptb/dsp_common/fft.hpp>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsptb::dsp {

struct PitchConfig {
    std::size_t frameSize = 2048; // samples per analysis frame
    std::size_t hop = 256;        // samples between frame starts; must be >= 1
    double minHz = 50;            // lowest f0 searched, Hz; needs sampleRate/minHz <= frameSize/2
    double maxHz = 1000;          // highest f0 searched, Hz
    double threshold = 0.15;      // YIN absolute threshold on the CMND; a frame is voiced below it
};

// One f0 estimate. `frameIndex` counts the frames analysed since
// construction or the last `reset` (shared by `push` and `analyzeFrame`).
struct PitchResult {
    float hz;         // estimated f0 in Hz; when unvoiced, the best-guess minimum of the CMND
    float confidence; // 1 - CMND at the chosen lag, clamped to [0, 1]
    bool voiced;      // CMND at the chosen lag < threshold
    std::uint64_t frameIndex;
};

// Streaming YIN over a mono signal. Samples are fed with `push` in chunks
// of any size; the callback fires once per completed hop with the result
// for one frame. Frame f covers the pushed samples [f*hop, f*hop +
// frameSize) — no centring or padding — so the first result fires only
// after frameSize samples have been pushed. Framing depends only on the
// sample stream, never on the chunk sizes it arrived in. `analyzeFrame`
// runs the same estimator on one caller-supplied frame.
//
// All memory is allocated by the constructor. `push`, `analyzeFrame` and
// `reset` are noexcept and never allocate, lock or throw, so they are
// safe on a real-time thread (the callback runs on the caller's thread
// and must uphold the same rules).
//
// Thread safety: one instance is a single stateful stream; it must not
// be used concurrently from several threads.
class YinPitch {
public:
    // Throws std::invalid_argument for an invalid config: non-positive
    // sample rate, frameSize < 2, hop == 0, non-positive minHz/maxHz, or
    // sampleRate/minHz > frameSize/2 (the longest period searched must
    // fit the frame's integration window W = frameSize/2). Note the
    // construction order: the FFT is sized from config.frameSize before
    // the validation checks run, so an out-of-range frameSize (e.g. one
    // needing an FFT above 65536) surfaces as RealFft's range error
    // rather than a YinPitch message — it is still invalid_argument.
    YinPitch(double sampleRate, const PitchConfig& config);
    // Drops all buffered samples and restarts framing at frame index 0.
    void reset() noexcept;

    // Feeds n mono samples. onResult(const PitchResult&) is called once
    // per completed frame (one result per hop).
    template <class F> void push(const float* mono, std::size_t n, F&& onResult) noexcept {
        pushStream(mono, n, static_cast<F&&>(onResult));
    }

    // Analyses one frame of config.frameSize samples and returns its
    // result. Uses the same scratch state as `push` (and advances the
    // same frame index), so the two must not be interleaved on one
    // instance unless that is intended.
    PitchResult analyzeFrame(const float* frame) noexcept;

private:
    template <class F> void pushStream(const float* mono, std::size_t n, F&& onResult) noexcept {
        std::size_t i = 0;
        while (i < n) {
            if (skip_ > 0) {
                // hop > frameSize: samples between two frames belong to no frame.
                const std::size_t take = skip_ < n - i ? skip_ : n - i;
                skip_ -= take;
                i += take;
                continue;
            }
            const std::size_t space = config_.frameSize - fill_;
            const std::size_t take = space < n - i ? space : n - i;
            std::memcpy(frameBuf_.data() + fill_, mono + i, take * sizeof(float));
            fill_ += take;
            i += take;
            if (fill_ == config_.frameSize) {
                onResult(analyzeFrame(frameBuf_.data()));
                advanceFrame();
            }
        }
    }

    // Slides frameBuf_ to the next frame start after a frame was emitted.
    void advanceFrame() noexcept;

    double sampleRate_ = 0.0;
    PitchConfig config_{};
    std::size_t windowW_ = 0; // integration window: frameSize/2
    std::size_t tauMin_ = 1;  // shortest lag searched: floor(sampleRate/maxHz), at least 1
    std::size_t tauMax_ = 0;  // longest lag searched: min(W, ceil(sampleRate/minHz))
    RealFft fft_;             // transform size: smallest power of two >= 2*frameSize
    std::vector<float> frameBuf_;        // streaming frame, frameSize samples
    std::vector<float> paddedFrame_;     // frame zero-padded to the FFT size
    std::vector<float> paddedWindow_;    // first W frame samples, zero-padded to the FFT size
    std::vector<std::complex<float>> spectrumFrame_;  // bins() of paddedFrame_
    std::vector<std::complex<float>> spectrumWindow_; // bins() of paddedWindow_
    std::vector<float> corr_;                         // cross-correlation, FFT size samples
    std::vector<double> cumSq_;                       // cumulative sum of x^2, frameSize + 1 entries
    std::vector<double> cmnd_;                        // cumulative mean normalised difference, tauMax + 1
    std::size_t fill_ = 0; // valid samples currently held in frameBuf_
    std::size_t skip_ = 0; // samples still to drop between frames (hop > frameSize only)
    std::uint64_t frameIndex_ = 0;
};

} // namespace dsptb::dsp
