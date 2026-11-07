// latency: measure the real round-trip delay of an audio configuration with an acoustic loopback.
//
// A short chirp is played through the OUTPUT device while the INPUT device records. The same backend code the
// app uses runs the stream (including the drift-compensating bridge in exclusive mode). For every chirp the
// delay is found by cross-correlating the recording with the chirp. The round trip is
//   output buffering + acoustic travel + input buffering (+ the duplex bridge's cushion),
// which is exactly the delay you hear when the microphone is monitored (monitor path = input + output
// latency), plus about 3 ms per metre of air between speaker and microphone. With a loopback cable (or a
// virtual "stereo mix" input) the acoustic part is zero.
#include "commands.hpp"

#include <dsptb/core/backend.hpp>
#include <dsptb/core/signal_gen.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

namespace dsptb::cli {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct LatencyOptions {
    std::string backend = "wasapi-shared";
    uint32_t rate = 48000;
    uint32_t block = 256;
    uint32_t clicks = 6;
    double dbfs = -24.0;
    bool asJson = false;
    std::string inputId, outputId;
};

// Plays the chirp train and records the input, indexed by the callback's own frame counter.
class Tester final : public IAudioCallback {
public:
    Tester(std::vector<float> chirp, uint32_t spacingFrames, uint32_t count, double seconds, uint32_t rate)
        : chirp_(std::move(chirp)), spacing_(spacingFrames), count_(count),
          rec_(static_cast<size_t>(seconds * rate), 0.0f) {}

    void onAudio(const float* in, float* out, uint32_t frames) noexcept override {
        for (uint32_t i = 0; i < frames; ++i) {
            const uint64_t n = counter_ + i;
            float v = 0.0f;
            // Start after half a second so the streams are settled.
            if (n >= start_) {
                const uint64_t rel = n - start_;
                const uint64_t k = rel / spacing_;
                const uint64_t off = rel % spacing_;
                if (k < count_ && off < chirp_.size())
                    v = chirp_[off];
            }
            out[2 * i] = v;
            out[2 * i + 1] = v;
            if (in && n < rec_.size())
                rec_[n] = 0.5f * (in[2 * i] + in[2 * i + 1]);
        }
        counter_ += frames;
        done_.store(counter_, std::memory_order_relaxed);
    }

    uint64_t framesSeen() const { return done_.load(std::memory_order_relaxed); }
    const std::vector<float>& recording() const { return rec_; }
    uint64_t chirpStart(uint32_t k) const { return start_ + static_cast<uint64_t>(k) * spacing_; }
    void setStart(uint64_t s) { start_ = s; }

private:
    std::vector<float> chirp_;
    uint32_t spacing_;
    uint32_t count_;
    std::vector<float> rec_;
    uint64_t start_ = 24000;
    uint64_t counter_ = 0;
    std::atomic<uint64_t> done_{0};
};

std::vector<float> makeChirp(uint32_t rate, double dbfs) {
    const size_t n = static_cast<size_t>(0.008 * rate); // 8 ms
    std::vector<float> c(n);
    const double f0 = 700.0, f1 = 7000.0, T = static_cast<double>(n) / rate, k = std::log(f1 / f0);
    const double amp = dbfsToLinear(dbfs);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double window =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        c[i] =
            static_cast<float>(amp * window * std::sin(2.0 * kPi * f0 * T / k * (std::exp(k * t / T) - 1.0)));
    }
    return c;
}

struct Hit {
    double delayFrames = 0.0;
    double quality = 0.0; // normalised correlation peak, 0..1
};

// Normalised cross-correlation of the chirp against the recording in [from, from+window).
Hit locate(const std::vector<float>& rec, const std::vector<float>& chirp, size_t from, size_t window) {
    Hit best;
    double chirpEnergy = 0.0;
    for (float v : chirp)
        chirpEnergy += static_cast<double>(v) * v;
    size_t bestPos = 0;
    for (size_t d = 0; d < window; ++d) {
        const size_t base = from + d;
        if (base + chirp.size() >= rec.size())
            break;
        double dot = 0.0, e = 0.0;
        for (size_t i = 0; i < chirp.size(); ++i) {
            const double r = rec[base + i];
            dot += r * chirp[i];
            e += r * r;
        }
        const double q = (e > 0.0 && chirpEnergy > 0.0) ? dot / std::sqrt(e * chirpEnergy) : 0.0;
        if (q > best.quality) {
            best.quality = q;
            bestPos = d;
        }
    }
    best.delayFrames = static_cast<double>(bestPos);
    return best;
}

int runLatency(const LatencyOptions& o) {
    AudioConfig cfg;
    if (!parseBackendKind(o.backend, cfg.backend) || cfg.backend == BackendKind::Null) {
        std::fprintf(stderr, "error: --backend must be one of: %s\n", backendList().c_str());
        return 2;
    }
    cfg.sampleRate = o.rate;
    cfg.bufferFrames = o.block;
    cfg.capture = true;
    cfg.inputDeviceId = o.inputId;
    cfg.outputDeviceId = o.outputId;

    auto backend = createBackend(cfg.backend);
    const double seconds = 1.0 + static_cast<double>(o.clicks) * 0.5 + 1.0;
    auto chirp = makeChirp(o.rate, o.dbfs);
    // The stream may run at a different rate than requested (shared mode): open first, then size the tester.
    std::string error;
    Tester* testerPtr = nullptr;
    struct Forward final : IAudioCallback {
        Tester*& target;
        explicit Forward(Tester*& t) : target(t) {}
        void onAudio(const float* in, float* out, uint32_t frames) noexcept override {
            if (target)
                target->onAudio(in, out, frames);
            else
                std::fill_n(out, static_cast<size_t>(frames) * 2, 0.0f);
        }
    } forward(testerPtr);

    if (!backend->open(cfg, &forward, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const uint32_t rate = backend->sampleRate();
    if (rate != o.rate)
        chirp = makeChirp(rate, o.dbfs);
    Tester tester(chirp, rate / 2, o.clicks, seconds, rate);
    tester.setStart(rate / 2);
    testerPtr = &tester;
    if (!backend->start(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds + 0.5);
    while (std::chrono::steady_clock::now() < end &&
           tester.framesSeen() < static_cast<uint64_t>(seconds * rate))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    backend->stop();
    const double reportedMs = 1000.0 * backend->latencyFrames() / rate;
    const std::string details = backend->describe();
    backend->close();

    // Analyse.
    std::vector<double> delaysMs, qualities, allDelays;
    for (uint32_t k = 0; k < o.clicks; ++k) {
        const size_t from = static_cast<size_t>(tester.chirpStart(k));
        const Hit h = locate(tester.recording(), chirp, from, static_cast<size_t>(0.3 * rate));
        qualities.push_back(h.quality);
        allDelays.push_back(1000.0 * h.delayFrames / rate);
        if (h.quality >= 0.5)
            delaysMs.push_back(1000.0 * h.delayFrames / rate);
    }
    // Level of the recording: the loudest 8 ms window after each chirp versus the median window (noise
    // floor).
    double recPeak = 0.0;
    for (float v : tester.recording())
        recPeak = std::max(recPeak, static_cast<double>(std::fabs(v)));
    std::sort(delaysMs.begin(), delaysMs.end());
    const bool ok = delaysMs.size() >= std::max<size_t>(2, o.clicks / 2);
    const double median = delaysMs.empty() ? 0.0 : delaysMs[delaysMs.size() / 2];
    const double spread = delaysMs.size() >= 2 ? delaysMs.back() - delaysMs.front() : 0.0;
    double meanQ = 0.0;
    for (double q : qualities)
        meanQ += q / static_cast<double>(qualities.size());

    // The latency target: buffer <= 10 ms exclusive, <= 20 ms shared. Those are
    // per-direction buffer sizes, i.e. the reported latency of the configuration; the measured round trip
    // includes both directions and the air.
    const bool exclusive = cfg.backend == BackendKind::WasapiExclusive;
    const double specBufferMs = exclusive ? 10.0 : 20.0;

    if (o.asJson) {
        nlohmann::json j;
        j["ok"] = ok;
        j["backend"] = o.backend;
        j["sampleRate"] = rate;
        j["requestedBlock"] = o.block;
        j["clicksHeard"] = delaysMs.size();
        j["clicksPlayed"] = o.clicks;
        j["roundTripMs"] = median;
        j["minMs"] = delaysMs.empty() ? 0.0 : delaysMs.front();
        j["maxMs"] = delaysMs.empty() ? 0.0 : delaysMs.back();
        j["spreadMs"] = spread;
        j["meanCorrelation"] = meanQ;
        j["reportedBufferLatencyMs"] = reportedMs;
        j["specBufferLimitMs"] = specBufferMs;
        j["details"] = details;
        j["perClickDelayMs"] = allDelays;
        j["perClickCorrelation"] = qualities;
        j["recordingPeakDbfs"] = 20.0 * std::log10(std::max(recPeak, 1e-9));
        std::cout << j.dump(2) << "\n";
    } else {
        std::printf("%s, %u Hz, requested block %u\n", o.backend.c_str(), rate, o.block);
        if (!ok) {
            std::printf("Not enough clicks were heard (%zu of %u, mean correlation %.2f). Put the microphone "
                        "close to the speaker, raise "
                        "--dbfs, or use a loopback input.\n",
                        delaysMs.size(), o.clicks, meanQ);
        } else {
            std::printf("round trip (speaker -> microphone, includes air): median %.1f ms, range %.1f - %.1f "
                        "ms over %zu clicks\n",
                        median, delaysMs.front(), delaysMs.back(), delaysMs.size());
            std::printf("reported device + bridge buffering (output + input): %.1f ms\n", reportedMs);
        }
        if (!details.empty())
            std::printf("%s\n", details.c_str());
    }
    return ok ? 0 : 1;
}

} // namespace

void addLatencyCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand(
        "latency",
        "Measure the real round-trip latency with an acoustic loopback (plays short quiet chirps)");
    auto o = std::make_shared<LatencyOptions>();
    cmd->add_option("--backend", o->backend, "Audio backend: " + backendList() + " (default wasapi-shared)");
    cmd->add_option("--rate", o->rate, "Sample rate (exclusive mode only)")->capture_default_str();
    cmd->add_option("--block", o->block, "Buffer size in frames")->capture_default_str();
    cmd->add_option("--clicks", o->clicks, "Number of chirps")->capture_default_str();
    cmd->add_option("--dbfs", o->dbfs, "Chirp level in dBFS (keep it quiet)")->capture_default_str();
    cmd->add_option("--input", o->inputId, "Input device id");
    cmd->add_option("--output", o->outputId, "Output device id");
    cmd->add_flag("--json", o->asJson, "Print JSON");
    cmd->callback([&exitCode, o] { exitCode = runLatency(*o); });
}

} // namespace dsptb::cli
