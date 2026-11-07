#pragma once

#include "backend.hpp"

#include <atomic>
#include <memory>

namespace dsptb {

// WASAPI shared or exclusive through miniaudio. float32 stereo, low-latency profile.
// Playback-only unless config.capture is set, in which case the stream is duplex.
class MiniaudioBackend final : public IAudioBackend {
public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    bool open(const AudioConfig& config, IAudioCallback* callback, std::string& error) override;
    bool start(std::string& error) override;
    void stop() override;
    void close() override;

    uint32_t sampleRate() const override { return sampleRate_; }
    uint32_t bufferFrames() const override { return bufferFrames_; }
    uint32_t latencyFrames() const override;
    bool deviceLost() const override { return lost_.load(std::memory_order_relaxed); }

    std::string describe() const override;

    static bool enumerate(std::vector<DeviceInfo>& out, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    IAudioCallback* callback_ = nullptr;
    uint32_t sampleRate_ = 0;
    uint32_t bufferFrames_ = 0;
    uint32_t latencyFrames_ = 0;
    uint32_t baseLatencyFrames_ = 0; // device buffers; the split-duplex bridge cushion is added live
    std::atomic<bool> lost_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace dsptb
