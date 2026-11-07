#pragma once

// Re-blocks a stream of arbitrary block sizes into fixed-size internal blocks, for
// processing that wants a constant frame size, such as Steam Audio's 256-frame effects.
//
// It adds exactly `frameSize` frames of latency: the output of a call is the processed input of the previous
// internal blocks, so a module that uses it reports `latencyFrames()` from `latency_frames`. Until the first
// internal block has been processed the output is silence.
//
// Everything is allocated in the constructor; process() and reset() are RT-safe. Planar stereo only (the
// host's bus format).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb::dsp {

class FifoAdapter {
public:
    explicit FifoAdapter(uint32_t frameSize)
        : frameSize_(frameSize ? frameSize : 1), in_(2 * static_cast<size_t>(frameSize_)),
          outReady_(2 * static_cast<size_t>(frameSize_)), outNext_(2 * static_cast<size_t>(frameSize_)) {}

    uint32_t frameSize() const noexcept { return frameSize_; }
    uint32_t latencyFrames() const noexcept { return frameSize_; }

    // Silences the output and forgets buffered input.
    void reset() noexcept {
        std::fill(in_.begin(), in_.end(), 0.0f);
        std::fill(outReady_.begin(), outReady_.end(), 0.0f);
        std::fill(outNext_.begin(), outNext_.end(), 0.0f);
        fill_ = 0;
    }

    // Processes `n` frames in place (io[0] = left, io[1] = right). Whenever frameSize input frames have
    // accumulated, `block(in, out, firstInputOffset)` is called: `in` and `out` are two channel pointers to
    // frameSize frames each, and `firstInputOffset` is the position of the block's first input frame relative
    // to the first frame of THIS call (negative when the block started in an earlier call). The block must
    // write all of `out`.
    template <class F> void process(float* const* io, uint32_t n, F&& block) noexcept {
        for (uint32_t i = 0; i < n; ++i) {
            const float l = io[0][i], r = io[1][i];
            io[0][i] = outReady_[fill_];
            io[1][i] = outReady_[frameSize_ + fill_];
            in_[fill_] = l;
            in_[frameSize_ + fill_] = r;
            if (++fill_ == frameSize_) {
                const float* inCh[2] = {in_.data(), in_.data() + frameSize_};
                float* outCh[2] = {outNext_.data(), outNext_.data() + frameSize_};
                block(inCh, outCh, static_cast<int64_t>(i) + 1 - static_cast<int64_t>(frameSize_));
                outReady_.swap(outNext_);
                fill_ = 0;
            }
        }
    }

private:
    uint32_t frameSize_;
    std::vector<float> in_;       // collecting the next internal block (L then R)
    std::vector<float> outReady_; // the previous block's output, being played out
    std::vector<float> outNext_;  // written by the block callback
    uint32_t fill_ = 0;
};

} // namespace dsptb::dsp
