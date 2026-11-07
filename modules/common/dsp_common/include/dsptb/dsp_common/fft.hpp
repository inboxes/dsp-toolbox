#pragma once

// RealFft: a real-to-complex FFT over PFFFT.

#include <complex>
#include <cstddef>

namespace dsptb::dsp {

// Unscaled DFT of a real signal: X_k = sum_n x_n * exp(-2*pi*i*k*n/N), for
// k in [0, N/2]. `forward` is the raw transform (no window, no scaling);
// `inverse` applies the 1/N scaling so inverse(forward(x)) == x.
//
// All memory (the PFFFT setup and its aligned scratch buffers) is allocated
// by the constructor. The transform calls are noexcept and never allocate,
// lock or throw, so they are safe on a real-time thread. Input and output
// pointers may be unaligned; samples are copied through aligned internal
// buffers either way.
//
// Thread safety: the transform calls share internal scratch buffers, so one
// instance must not be used concurrently from several threads. Separate
// instances are fully independent.
class RealFft {
public:
    // n is the transform size: a power of two in [32, 65536]. Any other size
    // throws std::invalid_argument.
    explicit RealFft(std::size_t n);
    ~RealFft();

    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;
    RealFft(RealFft&& other) noexcept;
    RealFft& operator=(RealFft&& other) noexcept;

    // Transform size N.
    std::size_t size() const noexcept { return n_; }
    // Number of spectrum bins returned by the transforms: N/2 + 1.
    std::size_t bins() const noexcept { return n_ / 2 + 1; }

    // Ordered spectrum: writes bins() complex values (bins 0..N/2). DC
    // (bin 0) and Nyquist (bin N/2) are real; PFFFT packs them into one
    // complex entry and this wrapper unpacks them.
    void forward(const float* in, std::complex<float>* out) noexcept;
    // |X_k|^2 for k in [0, N/2]: raw power, no window and no scaling.
    void powerSpectrum(const float* in, float* power) noexcept;
    // |X_k| for k in [0, N/2]: raw magnitude, no window and no scaling.
    void magnitudeSpectrum(const float* in, float* mag) noexcept;
    // Inverse of `forward`, scaled by 1/N so it round-trips. Only the real
    // parts of the DC and Nyquist entries of `in` are used, matching the
    // spectrum `forward` produces.
    void inverse(const std::complex<float>* in, float* out) noexcept;

private:
    // Runs PFFFT on `in` and leaves the packed ordered spectrum (N floats)
    // in outBuf_ for the caller to unpack or reduce.
    void transformPacked(const float* in) noexcept;

    std::size_t n_ = 0;
    void* setup_ = nullptr; // PFFFT_Setup*, kept opaque so no public header includes PFFFT.
    float* inBuf_ = nullptr;
    float* outBuf_ = nullptr;
    float* workBuf_ = nullptr;
};

} // namespace dsptb::dsp
