#include "dsptb/core/logging.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <vector>

namespace dsptb {

void LogBuffer::push(spdlog::level::level_enum level, std::string text) {
    {
        std::lock_guard lock(mutex_);
        entries_.push_back({level, std::move(text)});
        while (entries_.size() > capacity_)
            entries_.pop_front();
    }
    version_.fetch_add(1, std::memory_order_release);
}

void LogBuffer::clear() {
    {
        std::lock_guard lock(mutex_);
        entries_.clear();
    }
    version_.fetch_add(1, std::memory_order_release);
}

void LogBuffer::snapshot(std::vector<LogEntry>& out) const {
    std::lock_guard lock(mutex_);
    out.assign(entries_.begin(), entries_.end());
}

namespace {

class RingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit RingSink(LogBuffer& buffer) : buffer_(buffer) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::string line(formatted.data(), formatted.size());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        buffer_.push(msg.level, std::move(line));
    }
    void flush_() override {}

private:
    LogBuffer& buffer_;
};

std::shared_ptr<LogBuffer> gBuffer;

// spdlog takes narrow filenames; the app manifest sets the UTF-8 active code page so this round-trips.
std::string utf8Path(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace

std::shared_ptr<spdlog::sinks::sink> LogBuffer::makeSink() {
    return std::make_shared<RingSink>(*this);
}

std::shared_ptr<LogBuffer> initLogging(const std::filesystem::path& logDirectory) {
    gBuffer = std::make_shared<LogBuffer>();
    std::vector<spdlog::sink_ptr> sinks;
    if (!logDirectory.empty()) {
        try {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                utf8Path(logDirectory / "dsptb.log"), 2 * 1024 * 1024, 5));
        } catch (const spdlog::spdlog_ex&) {
            // Log folder not writable: keep going with the in-app ring only.
        }
    }
    sinks.push_back(gBuffer->makeSink());
    auto logger = std::make_shared<spdlog::logger>("dsptb", sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    return gBuffer;
}

void shutdownLogging() {
    spdlog::shutdown();
    gBuffer.reset();
}

} // namespace dsptb
