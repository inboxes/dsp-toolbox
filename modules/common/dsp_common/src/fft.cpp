#include <dsptb/dsp_common/fft.hpp>

#include <pffft/pffft.h>

#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

namespace dsptb::dsp {
namespace {

bool isPowerOfTwo(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

float* alignedAlloc(std::size_t n) {
    return static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
}

void alignedFree(float* p) noexcept {
    if (p != nullptr)
        pffft_aligned_free(p);
}

} // namespace

RealFft::RealFft(std::size_t n) : n_(n) {
    if (!isPowerOfTwo(n) || n < 32 || n > 65536)
        throw std::invalid_argument("RealFft: size must be a power of two in [32, 65536]");
    setup_ = pffft_new_setup(static_cast<int>(n), PFFFT_REAL);
    inBuf_ = alignedAlloc(n);
    outBuf_ = alignedAlloc(n);
    workBuf_ = alignedAlloc(n);
    if (setup_ == nullptr || inBuf_ == nullptr || outBuf_ == nullptr || workBuf_ == nullptr) {
        // Roll back the partial setup before reporting the failure.
        alignedFree(inBuf_);
        alignedFree(outBuf_);
        alignedFree(workBuf_);
        if (setup_ != nullptr)
            pffft_destroy_setup(static_cast<PFFFT_Setup*>(setup_));
        setup_ = nullptr;
        inBuf_ = outBuf_ = workBuf_ = nullptr;
        throw std::bad_alloc();
    }
}

RealFft::~RealFft() {
    alignedFree(inBuf_);
    alignedFree(outBuf_);
    alignedFree(workBuf_);
    if (setup_ != nullptr)
        pffft_destroy_setup(static_cast<PFFFT_Setup*>(setup_));
}

RealFft::RealFft(RealFft&& other) noexcept
    : n_(std::exchange(other.n_, 0)), setup_(std::exchange(other.setup_, nullptr)),
      inBuf_(std::exchange(other.inBuf_, nullptr)), outBuf_(std::exchange(other.outBuf_, nullptr)),
      workBuf_(std::exchange(other.workBuf_, nullptr)) {}

RealFft& RealFft::operator=(RealFft&& other) noexcept {
    if (this != &other) {
        alignedFree(inBuf_);
        alignedFree(outBuf_);
        alignedFree(workBuf_);
        if (setup_ != nullptr)
            pffft_destroy_setup(static_cast<PFFFT_Setup*>(setup_));
        n_ = std::exchange(other.n_, 0);
        setup_ = std::exchange(other.setup_, nullptr);
        inBuf_ = std::exchange(other.inBuf_, nullptr);
        outBuf_ = std::exchange(other.outBuf_, nullptr);
        workBuf_ = std::exchange(other.workBuf_, nullptr);
    }
    return *this;
}

void RealFft::transformPacked(const float* in) noexcept {
    // Copy in: the caller's pointer may be unaligned, PFFFT's must not be.
    std::memcpy(inBuf_, in, n_ * sizeof(float));
    pffft_transform_ordered(static_cast<PFFFT_Setup*>(setup_), inBuf_, outBuf_, workBuf_,
                            PFFFT_FORWARD);
}

void RealFft::forward(const float* in, std::complex<float>* out) noexcept {
    transformPacked(in);
    // PFFFT's ordered real output packs the two real bins into entry 0 as
    // (DC, Nyquist); entries 1..N/2-1 are the ordinary complex bins.
    out[0] = {outBuf_[0], 0.0f};
    out[n_ / 2] = {outBuf_[1], 0.0f};
    for (std::size_t k = 1; k < n_ / 2; ++k)
        out[k] = {outBuf_[2 * k], outBuf_[2 * k + 1]};
}

void RealFft::powerSpectrum(const float* in, float* power) noexcept {
    transformPacked(in);
    power[0] = outBuf_[0] * outBuf_[0];
    power[n_ / 2] = outBuf_[1] * outBuf_[1];
    for (std::size_t k = 1; k < n_ / 2; ++k) {
        const float re = outBuf_[2 * k];
        const float im = outBuf_[2 * k + 1];
        power[k] = re * re + im * im;
    }
}

void RealFft::magnitudeSpectrum(const float* in, float* mag) noexcept {
    transformPacked(in);
    mag[0] = std::abs(outBuf_[0]);
    mag[n_ / 2] = std::abs(outBuf_[1]);
    for (std::size_t k = 1; k < n_ / 2; ++k) {
        const float re = outBuf_[2 * k];
        const float im = outBuf_[2 * k + 1];
        mag[k] = std::sqrt(re * re + im * im);
    }
}

void RealFft::inverse(const std::complex<float>* in, float* out) noexcept {
    // Repack into PFFFT's ordered layout (entry 0 = (DC, Nyquist)) — the
    // exact inverse of the unpacking in forward().
    outBuf_[0] = in[0].real();
    outBuf_[1] = in[n_ / 2].real();
    for (std::size_t k = 1; k < n_ / 2; ++k) {
        outBuf_[2 * k] = in[k].real();
        outBuf_[2 * k + 1] = in[k].imag();
    }
    pffft_transform_ordered(static_cast<PFFFT_Setup*>(setup_), outBuf_, inBuf_, workBuf_,
                            PFFFT_BACKWARD);
    // PFFFT transforms are unscaled, so the round trip multiplies by N.
    const float scale = 1.0f / static_cast<float>(n_);
    for (std::size_t i = 0; i < n_; ++i)
        out[i] = inBuf_[i] * scale;
}

} // namespace dsptb::dsp
