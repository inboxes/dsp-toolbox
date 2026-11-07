#pragma once

// Recorder: records the PRE (input) or POST (output) signal to a 32-bit float
// stereo WAV. The engine writes the signal into a 4 s tap ring on the audio thread (never blocking); a writer
// thread drains it every 20 ms into the file. Frames the ring could not take are counted as overruns.

#include "session_model.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace dsptb {

class Recorder {
public:
    Recorder();
    ~Recorder(); // stops a recording that is still running

    // Opens <folder>/rec_YYYYMMDD_HHMMSS.wav (the folder is created if needed) and starts. Fails with
    // `error` if a recording is running or the file cannot be created.
    bool start(SessionModel& model, uint32_t sampleRate, dsptb_tap tap, const std::filesystem::path& folder,
               std::string& error);
    // Detaches the tap, writes what is left in the ring and finalises the file. Returns its path.
    std::filesystem::path stop(SessionModel& model);

    bool active() const { return active_.load(std::memory_order_acquire); }
    double elapsedSeconds() const;
    uint64_t framesWritten() const { return written_.load(std::memory_order_relaxed); }
    uint64_t overrunFrames() const;
    const std::filesystem::path& path() const { return path_; }
    // Set when the writer thread could not write (disk full, file removed); the recording stops.
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    std::string failure() const { return failure_; }

private:
    struct Impl;
    void writerLoop();

    std::unique_ptr<Impl> impl_;
    std::shared_ptr<TapRing> ring_;
    std::thread thread_;
    std::atomic<bool> active_{false}, quit_{false}, failed_{false};
    std::atomic<uint64_t> written_{0}, lastDropped_{0};
    uint32_t sampleRate_ = 0;
    std::filesystem::path path_;
    std::string failure_;
};

} // namespace dsptb
