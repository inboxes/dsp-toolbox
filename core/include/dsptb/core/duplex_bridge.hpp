#pragma once

#include <dsptb/rt/spsc_ring.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace dsptb {

// Joins a capture stream and a playback stream that run on INDEPENDENT hardware clocks (for example a USB
// microphone and a different output device in WASAPI exclusive mode).
//
// Two clocks never agree exactly: even 20 ppm of difference makes a fixed-size buffer between them run dry
// or overflow every few seconds, which is heard as a soft rhythmic click. The bridge therefore keeps the
// buffer fill near a target by resampling the capture stream very slightly (linear interpolation, ratio
// within
// +-0.5 %, normally a few tens of ppm), so frames are never dropped or repeated.
//
// The fill it controls is continuous: ring contents plus the part of the next capture block that the capture
// hardware has already accumulated (interpolated from push timestamps). Sampling only the ring would quantise
// the measurement to whole capture blocks and turn the controller into a slow bang-bang oscillator.
//
//   capture thread:  pushCapture(frames, nowNs)    (wait-free)
//   playback thread: pull(out, frames, nowNs)      (wait-free, always writes exactly `frames` frames)
//
// Both sides are interleaved stereo float. `nowNs` is any monotonic clock in nanoseconds shared by both
// threads (QPC in the backend, simulated time in tests). prepare() allocates; nothing else does.
class DuplexBridge {
public:
    // Sizes the buffer for the two streams. The cushion has two parts:
    //  * a floor of capture block + playback block + margin: the fill is a sawtooth (capture arrives in
    //  blocks,
    //    playback consumes blocks) and jitter can flip which comes first, leaving one capture block less;
    //  * the longest recent gap between two capture callbacks. Some devices (USB microphones especially)
    //  deliver in
    //    bursts with stalls of 5-10 ms; a cushion shorter than the stall runs dry and clicks. The observed
    //    peak decays slowly (about 10 s), so the latency starts generous (`initialGapMs`) and settles to what
    //    the hardware needs.
    // capacityFrames bounds the worst-case backlog.
    void prepare(uint32_t capacityFrames, uint32_t captureBlock, uint32_t playbackBlock, uint32_t sampleRate,
                 double initialGapMs = 8.0, uint32_t marginFrames = 48) {
        ring_ = std::make_unique<rt::SpscRing<float>>(static_cast<size_t>(capacityFrames) * 2);
        capacityFrames_ = static_cast<uint32_t>(ring_->capacity() / 2);
        sampleRate_ = sampleRate;
        playbackBlock_ = playbackBlock;
        marginFrames_ = marginFrames;
        floorFrames_ = captureBlock + playbackBlock + marginFrames;
        resetCounters();
        peakGapFrames_.store(static_cast<float>(initialGapMs * 1e-3 * sampleRate), std::memory_order_relaxed);
        targetFrames_.store(computeTarget(), std::memory_order_relaxed);
    }

    // ---- capture thread ----
    void pushCapture(const float* stereo, uint32_t frames, int64_t nowNs) noexcept {
        if (!ring_)
            return;
        const size_t want = static_cast<size_t>(frames) * 2;
        // Whole frames only: never leave a half frame in the ring.
        const size_t space = ring_->freeSpace() & ~static_cast<size_t>(1);
        const size_t n = std::min(want, space);
        ring_->write(stereo, n);
        if (n < want)
            overflowFrames_.fetch_add((want - n) / 2, std::memory_order_relaxed);

        const int64_t last = lastPushNs_.load(std::memory_order_relaxed);
        if (last != 0 && nowNs > last) {
            const int64_t interval = nowNs - last;
            if (interval > maxPushGapNs_.load(std::memory_order_relaxed))
                maxPushGapNs_.store(interval, std::memory_order_relaxed);
            const float gapFrames = static_cast<float>(static_cast<double>(interval) * 1e-9 * sampleRate_);
            // Peak hold with slow decay (0.9997 per callback is about 10 s at 375 callbacks/s).
            const float held = peakGapFrames_.load(std::memory_order_relaxed) * 0.9997f;
            peakGapFrames_.store(std::max(held, gapFrames), std::memory_order_relaxed);
            const double ms = static_cast<double>(interval) * 1e-6;
            const int bucket = ms < 1.0 ? 0 : ms < 2.0 ? 1 : ms < 3.5 ? 2 : ms < 5.0 ? 3 : ms < 8.0 ? 4 : 5;
            gapBuckets_[bucket].fetch_add(1, std::memory_order_relaxed);
            if (frames < minBlock_.load(std::memory_order_relaxed))
                minBlock_.store(frames, std::memory_order_relaxed);
            if (frames > maxBlock_.load(std::memory_order_relaxed))
                maxBlock_.store(frames, std::memory_order_relaxed);
            const int64_t old = pushIntervalNs_.load(std::memory_order_relaxed);
            pushIntervalNs_.store(old == 0 ? interval : old + (interval - old) / 16,
                                  std::memory_order_relaxed);
        }
        lastBlockFrames_.store(frames, std::memory_order_relaxed);
        lastPushNs_.store(nowNs, std::memory_order_release);
    }

    // ---- playback thread ----
    void pull(float* out, uint32_t frames, int64_t nowNs) noexcept {
        if (!ring_) {
            std::fill_n(out, static_cast<size_t>(frames) * 2, 0.0f);
            return;
        }
        size_t avail = ring_->size() / 2;
        const uint32_t target = computeTarget();
        targetFrames_.store(target, std::memory_order_relaxed);
        if (lastPullNs_ != 0 && nowNs > lastPullNs_ &&
            nowNs - lastPullNs_ > maxPullGapNs_.load(std::memory_order_relaxed))
            maxPullGapNs_.store(nowNs - lastPullNs_, std::memory_order_relaxed);
        lastPullNs_ = nowNs;
        if (primed_ && avail < minAvail_.load(std::memory_order_relaxed))
            minAvail_.store(static_cast<uint32_t>(avail), std::memory_order_relaxed);

        // Far too much backlog (a stalled playback thread, say): jump back to the target rather than add
        // latency.
        if (primed_ && avail > static_cast<size_t>(capacityFrames_) * 3 / 4) {
            discard(avail > target ? avail - target : 0);
            avail = ring_->size() / 2;
            overflowEvents_.fetch_add(1, std::memory_order_relaxed);
        }

        // Priming: wait until the buffer holds the target before the first frame is consumed.
        if (!primed_) {
            if (avail < target || !loadFrame(frameA_) || !loadFrame(frameB_)) {
                std::fill_n(out, static_cast<size_t>(frames) * 2, 0.0f);
                return;
            }
            phase_ = 0.0;
            fillEma_ = static_cast<double>(avail);
            primed_ = true;
        }

        // Continuous fill: ring contents plus the share of the next capture block already accumulated in
        // hardware.
        double level = static_cast<double>(avail);
        const int64_t lastPush = lastPushNs_.load(std::memory_order_acquire);
        const int64_t interval = pushIntervalNs_.load(std::memory_order_relaxed);
        if (lastPush != 0 && interval > 0 && nowNs > lastPush) {
            const double since =
                std::min(1.0, static_cast<double>(nowNs - lastPush) / static_cast<double>(interval));
            level += since * static_cast<double>(lastBlockFrames_.load(std::memory_order_relaxed));
        }
        fillEma_ += 0.02 * (level - fillEma_);
        const double error = fillEma_ - static_cast<double>(target);
        ratio_ = 1.0 + std::clamp(error * kGain, -kMaxRatioOffset, kMaxRatioOffset);
        ratioPpm_.store(static_cast<float>((ratio_ - 1.0) * 1e6), std::memory_order_relaxed);
        ratioAvgPpm_ +=
            0.002 * ((ratio_ - 1.0) * 1e6 - ratioAvgPpm_); // about 1-2 s time constant at typical block sizes
        ratioAvgPpmShared_.store(static_cast<float>(ratioAvgPpm_), std::memory_order_relaxed);

        for (uint32_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(phase_);
            out[2 * i] = frameA_[0] + (frameB_[0] - frameA_[0]) * t;
            out[2 * i + 1] = frameA_[1] + (frameB_[1] - frameA_[1]) * t;
            phase_ += ratio_;
            while (phase_ >= 1.0) {
                phase_ -= 1.0;
                frameA_[0] = frameB_[0];
                frameA_[1] = frameB_[1];
                if (!loadFrame(frameB_)) {
                    // Ran dry despite the controller (a long capture stall): output silence for the rest of
                    // the block and re-prime, instead of repeating the last sample.
                    underflows_.fetch_add(1, std::memory_order_relaxed);
                    primed_ = false;
                    std::fill(out + 2 * (i + 1), out + 2 * static_cast<size_t>(frames), 0.0f);
                    return;
                }
            }
        }
        fill_.store(static_cast<uint32_t>(ring_->size() / 2), std::memory_order_relaxed);
    }

    // ---- statistics (any thread) ----
    uint64_t underflows() const noexcept { return underflows_.load(std::memory_order_relaxed); }
    uint64_t overflowFrames() const noexcept { return overflowFrames_.load(std::memory_order_relaxed); }
    uint64_t overflowEvents() const noexcept { return overflowEvents_.load(std::memory_order_relaxed); }
    uint32_t fillFrames() const noexcept { return fill_.load(std::memory_order_relaxed); }
    uint32_t targetFrames() const noexcept { return targetFrames_.load(std::memory_order_relaxed); }
    float peakGapMs() const noexcept {
        return peakGapFrames_.load(std::memory_order_relaxed) * 1000.0f /
               static_cast<float>(sampleRate_ ? sampleRate_ : 1);
    }
    // How far the controller is currently stretching or squeezing time to follow the capture clock.
    float ratioPpm() const noexcept { return ratioPpm_.load(std::memory_order_relaxed); }
    // Slow average of the above: an estimate of the real clock difference between the two devices.
    // Timing diagnostics: the longest gap between two capture callbacks / two playback callbacks, and the
    // lowest ring fill seen at the start of a pull.
    double maxPushGapMs() const noexcept {
        return static_cast<double>(maxPushGapNs_.load(std::memory_order_relaxed)) * 1e-6;
    }
    double maxPullGapMs() const noexcept {
        return static_cast<double>(maxPullGapNs_.load(std::memory_order_relaxed)) * 1e-6;
    }
    // Capture callback gaps: counts in buckets <1, 1-2, 2-3.5, 3.5-5, 5-8, >8 ms, plus block size range.
    uint32_t gapBucket(int i) const noexcept { return gapBuckets_[i].load(std::memory_order_relaxed); }
    uint32_t minCaptureBlock() const noexcept { return minBlock_.load(std::memory_order_relaxed); }
    uint32_t maxCaptureBlock() const noexcept { return maxBlock_.load(std::memory_order_relaxed); }
    uint32_t minAvailFrames() const noexcept { return minAvail_.load(std::memory_order_relaxed); }
    float driftEstimatePpm() const noexcept { return ratioAvgPpmShared_.load(std::memory_order_relaxed); }

private:
    static constexpr double kGain = 6e-6; // ratio change per frame of fill error (time constant about 3.5 s)
    static constexpr double kMaxRatioOffset = 0.005; // +-0.5 %

    void resetCounters() {
        underflows_ = 0;
        overflowFrames_ = 0;
        overflowEvents_ = 0;
        fill_ = 0;
        ratioPpm_ = 0.0f;
        ratioAvgPpmShared_ = 0.0f;
        ratioAvgPpm_ = 0.0;
        lastPushNs_ = 0;
        pushIntervalNs_ = 0;
        lastBlockFrames_ = 0;
        maxPushGapNs_ = 0;
        maxPullGapNs_ = 0;
        minAvail_ = 0xFFFFFFFFu;
        for (auto& b : gapBuckets_)
            b = 0;
        minBlock_ = 0xFFFFFFFFu;
        maxBlock_ = 0;
        lastPullNs_ = 0;
        primed_ = false;
    }

    uint32_t computeTarget() const noexcept {
        const float gap = peakGapFrames_.load(std::memory_order_relaxed);
        const uint32_t byGap = static_cast<uint32_t>(gap * 1.05f) + playbackBlock_ + marginFrames_;
        return std::min(std::max(floorFrames_, byGap), capacityFrames_ / 2);
    }

    bool loadFrame(float* frame) noexcept {
        float tmp[2];
        if (ring_->read(tmp, 2) != 2)
            return false;
        frame[0] = tmp[0];
        frame[1] = tmp[1];
        return true;
    }

    void discard(size_t frames) noexcept {
        float tmp[64];
        size_t left = frames * 2;
        while (left > 0) {
            const size_t got = ring_->read(tmp, std::min<size_t>(left, 64));
            if (got == 0)
                break;
            left -= got;
        }
    }

    std::unique_ptr<rt::SpscRing<float>> ring_;
    uint32_t capacityFrames_ = 0;
    std::atomic<uint32_t> targetFrames_{0};
    uint32_t floorFrames_ = 0;
    uint32_t playbackBlock_ = 0;
    uint32_t marginFrames_ = 0;
    uint32_t sampleRate_ = 48000;
    std::atomic<float> peakGapFrames_{0.0f};

    // playback-thread state
    bool primed_ = false;
    double phase_ = 0.0;
    double ratio_ = 1.0;
    double fillEma_ = 0.0;
    double ratioAvgPpm_ = 0.0;
    float frameA_[2] = {0, 0};
    float frameB_[2] = {0, 0};

    int64_t lastPullNs_ = 0;
    std::atomic<int64_t> maxPushGapNs_{0};
    std::atomic<int64_t> maxPullGapNs_{0};
    std::atomic<uint32_t> minAvail_{0xFFFFFFFFu};
    std::atomic<uint32_t> gapBuckets_[6] = {};
    std::atomic<uint32_t> minBlock_{0xFFFFFFFFu};
    std::atomic<uint32_t> maxBlock_{0};

    // capture -> playback timing hints
    std::atomic<int64_t> lastPushNs_{0};
    std::atomic<int64_t> pushIntervalNs_{0};
    std::atomic<uint32_t> lastBlockFrames_{0};

    std::atomic<uint64_t> underflows_{0};
    std::atomic<uint64_t> overflowFrames_{0};
    std::atomic<uint64_t> overflowEvents_{0};
    std::atomic<uint32_t> fill_{0};
    std::atomic<float> ratioPpm_{0.0f};
    std::atomic<float> ratioAvgPpmShared_{0.0f};
};

} // namespace dsptb
