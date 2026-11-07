#pragma once

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsptb {

struct LogEntry {
    spdlog::level::level_enum level = spdlog::level::info;
    std::string text; // fully formatted line, no trailing newline
};

// Thread-safe bounded ring of recent log lines, fed by a spdlog sink and read by the Log panel.
// Never used from the audio thread (it locks and allocates); the audio thread posts event codes instead.
class LogBuffer {
public:
    explicit LogBuffer(size_t capacity = 2000) : capacity_(capacity) {}

    void push(spdlog::level::level_enum level, std::string text);
    void clear();
    // Copies the current contents (oldest first) into `out`.
    void snapshot(std::vector<LogEntry>& out) const;
    // Increments on every push/clear; lets the UI skip copying when nothing changed.
    uint64_t version() const noexcept { return version_.load(std::memory_order_acquire); }

    // Creates a spdlog sink that feeds this buffer. The buffer must outlive the sink.
    std::shared_ptr<spdlog::sinks::sink> makeSink();

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
    std::atomic<uint64_t> version_{0};
};

// Sets up the default spdlog logger: rotating file (5 x 2 MB) in `logDirectory` when non-empty, plus the
// ring sink. Returns the ring buffer, which stays valid until shutdownLogging().
std::shared_ptr<LogBuffer> initLogging(const std::filesystem::path& logDirectory);
void shutdownLogging();

} // namespace dsptb
