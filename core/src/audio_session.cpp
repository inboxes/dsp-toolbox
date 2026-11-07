#include "dsptb/core/audio_session.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace dsptb {

AudioSession::AudioSession() = default;

AudioSession::~AudioSession() {
    if (job_)
        job_->cancel = true;
    engine_.close();
    // pool_ is destroyed after this body: it finishes any queued decode (cancelled ones return quickly).
}

// ----------------------------------------------------------------------------------------------------------
// stream
// ----------------------------------------------------------------------------------------------------------

bool AudioSession::openStream(AudioConfig config, std::string& error) {
    config.capture = wantCapture_;
    if (!engine_.open(config, error))
        return false;
    if (!engine_.start(error)) {
        engine_.close();
        return false;
    }
    config_ = config;
    return true;
}

bool AudioSession::applyConfig(AudioConfig config, std::string& error) {
    streamWanted_ = true;
    const bool hadStream = engine_.isOpen();
    const AudioConfig previous = config_;
    const uint32_t previousRate = engine_.sampleRate();

    engine_.close();
    if (openStream(config, error)) {
        lost_ = false;
        spdlog::info("Audio: {} at {} Hz, {} frames, latency {:.1f} ms", backendDisplayName(config_.backend),
                     engine_.sampleRate(), engine_.bufferFrames(), engine_.latencyMs());
        if (engine_.sampleRate() != previousRate)
            reloadFileForNewRate();
        return true;
    }
    // the new configuration failed, so go back to the last one that worked.
    spdlog::warn("Audio: could not apply configuration: {}", error);
    if (hadStream) {
        std::string ignored;
        if (!openStream(previous, ignored))
            lost_ = true;
    } else {
        lost_ = true;
    }
    return false;
}

void AudioSession::retryStream(std::vector<AudioMessage>& messages) {
    std::string error;
    AudioConfig config = config_;
    bool ok = openStream(config, error);
    if (!ok && (!config.inputDeviceId.empty() || !config.outputDeviceId.empty())) {
        config.inputDeviceId.clear(); // fall back to the system default device
        config.outputDeviceId.clear();
        ok = openStream(config, error);
    }
    if (ok) {
        lost_ = false;
        spdlog::info("Audio: device restored ({} Hz, {} frames)", engine_.sampleRate(),
                     engine_.bufferFrames());
        messages.push_back({NoticeLevel::Info, "Audio device restored."});
        if (engine_.sampleRate() != (file_ ? file_->audio.sampleRate : engine_.sampleRate()))
            reloadFileForNewRate();
    }
}

bool AudioSession::setSource(SourceKind source, std::string& error) {
    const bool needCapture = source == SourceKind::Mic;
    if (needCapture != wantCapture_ || (needCapture && !engine_.captureActive())) {
        wantCapture_ = needCapture;
        if (engine_.isOpen()) {
            // Pause the file first so nothing is cut abruptly by the reopen, then reopen with/without
            // capture.
            engine_.pause();
            engine_.close();
            if (!openStream(config_, error)) {
                wantCapture_ = false;
                std::string ignored;
                if (!openStream(config_, ignored))
                    lost_ = true;
                return false;
            }
        }
    }
    engine_.setSource(source);
    if (source == SourceKind::Mic)
        engine_.pause(); // the transport belongs to the file source
    return true;
}

// ----------------------------------------------------------------------------------------------------------
// file and transport
// ----------------------------------------------------------------------------------------------------------

void AudioSession::openFile(const std::filesystem::path& file) {
    if (job_)
        job_->cancel = true; // superseded
    engine_.pause();
    const uint32_t rate = engine_.sampleRate() ? engine_.sampleRate() : config_.sampleRate;
    job_ = startLoad(pool_, file, rate);
    pendingSeekSeconds_ = 0.0;
    spdlog::info("Loading {} at {} Hz", toUtf8(file), rate);
}

void AudioSession::closeFile() {
    if (job_)
        job_->cancel = true;
    job_.reset();
    engine_.pause();
    file_.reset();
    engine_.setFile(nullptr);
    pendingSeekSeconds_ = 0.0;
}

double AudioSession::positionSeconds() const {
    const uint32_t rate = engine_.sampleRate();
    return rate ? static_cast<double>(engine_.positionFrames()) / rate : 0.0;
}

void AudioSession::seekSeconds(double seconds) {
    const uint32_t rate = engine_.sampleRate();
    if (!rate || !file_)
        return;
    const double clamped = std::clamp(seconds, 0.0, file_->seconds);
    engine_.seekFrames(static_cast<uint64_t>(clamped * rate));
}

double AudioSession::loopEndSeconds() const {
    return loopEndSec_ > loopStartSec_ ? loopEndSec_ : durationSeconds();
}

void AudioSession::setLoopSeconds(bool enabled, double startSeconds, double endSeconds) {
    loopOn_ = enabled;
    loopStartSec_ = std::max(0.0, startSeconds);
    loopEndSec_ = endSeconds;
    applyLoop();
}

void AudioSession::applyLoop() {
    const uint32_t rate = engine_.sampleRate();
    if (!rate || !file_)
        return;
    const double end = std::min(loopEndSeconds(), file_->seconds);
    const double start = std::min(loopStartSec_, end);
    engine_.setLoop(loopOn_, static_cast<uint64_t>(start * rate), static_cast<uint64_t>(end * rate));
}

void AudioSession::reloadFileForNewRate() {
    if (!file_)
        return;
    const double position = positionSeconds();
    openFile(fromUtf8(file_->path));
    pendingSeekSeconds_ = position;
}

void AudioSession::finishLoad(std::vector<AudioMessage>& messages) {
    std::shared_ptr<LoadJob> job = std::move(job_);
    job_.reset();
    if (!job->result) {
        if (job->error != "cancelled") {
            spdlog::error("Could not load {}: {}", toUtf8(job->path), job->error);
            messages.push_back(
                {NoticeLevel::Error, "Could not open " + toUtf8(job->path.filename()) + ": " + job->error});
        }
        return;
    }
    if (engine_.sampleRate() != 0 && job->result->audio.sampleRate != engine_.sampleRate()) {
        // The device rate changed while decoding: decode again at the new rate.
        spdlog::info("Sample rate changed during load; decoding again at {} Hz", engine_.sampleRate());
        job_ = startLoad(pool_, job->path, engine_.sampleRate());
        return;
    }
    file_ = job->result;
    engine_.setFile(file_);
    applyLoop();
    if (pendingSeekSeconds_ > 0.0)
        seekSeconds(pendingSeekSeconds_);
    pendingSeekSeconds_ = 0.0;
    spdlog::info("Loaded {} ({:.2f} s, {} Hz)", file_->path, file_->seconds, file_->audio.sampleRate);
    if (file_->seconds > 3600.0)
        messages.push_back(
            {NoticeLevel::Warning, "This file is longer than 60 minutes; it is held in memory."});
}

// ----------------------------------------------------------------------------------------------------------
// per-frame housekeeping
// ----------------------------------------------------------------------------------------------------------

void AudioSession::tick(double now, std::vector<AudioMessage>& messages) {
    if (job_ && job_->done.load(std::memory_order_acquire))
        finishLoad(messages);
    engine_.collectGarbage();

    EngineEvent ev;
    while (engine_.popEvent(ev)) {
        switch (ev.type) {
        case EngineEventType::Underrun:
            spdlog::warn("Audio underrun: the callback took {}% of the block period", ev.value);
            messages.push_back({NoticeLevel::Warning, "Audio underrun: try a larger buffer size."});
            break;
        case EngineEventType::Clip:
            spdlog::info("Output clipped at 0 dBFS");
            break;
        case EngineEventType::NonFinite:
            spdlog::warn("Output contained {} NaN/Inf sample(s); they were replaced with silence", ev.value);
            messages.push_back(
                {NoticeLevel::Warning, "Output contained invalid samples (NaN/Inf) and was repaired."});
            break;
        case EngineEventType::EndOfFile:
            spdlog::debug("End of file reached");
            break;
        case EngineEventType::TapOverrun:
            spdlog::warn("An analysis tap overran");
            break;
        case EngineEventType::ModuleFaulted: // the SessionModel raises the banner; this is for the log
            spdlog::warn("Module instance {} faulted on the audio thread (reason {})", ev.value, ev.extra);
            break;
        }
    }

    // a device that went away is retried every 2 s (the default device if the chosen one is gone).
    const bool down = streamWanted_ && (!engine_.isOpen() || engine_.deviceLost());
    if (down && !lost_) {
        lost_ = true;
        lastRetry_ = now;
        spdlog::warn("Audio device lost{}{}", engine_.isOpen() ? ": " : "", engine_.describeBackend());
        messages.push_back({NoticeLevel::Error, "Audio device lost. Retrying every 2 seconds."});
        engine_.stop(); // a clean stop frees the dead stream; retry reopens it
    } else if (lost_ && now - lastRetry_ >= 2.0) {
        lastRetry_ = now;
        engine_.close();
        retryStream(messages);
    }
}

} // namespace dsptb
