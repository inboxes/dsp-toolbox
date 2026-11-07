#include "dsptb/core/null_backend.hpp"

#include <algorithm>
#include <cstring>

namespace dsptb {

void NullBackend::setCaptureOutput(bool enable, size_t reserveFrames) {
    capture_ = enable;
    captured_.clear();
    if (enable && reserveFrames)
        captured_.reserve(reserveFrames * 2);
}

bool NullBackend::open(const AudioConfig& config, IAudioCallback* callback, std::string& error) {
    if (opened_)
        close();
    if (config.sampleRate == 0 || config.bufferFrames == 0 || !callback) {
        error = "invalid configuration";
        return false;
    }
    config_ = config;
    callback_ = callback;
    // Allow the engine to be driven with blocks up to 4x the nominal size from pump().
    out_.assign(static_cast<size_t>(config.bufferFrames) * 2 * 8, 0.0f);
    in_.assign(out_.size(), 0.0f);
    lost_ = false;
    opened_ = true;
    return true;
}

bool NullBackend::start(std::string& error) {
    if (!opened_) {
        error = "not open";
        return false;
    }
    if (running_)
        return true;
    running_ = true;
    stopRequested_ = false;
    if (realtime_)
        thread_ = std::thread([this] { timerLoop(); });
    return true;
}

void NullBackend::stop() {
    if (!running_)
        return;
    stopRequested_ = true;
    if (thread_.joinable())
        thread_.join();
    running_ = false;
}

void NullBackend::close() {
    stop();
    opened_ = false;
    callback_ = nullptr;
}

void NullBackend::runBlock(uint32_t frames) {
    const size_t samples = static_cast<size_t>(frames) * 2;
    if (out_.size() < samples) {
        out_.resize(samples);
        in_.resize(samples);
    }
    const float* in = nullptr;
    if (config_.capture) {
        if (input_.empty()) {
            std::fill_n(in_.begin(), samples, 0.0f);
        } else {
            for (size_t i = 0; i < samples; ++i) {
                in_[i] = input_[inputPos_];
                inputPos_ = (inputPos_ + 1) % input_.size();
            }
        }
        in = in_.data();
    }
    callback_->onAudio(in, out_.data(), frames);
    // Captured after the callback returned, i.e. outside the engine's RT scope.
    if (capture_)
        captured_.insert(captured_.end(), out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(samples));
}

void NullBackend::pump(size_t frames, uint32_t block) {
    if (!running_ || !callback_)
        return;
    const uint32_t blk = block ? block : config_.bufferFrames;
    while (frames > 0) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(frames, blk));
        runBlock(n);
        frames -= n;
    }
}

void NullBackend::timerLoop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(config_.bufferFrames) / config_.sampleRate));
    auto next = clock::now() + period;
    while (!stopRequested_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next);
        next += period;
        // If we fell behind (coarse timers), catch up with back-to-back blocks instead of drifting.
        runBlock(config_.bufferFrames);
    }
}

} // namespace dsptb
