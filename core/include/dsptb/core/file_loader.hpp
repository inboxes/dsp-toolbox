#pragma once

#include "engine.hpp"
#include "worker_pool.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace dsptb {

// One background decode of a file into a LoadedFile (decode, resample, downmix, overview).
// The UI polls `done`; once it is true, `result` or `error` is valid and no longer changes.
struct LoadJob {
    std::filesystem::path path;
    uint32_t sampleRate = 0;

    std::atomic<float> progress{0.0f};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::shared_ptr<LoadedFile> result; // null on failure
    std::string error;
};

std::shared_ptr<LoadJob> startLoad(WorkerPool& pool, const std::filesystem::path& file, uint32_t sampleRate);

// Synchronous variant for tools and tests.
std::shared_ptr<LoadedFile> loadFileBlocking(const std::filesystem::path& file, uint32_t sampleRate,
                                             std::string& error);

} // namespace dsptb
