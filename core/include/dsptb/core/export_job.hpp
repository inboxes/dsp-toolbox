#pragma once

// Export processed WAV: renders the working file through the enabled processors offline, on the
// worker pool, with progress and cancel. Uses the same `renderOffline` as `dsptb-cli render`, so the two give
// the same samples. The output is written to "<name>.part" and renamed on success, so a cancelled or
// failed export never leaves a partial file at the real name.

#include "audio_io.hpp"
#include "module_registry.hpp"
#include "offline_render.hpp"
#include "worker_pool.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace dsptb {

struct ExportOptions {
    std::filesystem::path out;
    bool loopRegionOnly = false; // otherwise the whole file
    double regionStartSeconds = 0.0;
    double regionEndSeconds = 0.0;
    double tailSeconds = 2.0; // 0 to 10
    WavFormat format = WavFormat::S24;
    uint32_t blockFrames = 512;
};

class ExportJob {
public:
    // `input` is the working file's audio at the session rate (copied). `session` lists the processors.
    // Modules are loaded here, on the calling (UI) thread, so the worker only reads the registry.
    // Returns null with `error` if the session cannot be rendered (a missing module, say).
    static std::shared_ptr<ExportJob> start(WorkerPool& pool, ModuleRegistry& registry,
                                            const SessionDescription& session, const AudioBuffer& input,
                                            const ExportOptions& options, std::string& error);

    float progress() const { return progress_.load(std::memory_order_relaxed); }
    bool done() const { return done_.load(std::memory_order_acquire); }
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }
    // Valid once done(): success, cancelled (no error text), or the failure reason.
    bool succeeded() const { return ok_; }
    bool cancelled() const { return cancelled_; }
    const std::string& error() const { return error_; }
    const std::filesystem::path& output() const { return options_.out; }

private:
    ExportJob() = default;
    void run();

    ModuleRegistry* registry_ = nullptr;
    SessionDescription session_;
    AudioBuffer input_;
    ExportOptions options_;
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> cancel_{false}, done_{false};
    bool ok_ = false, cancelled_ = false;
    std::string error_;
};

} // namespace dsptb
