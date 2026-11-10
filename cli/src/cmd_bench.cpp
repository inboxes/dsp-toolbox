// bench: callback timing of the real engine on the NullBackend
#include "commands.hpp"

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/engine.hpp>
#include <dsptb/core/gpu_runtime_loader.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/offline_render.hpp>
#include <dsptb/core/paths.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/signal_gen.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dsptb::cli {

namespace {

struct BenchOptions {
    std::string session;
    std::vector<std::string> moduleDirs;
    uint32_t rate = 48000;
    uint32_t block = 256;
    double seconds = 30.0;
    double soak = 0.0;
    bool asJson = false;
};

double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty())
        return 0.0;
    const size_t idx =
        std::min(sorted.size() - 1, static_cast<size_t>(p * static_cast<double>(sorted.size())));
    return sorted[idx];
}

int runBench(const BenchOptions& o) {
    namespace fs = std::filesystem;
    // A session file lists module instances (processors and analyzers) with params and state; they are added
    // exactly as the app would, and the real analysis thread runs alongside the audio callbacks.
    SessionDescription sessionDesc;
    if (!o.session.empty()) {
        std::ifstream sf(o.session, std::ios::binary);
        std::stringstream ss;
        ss << sf.rdbuf();
        std::string perr;
        if (!sf || !parseSessionDescription(ss.str(), sessionDesc, perr)) {
            std::fprintf(stderr, "error: cannot use session %s: %s\n", o.session.c_str(),
                         perr.empty() ? "unreadable" : perr.c_str());
            return 2;
        }
    }

    // Workload: a looping stereo file plays through the full engine path (source, taps point, safety stage).
    GenParams gp;
    gp.freq = 440.0;
    gp.dbfs = -6.0;
    gp.seconds = 10.0;
    gp.sampleRate = o.rate;
    auto file = std::make_shared<LoadedFile>();
    file->audio = pcmToStereo(generateSignal(gp));
    file->seconds = gp.seconds;

    if (appDir().empty()) {
        std::fprintf(stderr, "error: cannot determine the executable's folder\n");
        return 2;
    }
    loadGpuRuntimeFromSettings();
    ModuleRegistry registry;
    {
        std::vector<fs::path> folders{modulesDir()};
        for (const auto& d : o.moduleDirs)
            folders.push_back(fs::path(d));
        registry.scan(folders);
    }
    Engine engine;
    SessionModel model(engine, registry);
    AudioConfig cfg;
    cfg.backend = BackendKind::Null;
    cfg.sampleRate = o.rate;
    cfg.bufferFrames = o.block;
    std::string error;
    if (!engine.open(cfg, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }
    NullBackend* nb = engine.nullBackend();
    nb->setRealtime(false);
    if (!engine.start(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }
    for (const InstanceDescription& d : sessionDesc.instances) {
        std::string err;
        const InstanceId id = model.add(d.module, err);
        if (id == 0) {
            std::fprintf(stderr, "error: cannot add %s: %s\n", d.module.c_str(), err.c_str());
            return 2;
        }
        InstanceRecord* rec = model.find(id);
        if (!d.stateJson.empty())
            rec->instance->loadState(d.stateJson);
        for (const auto& [pid, value] : d.params) {
            const int idx = rec->instance->params().indexOf(pid);
            if (idx < 0) {
                std::fprintf(stderr, "error: %s has no parameter \"%s\"\n", d.module.c_str(), pid.c_str());
                return 2;
            }
            rec->instance->params().set(static_cast<uint32_t>(idx), value);
        }
        if (!d.enabled)
            model.setEnabled(id, false);
    }
    engine.setFile(file);
    engine.setLoop(true, 0, file->audio.frames());
    engine.play();

    const double runSeconds = o.soak > 0.0 ? o.soak : o.seconds;
    const size_t callbacks = static_cast<size_t>(runSeconds * o.rate / o.block);
    std::vector<double> micros;
    micros.reserve(callbacks);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    rt::resetRtGuardViolations();
    for (size_t i = 0; i < callbacks; ++i) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        nb->pump(o.block, o.block); // exactly one engine callback
        QueryPerformanceCounter(&t1);
        micros.push_back(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 /
                         static_cast<double>(freq.QuadPart));
        if ((i & 255) == 0) {
            engine.collectGarbage();
            std::vector<SessionMessage> messages; // faults would show up here
            model.tick(static_cast<double>(i) * o.block / o.rate, messages);
            for (const auto& msg : messages)
                std::fprintf(stderr, "session: %s\n", msg.text.c_str());
        }
    }

    std::vector<double> sorted = micros;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double m : micros)
        sum += m;
    const double mean = micros.empty() ? 0.0 : sum / static_cast<double>(micros.size());
    const double p50 = percentile(sorted, 0.50), p99 = percentile(sorted, 0.99),
                 mx = sorted.empty() ? 0.0 : sorted.back();
    const double periodUs = 1e6 * o.block / o.rate;
    const uint32_t hits = rt::rtGuardViolations();

    if (o.asJson) {
        nlohmann::json j;
        j["rate"] = o.rate;
        j["block"] = o.block;
        j["seconds"] = runSeconds;
        j["callbacks"] = callbacks;
        j["meanUs"] = mean;
        j["p50Us"] = p50;
        j["p99Us"] = p99;
        j["maxUs"] = mx;
        j["periodUs"] = periodUs;
        j["meanPercent"] = 100.0 * mean / periodUs;
        j["p99Percent"] = 100.0 * p99 / periodUs;
        j["maxPercent"] = 100.0 * mx / periodUs;
        j["rtGuardViolations"] = hits;
        j["underruns"] = engine.underruns();
        std::cout << j.dump(2) << "\n";
    } else {
        std::printf("bench: %zu callbacks of %u frames at %u Hz (%.1f s of audio, %zu module instance(s))\n",
                    callbacks, o.block, o.rate, runSeconds, sessionDesc.instances.size());
        std::printf(
            "callback time: mean %.2f us, p50 %.2f us, p99 %.2f us, max %.2f us (block period %.1f us)\n",
            mean, p50, p99, mx, periodUs);
        std::printf("percent of period: mean %.3f %%, p99 %.3f %%, max %.3f %%\n", 100.0 * mean / periodUs,
                    100.0 * p99 / periodUs, 100.0 * mx / periodUs);
        std::printf("RT guard hits: %u, underruns: %llu\n", hits,
                    static_cast<unsigned long long>(engine.underruns()));
    }
    return hits == 0 ? 0 : 1;
}

} // namespace

void addBenchCommand(CLI::App& app, int& exitCode) {
    auto* cmd =
        app.add_subcommand("bench", "Time the engine callback on the NullBackend (as fast as possible)");
    auto o = std::make_shared<BenchOptions>();
    cmd->add_option("--session", o->session,
                    "Session file: the module instances to run (processors and analyzers)");
    cmd->add_option("--module-dir", o->moduleDirs, "Extra folder to scan for module packages (repeatable)");
    cmd->add_option("--rate", o->rate, "Sample rate")->capture_default_str();
    cmd->add_option("--block", o->block, "Block size in frames")->capture_default_str();
    cmd->add_option("--seconds", o->seconds, "Seconds of audio to process")->capture_default_str();
    cmd->add_option("--soak", o->soak, "Seconds of audio for a long soak run (overrides --seconds)");
    cmd->add_flag("--json", o->asJson, "Print JSON");
    cmd->callback([&exitCode, o] {
        if (o->rate == 0 || o->block == 0 || o->seconds <= 0.0) {
            std::fprintf(stderr, "error: rate, block and seconds must be positive\n");
            exitCode = 2;
            return;
        }
        exitCode = runBench(*o);
    });
}

} // namespace dsptb::cli
