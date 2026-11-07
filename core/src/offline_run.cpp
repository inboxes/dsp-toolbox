#include "dsptb/core/offline_run.hpp"

#include <chrono>
#include <thread>

namespace dsptb {

std::shared_ptr<OfflineRun> OfflineRun::start(WorkerPool& pool, std::shared_ptr<ModuleInstance> instance,
                                              const AudioBuffer& audio, std::shared_ptr<const void> keepAlive) {
    std::shared_ptr<OfflineRun> run(new OfflineRun);
    run->instance_ = std::move(instance);
    run->audio_ = &audio;
    run->keepAlive_ = std::move(keepAlive);
    // A raw pointer, so the worker never holds the last reference: that would destroy the module off the UI
    // thread. The destructor waits for done_.
    OfflineRun* raw = run.get();
    pool.submit([raw] { raw->run(); });
    return run;
}

OfflineRun::~OfflineRun() {
    cancel();
    while (!done())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void OfflineRun::run() noexcept {
    try {
        ModuleInstance& inst = *instance_;
        std::string error;
        if (inst.prepare(static_cast<double>(audio_->sampleRate), kOfflineMaxBlock, true, error)) {
            const float* channels[2] = {audio_->ch[0].data(), audio_->ch[1].data()};
            int result = 1;
            if (!inst.callRunOffline(channels, 2, audio_->frames(), static_cast<double>(audio_->sampleRate),
                                     &progress_, &cancel_, result)) {
                error_ = "the module crashed";
            } else if (result == 0 && !cancel_.load(std::memory_order_relaxed)) {
                ok_ = true;
            } else if (cancel_.load(std::memory_order_relaxed)) {
                cancelled_ = true;
            } else {
                error_ = "the module reported a failure";
            }
        } else {
            error_ = error;
        }
    } catch (...) {
        error_ = "out of memory";
    }
    // The last thing this thread does here: after this store the object may be destroyed.
    done_.store(true, std::memory_order_release);
}

} // namespace dsptb
