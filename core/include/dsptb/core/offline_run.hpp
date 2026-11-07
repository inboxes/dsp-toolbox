#pragma once

// Runs one offline module over the working file on a worker thread, with progress and cancel.

#include "audio_buffer.hpp"
#include "module_instance.hpp"
#include "worker_pool.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace dsptb {

class OfflineRun {
public:
    // `audio` must stay valid until the run is destroyed: `keepAlive` holds it. The caller owns the result and
    // drops it on the UI thread, so the module is always destroyed there, as the ABI promises.
    static std::shared_ptr<OfflineRun> start(WorkerPool& pool, std::shared_ptr<ModuleInstance> instance,
                                             const AudioBuffer& audio, std::shared_ptr<const void> keepAlive);
    // Cancels, and waits for the worker to let go of the module.
    ~OfflineRun();
    OfflineRun(const OfflineRun&) = delete;
    OfflineRun& operator=(const OfflineRun&) = delete;

    float progress() const { return progress_.load(std::memory_order_relaxed); }
    bool done() const { return done_.load(std::memory_order_acquire); }
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }
    // Valid once done(): success, cancelled (no error text), or the failure reason.
    bool succeeded() const { return ok_; }
    bool cancelled() const { return cancelled_; }
    const std::string& error() const { return error_; }

private:
    OfflineRun() = default;
    void run() noexcept;

    std::shared_ptr<ModuleInstance> instance_;
    const AudioBuffer* audio_ = nullptr;
    std::shared_ptr<const void> keepAlive_;
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> cancel_{false}, done_{false};
    bool ok_ = false, cancelled_ = false;
    std::string error_;
};

} // namespace dsptb
