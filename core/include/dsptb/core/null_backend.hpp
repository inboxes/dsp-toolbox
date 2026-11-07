#pragma once

#include "backend.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace dsptb {

// A backend with no device. Used by CI, tests, --smoke-test and `bench`.
//   * realtime (default): a timer thread calls the engine every bufferFrames/sampleRate seconds.
//   * manual: no thread; the caller drives time with pump(), as fast as it likes. Deterministic.
// It can capture everything the engine outputs and feed a synthetic microphone signal.
class NullBackend final : public IAudioBackend {
public:
    ~NullBackend() override { close(); }

    // Configure before start().
    void setRealtime(bool realtime) { realtime_ = realtime; }
    void setCaptureOutput(bool enable, size_t reserveFrames = 0);
    // Provides microphone input (interleaved stereo) when the config has capture = true. Loops if shorter
    // than the run. Silence if never set.
    void setInput(std::vector<float> interleavedStereo) {
        input_ = std::move(interleavedStereo);
        inputPos_ = 0;
    }

    bool open(const AudioConfig& config, IAudioCallback* callback, std::string& error) override;
    bool start(std::string& error) override;
    void stop() override;
    void close() override;

    uint32_t sampleRate() const override { return config_.sampleRate; }
    uint32_t bufferFrames() const override { return config_.bufferFrames; }
    uint32_t latencyFrames() const override { return config_.bufferFrames; }
    bool deviceLost() const override { return lost_.load(std::memory_order_relaxed); }

    // Manual mode: runs the engine callback synchronously for `frames` frames, in blocks of `block`
    // (0 = bufferFrames). Must be started.
    void pump(size_t frames, uint32_t block = 0);
    // Test hook: pretend the device was unplugged.
    void simulateDeviceLoss() { lost_ = true; }

    // Interleaved stereo. In realtime mode read it only after stop().
    const std::vector<float>& captured() const { return captured_; }
    void clearCaptured() { captured_.clear(); }

private:
    void runBlock(uint32_t frames);
    void timerLoop();

    AudioConfig config_;
    IAudioCallback* callback_ = nullptr;
    bool opened_ = false;
    bool running_ = false;
    bool realtime_ = true;
    bool capture_ = false;
    std::atomic<bool> lost_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;

    std::vector<float> out_;
    std::vector<float> in_;
    std::vector<float> input_;
    size_t inputPos_ = 0;
    std::vector<float> captured_;
};

} // namespace dsptb
