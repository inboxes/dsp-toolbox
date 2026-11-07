#include "dsptb/core/recorder.hpp"

#include "dsptb/core/paths.hpp"

#include <miniaudio.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <vector>

namespace dsptb {

struct Recorder::Impl {
    ma_encoder encoder{};
    bool open = false;
};

namespace {
constexpr double kRingSeconds = 4.0;
constexpr auto kDrainPeriod = std::chrono::milliseconds(20);

std::string timestampName() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[40];
    std::strftime(buf, sizeof(buf), "rec_%Y%m%d_%H%M%S.wav", &tm);
    return buf;
}
} // namespace

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}

Recorder::~Recorder() {
    if (thread_.joinable()) {
        quit_.store(true);
        thread_.join();
    }
    if (impl_->open)
        ma_encoder_uninit(&impl_->encoder);
}

bool Recorder::start(SessionModel& model, uint32_t sampleRate, dsptb_tap tap,
                     const std::filesystem::path& folder, std::string& error) {
    if (active()) {
        error = "a recording is already running";
        return false;
    }
    if (sampleRate == 0) {
        error = "the audio stream is not running";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    path_ = folder / timestampName();
    // Two recordings in the same second would collide; the second gets a counter.
    for (int n = 2; std::filesystem::exists(path_, ec) && n < 100; ++n) {
        std::string name = timestampName();
        name.insert(name.size() - 4, "_" + std::to_string(n));
        path_ = folder / name;
    }
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, sampleRate);
    if (ma_encoder_init_file_w(path_.c_str(), &cfg, &impl_->encoder) != MA_SUCCESS) {
        error = "cannot create " + toUtf8(path_);
        return false;
    }
    impl_->open = true;
    sampleRate_ = sampleRate;
    written_.store(0);
    failed_.store(false);
    failure_.clear();
    quit_.store(false);
    ring_ = std::make_shared<TapRing>(static_cast<size_t>(kRingSeconds * sampleRate));
    active_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { writerLoop(); });
    model.setRecordTap(ring_, tap);
    spdlog::info("Recording {} to {}", tap == DSPTB_TAP_PRE ? "PRE" : "POST", toUtf8(path_));
    return true;
}

void Recorder::writerLoop() {
    std::vector<float> buf(4096 * 2);
    const auto drain = [&] {
        for (;;) {
            const size_t floats = ring_->ring.read(buf.data(), buf.size());
            if (floats == 0)
                return;
            ma_uint64 wrote = 0;
            const ma_uint64 frames = floats / 2;
            if (ma_encoder_write_pcm_frames(&impl_->encoder, buf.data(), frames, &wrote) != MA_SUCCESS ||
                wrote != frames) {
                failure_ = "writing the recording failed (disk full?)";
                failed_.store(true, std::memory_order_release);
                return;
            }
            written_.fetch_add(frames, std::memory_order_relaxed);
        }
    };
    while (!quit_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire)) {
        drain();
        std::this_thread::sleep_for(kDrainPeriod);
    }
    if (!failed_.load())
        drain(); // what arrived while we were told to stop
}

std::filesystem::path Recorder::stop(SessionModel& model) {
    if (!active())
        return path_;
    model.setRecordTap(nullptr, DSPTB_TAP_POST); // the engine stops writing once it has the new snapshot
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
    ma_encoder_uninit(&impl_->encoder);
    impl_->open = false;
    active_.store(false, std::memory_order_release);
    const uint64_t dropped = overrunFrames();
    lastDropped_.store(dropped);
    spdlog::info("Recording stopped: {} frames written, {} dropped", written_.load(), dropped);
    ring_.reset();
    return path_;
}

double Recorder::elapsedSeconds() const {
    return sampleRate_ ? static_cast<double>(written_.load(std::memory_order_relaxed)) / sampleRate_ : 0.0;
}

uint64_t Recorder::overrunFrames() const {
    return ring_ ? ring_->droppedFrames.load(std::memory_order_relaxed) : lastDropped_.load();
}

} // namespace dsptb
