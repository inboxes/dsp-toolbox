#pragma once

// ASIO backend. Only compiled with DSPTB_ENABLE_ASIO; nothing outside backend.cpp includes it.

#include "backend.hpp"

#include <atomic>
#include <memory>

namespace dsptb {

// Our own thin host for the Steinberg ASIO SDK: driver list from the registry, one driver
// open at a time, the first two output channels (and first two input channels when config.capture is set) run
// as the engine's interleaved stereo float stream. The device id is the driver's CLSID; the same driver
// serves input and output, so config.outputDeviceId selects it and inputDeviceId is ignored.
class AsioBackend final : public IAudioBackend {
public:
    AsioBackend();
    ~AsioBackend() override;

    bool open(const AudioConfig& config, IAudioCallback* callback, std::string& error) override;
    bool start(std::string& error) override;
    void stop() override;
    void close() override;

    uint32_t sampleRate() const override { return sampleRate_; }
    uint32_t bufferFrames() const override { return bufferFrames_; }
    uint32_t latencyFrames() const override { return latencyFrames_; }
    bool deviceLost() const override { return lost_.load(std::memory_order_relaxed); }
    std::string describe() const override;
    bool openControlPanel() override;

    // Installed ASIO drivers (registry). Does not load any driver.
    static bool enumerate(std::vector<DeviceInfo>& out, std::string& error);

private:
    bool openDriver(const AudioConfig& config, IAudioCallback* callback, std::string& error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t sampleRate_ = 0;
    uint32_t bufferFrames_ = 0;
    uint32_t latencyFrames_ = 0;
    std::atomic<bool> lost_{false};
};

} // namespace dsptb
