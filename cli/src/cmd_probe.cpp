// probe: open a real audio stream (silent output) and report what the device actually gave us.
#include "commands.hpp"

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/engine.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>

namespace dsptb::cli {

namespace {

struct ProbeOptions {
    std::string backend = "wasapi-shared";
    uint32_t rate = 48000;
    uint32_t block = 256;
    uint32_t milliseconds = 1000;
    bool capture = false;
    bool asJson = false;
    std::string inputId, outputId;
};

int runProbe(const ProbeOptions& o) {
    AudioConfig cfg;
    if (!parseBackendKind(o.backend, cfg.backend) || cfg.backend == BackendKind::Null) {
        std::fprintf(stderr, "error: --backend must be one of: %s\n", backendList().c_str());
        return 2;
    }
    cfg.sampleRate = o.rate;
    cfg.bufferFrames = o.block;
    cfg.capture = o.capture;
    cfg.inputDeviceId = o.inputId;
    cfg.outputDeviceId = o.outputId;

    Engine engine;
    nlohmann::json j;
    j["backend"] = o.backend;
    j["capture"] = o.capture;
    std::string error;
    if (!engine.open(cfg, error) || !engine.start(error)) {
        j["ok"] = false;
        j["error"] = error;
        if (o.asJson)
            std::cout << j.dump(2) << "\n";
        else
            std::fprintf(stderr, "probe failed: %s\n", error.c_str());
        return 1;
    }

    // Microphone path: select the Mic source with monitoring off, so nothing audible is produced, and watch
    // the PRE tap to measure what the capture device delivers.
    TapRing pre(engine.sampleRate() * 2);
    if (o.capture) {
        auto snap = std::make_unique<ChainSnapshot>();
        snap->preTaps.push_back(&pre);
        engine.publishChain(std::move(snap));
        engine.setMonitorMic(false);
        engine.setSource(SourceKind::Mic);
    }

    double peak = 0.0, sumSq = 0.0;
    uint64_t samples = 0;
    // Dropout detection: a real microphone never delivers long runs of exact digital zero, so a run of zero
    // frames means the stream had a gap that was filled with silence (the cause of rhythmic clicks in a
    // monitor path).
    constexpr size_t kZeroRun = 8;
    size_t run = 0;
    uint64_t zeroRuns = 0, zeroFrames = 0, frames = 0;
    std::vector<float> buf(8192);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.milliseconds);
    std::vector<float> carry; // keeps frame alignment between reads
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (size_t n; (n = pre.ring.read(buf.data(), buf.size() & ~size_t(1))) > 0;) {
            for (size_t i = 0; i + 1 < n; i += 2) {
                const double l = buf[i], r = buf[i + 1];
                peak = std::max({peak, std::fabs(l), std::fabs(r)});
                sumSq += l * l + r * r;
                samples += 2;
                ++frames;
                if (l == 0.0 && r == 0.0) {
                    ++zeroFrames;
                    if (++run == kZeroRun)
                        ++zeroRuns;
                } else {
                    run = 0;
                }
            }
        }
        engine.collectGarbage();
    }

    j["ok"] = true;
    j["sampleRate"] = engine.sampleRate();
    j["bufferFrames"] = engine.bufferFrames();
    j["latencyMs"] = engine.latencyMs();
    j["callbacks"] = engine.callbacks();
    j["underruns"] = engine.underruns();
    j["dspLoadPercent"] = static_cast<double>(engine.dspLoad()) * 100.0;
    j["deviceLost"] = engine.deviceLost();
    j["details"] = engine.describeBackend();
    if (o.capture) {
        j["inputSamples"] = samples;
        j["inputPeakDbfs"] = linearToDbfs(peak);
        j["inputRmsDbfs"] = linearToDbfs(samples ? std::sqrt(sumSq / static_cast<double>(samples)) : 0.0);
        j["tapDroppedFrames"] = pre.droppedFrames.load();
        j["inputFrames"] = frames;
        j["zeroFrameRuns"] = zeroRuns;
        j["zeroFrames"] = zeroFrames;
    }
    engine.close();

    if (o.asJson) {
        std::cout << j.dump(2) << "\n";
    } else {
        std::printf("%s: %u Hz, %u frames, latency %.1f ms, %llu callbacks, %llu underruns, DSP %.2f %%\n",
                    o.backend.c_str(), j["sampleRate"].get<uint32_t>(), j["bufferFrames"].get<uint32_t>(),
                    j["latencyMs"].get<double>(),
                    static_cast<unsigned long long>(j["callbacks"].get<uint64_t>()),
                    static_cast<unsigned long long>(j["underruns"].get<uint64_t>()),
                    j["dspLoadPercent"].get<double>());
        if (o.capture)
            std::printf("microphone: %llu samples, peak %.1f dBFS, RMS %.1f dBFS\n",
                        static_cast<unsigned long long>(j["inputSamples"].get<uint64_t>()),
                        j["inputPeakDbfs"].get<double>(), j["inputRmsDbfs"].get<double>());
    }
    return 0;
}

} // namespace

void addProbeCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand(
        "probe", "Open a real audio stream with silent output and report what the device provides");
    auto o = std::make_shared<ProbeOptions>();
    cmd->add_option("--backend", o->backend, "Audio backend: " + backendList() + " (default wasapi-shared)");
    cmd->add_option("--rate", o->rate, "Sample rate (exclusive mode only)")->capture_default_str();
    cmd->add_option("--block", o->block, "Buffer size in frames")->capture_default_str();
    cmd->add_option("--ms", o->milliseconds, "How long to run")->capture_default_str();
    cmd->add_flag("--capture", o->capture, "Also open the default microphone and measure its level");
    cmd->add_option("--input", o->inputId, "Input device id (from `devices --json`)");
    cmd->add_option("--output", o->outputId, "Output device id (from `devices --json`)");
    cmd->add_flag("--json", o->asJson, "Print JSON");
    cmd->callback([&exitCode, o] { exitCode = runProbe(*o); });
}

} // namespace dsptb::cli
