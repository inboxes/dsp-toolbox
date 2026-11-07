#pragma once

#include "engine.hpp"
#include "file_loader.hpp"
#include "notifications.hpp"
#include "paths.hpp"
#include "worker_pool.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dsptb {

// A message the UI should show as a banner.
struct AudioMessage {
    NoticeLevel level = NoticeLevel::Info;
    std::string text;
};

// UI-thread owner of everything audio: the engine, the worker pool, the loaded file, the loop region (kept in
// seconds so it survives a sample-rate change) and device-lost recovery. UI-agnostic.
class AudioSession {
public:
    AudioSession();
    ~AudioSession();

    // ---- stream configuration ----
    // Stops the stream, applies `config`, and restarts. If the new configuration cannot be opened the
    // previous one is restored and false is returned with the reason in `error`. The capture flag
    // is managed by setSource() and is ignored here.
    bool applyConfig(AudioConfig config, std::string& error);
    const AudioConfig& config() const { return config_; }

    Engine& engine() { return engine_; }
    const Engine& engine() const { return engine_; }

    // ---- working file (File source) ----
    // Decodes on the worker pool at the engine's sample rate. Playback stops; poll tick() for completion.
    void openFile(const std::filesystem::path& file);
    // Unloads the working file (New session). Playback stops.
    void closeFile();
    bool loading() const { return job_ && !job_->done.load(std::memory_order_acquire); }
    float loadProgress() const { return job_ ? job_->progress.load(std::memory_order_relaxed) : 0.0f; }
    std::string loadingName() const { return job_ ? toUtf8(job_->path.filename()) : std::string(); }
    std::shared_ptr<const LoadedFile> file() const { return file_; }
    double durationSeconds() const { return file_ ? file_->seconds : 0.0; }
    double positionSeconds() const;
    void seekSeconds(double seconds);
    // Loop region in seconds. An empty region (end <= start) loops the whole file.
    void setLoopSeconds(bool enabled, double startSeconds, double endSeconds);
    bool loopEnabled() const { return loopOn_; }
    double loopStartSeconds() const { return loopStartSec_; }
    double loopEndSeconds() const;

    // ---- source ----
    // Selecting Mic reopens the stream with capture; selecting File reopens it playback-only (releases the
    // mic).
    bool setSource(SourceKind source, std::string& error);
    SourceKind source() const { return engine_.source(); }

    // ---- housekeeping ----
    // Call every UI frame: finishes loads, frees retired objects, drains engine events (logging them),
    // retries a lost device every 2 s. `now` is any monotonically increasing clock in seconds. New banners
    // are appended.
    void tick(double now, std::vector<AudioMessage>& messages);
    // True while the stream is down (unplugged device or failed open) and we are retrying.
    bool deviceLost() const { return lost_; }

private:
    bool openStream(AudioConfig config, std::string& error);
    void finishLoad(std::vector<AudioMessage>& messages);
    void applyLoop();
    void reloadFileForNewRate();
    void retryStream(std::vector<AudioMessage>& messages);

    Engine engine_;
    WorkerPool pool_{2};
    AudioConfig config_;
    bool streamWanted_ = false;
    bool wantCapture_ = false;

    std::shared_ptr<LoadJob> job_;
    std::shared_ptr<const LoadedFile> file_;
    double pendingSeekSeconds_ = 0.0;

    bool loopOn_ = false;
    double loopStartSec_ = 0.0;
    double loopEndSec_ = 0.0;

    bool lost_ = false;
    double lastRetry_ = 0.0;
};

} // namespace dsptb
