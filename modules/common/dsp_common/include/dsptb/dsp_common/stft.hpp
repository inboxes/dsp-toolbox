#pragma once

// StftAnalyzer: streaming short-time Fourier transform.

#include <dsptb/dsp_common/fft.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsptb::dsp {

// Analysis window applied to each STFT frame. The coefficients are the
// periodic windows from sdk/include/dsptb/dsp/window.hpp.
enum class Window : std::uint8_t { Hann, BlackmanHarris };

// How mixToMono combines a stereo pair. Mid = (L + R) / 2, Side = (L - R) / 2.
enum class ChannelMix : std::uint8_t { Mid, Left, Right, Side };

struct StftConfig {
    std::size_t fftSize = 4096; // power of two in [32, 65536], as for RealFft
    std::size_t hop = 1024;     // samples between frame starts; must be >= 1
    Window window = Window::Hann;
};

// Streaming STFT over a mono signal. Samples are fed with `push` (or
// `pushPower`) in chunks of any size; the callback fires once per completed
// hop with the spectrum of one frame. Frame f covers the pushed samples
// [f*hop, f*hop + fftSize) — no centring or padding (this matches librosa
// with center=False), so the first frame fires only after fftSize samples
// have been pushed. Framing depends only on the sample stream, never on the
// chunk sizes it arrived in.
//
// All memory is allocated by the constructor / `configure`. `push`,
// `pushPower` and `reset` are noexcept and never allocate, lock or throw, so
// they are safe on a real-time thread (the callback runs on the caller's
// thread and must uphold the same rules).
//
// Thread safety: one instance is a single stateful stream; it must not be
// used concurrently from several threads.
class StftAnalyzer {
public:
    // Throws std::invalid_argument for an invalid config (see StftConfig).
    explicit StftAnalyzer(const StftConfig& config);
    // Reconfigures the analyzer and resets the stream. May allocate; not on
    // the per-block path. Strong guarantee: if it throws (invalid config or
    // allocation failure), the previous configuration is left fully in
    // place — the new FFT and buffers are built off to the side and
    // committed with noexcept moves only.
    void configure(const StftConfig& config);
    // Drops all buffered samples and restarts framing at frame index 0.
    void reset() noexcept;

    // Feeds n mono samples. onFrame(const float* magDb, size_t bins,
    // uint64_t frameIndex) is called once per completed frame with the
    // amplitude-corrected magnitude in dB:
    //   magDb[k] = 20*log10(|X_k| * 2 / sum(w) + 1e-12)
    // where X is the FFT of the windowed frame and sum(w) the sum of the
    // window coefficients. With this correction a full-scale sine sitting on
    // a bin reads 0 dB. The pointer is valid only during the callback.
    template <class F> void push(const float* mono, std::size_t n, F&& onFrame) noexcept {
        pushStream(false, mono, n, static_cast<F&&>(onFrame));
    }

    // Same framing as `push`, but onFrame(const float* power, size_t bins,
    // uint64_t frameIndex) receives the raw power spectrum |X_k|^2 of the
    // windowed frame, with no window correction (for MFCC / features).
    template <class F> void pushPower(const float* mono, std::size_t n, F&& onFrame) noexcept {
        pushStream(true, mono, n, static_cast<F&&>(onFrame));
    }

    // Number of spectrum bins per frame: fftSize/2 + 1.
    std::size_t bins() const noexcept { return fft_.bins(); }
    // Centre frequency of bin k in Hz, for a stream at `sampleRate` Hz.
    double binHz(double sampleRate, std::size_t k) const noexcept {
        return static_cast<double>(k) * sampleRate / static_cast<double>(config_.fftSize);
    }

private:
    template <class F> void pushStream(bool power, const float* mono, std::size_t n, F&& onFrame) noexcept {
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
                onFrame(analyzeFrame(power), bins(), frameIndex_);
                ++frameIndex_;
                advanceFrame();
            }
        }
    }

    // Windows the completed frame in frame_ and computes its spectrum into
    // scratch_: dB magnitudes (power == false) or raw power (power == true).
    const float* analyzeFrame(bool power) noexcept;
    // Slides frame_ to the next frame start after a frame was emitted.
    void advanceFrame() noexcept;
    // (Re)builds window_ and the scratch buffers for config_. May allocate.
    void buildBuffers();

    StftConfig config_{};
    RealFft fft_;
    std::vector<float> window_;
    std::vector<float> frame_;
    std::vector<float> windowed_;
    std::vector<float> scratch_;
    double windowSum_ = 0.0;
    std::size_t fill_ = 0; // valid samples currently held in frame_
    std::size_t skip_ = 0; // samples still to drop between frames (hop > fftSize only)
    std::uint64_t frameIndex_ = 0;
};

// Mixes n stereo samples down to mono per `mix` (see ChannelMix). Element i
// of `out` depends only on element i of `l` and `r`, so `out` may alias
// either input. Never allocates.
void mixToMono(const float* l, const float* r, std::size_t n, ChannelMix mix, float* out) noexcept;

} // namespace dsptb::dsp
