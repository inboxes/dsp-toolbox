#include "dsptb/core/file_loader.hpp"
#include "dsptb/core/paths.hpp"

#include "dsptb/core/audio_io.hpp"

namespace dsptb {

namespace {

std::shared_ptr<LoadedFile> decode(const std::filesystem::path& path, uint32_t rate, std::string& error,
                                   std::atomic<float>* progress, const std::atomic<bool>* cancel) {
    auto file = std::make_shared<LoadedFile>();
    file->path = toUtf8(path); // UTF-8 text; convert back with fromUtf8
    if (!decodeToStereo(path, rate, file->audio, error, progress, cancel))
        return nullptr;
    file->overview = computeOverview(file->audio);
    file->seconds = static_cast<double>(file->audio.frames()) / static_cast<double>(file->audio.sampleRate);
    return file;
}

} // namespace

std::shared_ptr<LoadJob> startLoad(WorkerPool& pool, const std::filesystem::path& file, uint32_t sampleRate) {
    auto job = std::make_shared<LoadJob>();
    job->path = file;
    job->sampleRate = sampleRate;
    pool.submit([job] {
        std::string error;
        auto result = decode(job->path, job->sampleRate, error, &job->progress, &job->cancel);
        job->result = std::move(result);
        job->error = std::move(error);
        job->done.store(true, std::memory_order_release);
    });
    return job;
}

std::shared_ptr<LoadedFile> loadFileBlocking(const std::filesystem::path& file, uint32_t sampleRate,
                                             std::string& error) {
    return decode(file, sampleRate, error, nullptr, nullptr);
}

} // namespace dsptb
