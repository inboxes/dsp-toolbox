// devices, gen, analyze, compare
#include "commands.hpp"

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/backend.hpp>
#include <dsptb/core/signal_gen.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>

namespace dsptb::cli {

using nlohmann::json;

void addDevicesCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand("devices", "List audio backends and devices");
    auto asJson = std::make_shared<bool>(false);
    cmd->add_flag("--json", *asJson, "Print JSON");
    cmd->callback([&exitCode, asJson] {
        std::vector<DeviceInfo> devices;
        std::string error;
        const bool ok = enumerateDevices(devices, error);
        // Backends other than WASAPI (a build with ASIO) list their own devices, one entry per driver.
        std::vector<BackendKind> backendKinds = availableBackends();
        std::vector<std::string> backends;
        std::vector<std::vector<DeviceInfo>> extra(backendKinds.size());
        for (size_t i = 0; i < backendKinds.size(); ++i) {
            backends.push_back(backendName(backendKinds[i]));
            if (backendKinds[i] != BackendKind::WasapiShared &&
                backendKinds[i] != BackendKind::WasapiExclusive) {
                std::string ignored;
                enumerateDevices(backendKinds[i], extra[i], ignored);
            }
        }
        if (*asJson) {
            json j;
            j["backends"] = backends;
            for (size_t i = 0; i < backendKinds.size(); ++i)
                if (backendKinds[i] != BackendKind::WasapiShared &&
                    backendKinds[i] != BackendKind::WasapiExclusive) {
                    json list = json::array();
                    for (const auto& d : extra[i])
                        list.push_back({{"id", d.id}, {"name", d.name}});
                    j["devicesByBackend"][backends[i]] = list;
                }
            j["ok"] = ok;
            if (!ok)
                j["error"] = error;
            j["devices"] = json::array();
            for (const auto& d : devices)
                j["devices"].push_back(
                    {{"id", d.id}, {"name", d.name}, {"input", d.isInput}, {"default", d.isDefault}});
            std::cout << j.dump(2) << "\n";
        } else if (!ok) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
        } else {
            std::string joined;
            for (const auto& b : backends)
                joined += (joined.empty() ? "" : ", ") + b;
            std::printf("backends: %s\n", joined.c_str());
            for (bool input : {false, true}) {
                std::printf("%s devices:\n", input ? "input" : "output");
                for (const auto& d : devices)
                    if (d.isInput == input)
                        std::printf("  %s%s\n", d.name.c_str(), d.isDefault ? "  (default)" : "");
            }
            for (size_t i = 0; i < backendKinds.size(); ++i)
                if (backendKinds[i] != BackendKind::WasapiShared &&
                    backendKinds[i] != BackendKind::WasapiExclusive) {
                    std::printf("%s devices:\n", backendDisplayName(backendKinds[i]));
                    if (extra[i].empty())
                        std::printf("  (none installed)\n");
                    for (const auto& d : extra[i])
                        std::printf("  %s\n", d.name.c_str());
                }
        }
        exitCode = ok ? 0 : 1;
    });
}

void addGenCommand(CLI::App& app, int& exitCode) {
    auto* cmd =
        app.add_subcommand("gen", "Generate a test signal: sine|noise|pink|impulse|sweep|clicks|silence");
    auto type = std::make_shared<std::string>();
    auto out = std::make_shared<std::string>();
    auto format = std::make_shared<std::string>("f32");
    auto p = std::make_shared<GenParams>();
    cmd->add_option("type", *type, "Signal type")->required();
    cmd->add_option("--out", *out, "Output WAV file")->required();
    cmd->add_option("--freq", p->freq, "Frequency in Hz (sine; sweep start)")->capture_default_str();
    cmd->add_option("--freq-end", p->freqEnd, "Sweep end frequency in Hz")->capture_default_str();
    cmd->add_option("--dbfs", p->dbfs,
                    "Level in dBFS: peak for sine/impulse/sweep/clicks, RMS for noise/pink")
        ->capture_default_str();
    cmd->add_option("--seconds", p->seconds, "Duration")->capture_default_str();
    cmd->add_option("--rate", p->sampleRate, "Sample rate")->capture_default_str();
    cmd->add_option("--channels", p->channels, "Channel count")->capture_default_str();
    cmd->add_option("--phase-deg", p->phaseDeg, "Sine start phase in degrees")->capture_default_str();
    cmd->add_option("--bpm", p->bpm, "Clicks per minute")->capture_default_str();
    cmd->add_option("--seed", p->seed, "Noise seed")->capture_default_str();
    cmd->add_option("--format", *format, "f32 (default), s24 or s16");
    cmd->callback([&exitCode, type, out, format, p] {
        WavFormat wf;
        if (!parseSignalType(*type, p->type)) {
            std::fprintf(stderr, "error: unknown signal type '%s'\n", type->c_str());
            exitCode = 2;
            return;
        }
        if (!parseWavFormat(*format, wf)) {
            std::fprintf(stderr, "error: --format must be f32, s24 or s16\n");
            exitCode = 2;
            return;
        }
        if (p->sampleRate == 0 || p->channels == 0 || p->seconds < 0.0) {
            std::fprintf(stderr, "error: rate, channels and seconds must be positive\n");
            exitCode = 2;
            return;
        }
        const PcmAudio audio = generateSignal(*p);
        std::string error;
        if (!writeWav(*out, audio, wf, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            exitCode = 1;
            return;
        }
        std::printf("wrote %s (%s, %u ch, %u Hz, %zu frames)\n", out->c_str(), signalTypeName(p->type),
                    audio.channels, audio.sampleRate, audio.frames());
        exitCode = 0;
    });
}

void addAnalyzeCommand(CLI::App& app, int& exitCode) {
    auto* cmd =
        app.add_subcommand("analyze", "Measure a WAV/FLAC/MP3 file, or run an analyzer module over it");
    auto file = std::make_shared<std::string>();
    auto asJson = std::make_shared<bool>(false);
    auto opts = std::make_shared<AnalysisOptions>();
    auto mod = std::make_shared<ModuleAnalyzeArgs>();
    cmd->add_option("file", *file, "Input audio file");
    cmd->add_option("--in", *file, "Input audio file (same as the positional argument)");
    cmd->add_option("--module", mod->moduleId,
                    "Run this analyzer module (e.g. org.dsptb.spectrogram) instead of measuring");
    cmd->add_option("--params", mod->params, "Module parameter as id=value (repeatable)");
    cmd->add_option("--dump-frames", mod->dumpPath, "Write the module's frame dump here (- for stdout)");
    cmd->add_option("--block", mod->block, "Chunk size handed to the module, in frames")
        ->capture_default_str();
    cmd->add_flag("--pre", mod->tapPre, "Tell the module it listens on the PRE tap");
    cmd->add_option("--module-dir", mod->moduleDirs, "Extra folder to scan for module packages (repeatable)");
    cmd->add_flag("--json", *asJson, "Print JSON");
    cmd->add_option("--window-ms", opts->windowMs, "Also report RMS per window of this length (ms)");
    cmd->add_option("--start", opts->startSeconds, "Analyse from this time (s)");
    cmd->add_option("--end", opts->endSeconds, "Analyse up to this time (s)");
    cmd->callback([&exitCode, file, asJson, opts, mod] {
        if (!mod->moduleId.empty()) {
            mod->input = *file;
            runModuleAnalyze(*mod, exitCode);
            return;
        }
        if (file->empty()) {
            std::fprintf(stderr, "error: analyze needs an input file\n");
            exitCode = 2;
            return;
        }
        PcmAudio audio;
        std::string error;
        if (!readPcm(*file, audio, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            exitCode = 2;
            return;
        }
        const AnalysisResult r = analyze(audio, *opts);
        if (*asJson) {
            json j;
            j["file"] = *file;
            j["sampleRate"] = r.sampleRate;
            j["channels"] = r.channels;
            j["frames"] = r.frames;
            j["seconds"] = r.seconds;
            j["peakDbfs"] = r.peakDbfs;
            j["rmsDbfs"] = r.rmsDbfs;
            j["perChannel"] = json::array();
            for (const auto& c : r.perChannel)
                j["perChannel"].push_back({{"peakDbfs", c.peakDbfs},
                                           {"rmsDbfs", c.rmsDbfs},
                                           {"dc", c.dc},
                                           {"onsetSample", c.onsetSample}});
            if (r.windowMs > 0.0) {
                j["windowMs"] = r.windowMs;
                j["windowRmsDbfs"] = r.windowRmsDbfs;
            }
            std::cout << j.dump(2) << "\n";
        } else {
            std::printf("%s: %u ch, %u Hz, %zu frames (%.3f s)\n", file->c_str(), r.channels, r.sampleRate,
                        r.frames, r.seconds);
            std::printf("overall: peak %.2f dBFS, RMS %.2f dBFS\n", r.peakDbfs, r.rmsDbfs);
            for (size_t c = 0; c < r.perChannel.size(); ++c) {
                const auto& s = r.perChannel[c];
                std::printf("ch%zu: peak %.2f dBFS, RMS %.2f dBFS, DC %.3e, onset sample %lld\n", c,
                            s.peakDbfs, s.rmsDbfs, s.dc, static_cast<long long>(s.onsetSample));
            }
            for (size_t w = 0; w < r.windowRmsDbfs.size(); ++w) {
                std::printf("window %zu:", w);
                for (double v : r.windowRmsDbfs[w])
                    std::printf(" %.2f", v);
                std::printf("\n");
            }
        }
        exitCode = 0;
    });
}

void addCompareCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand(
        "compare", "Compare two audio files; exit 1 if they differ by more than the tolerance");
    auto a = std::make_shared<std::string>(), b = std::make_shared<std::string>();
    auto tolerance = std::make_shared<double>(0.0);
    auto asJson = std::make_shared<bool>(false);
    cmd->add_option("a", *a, "First file")->required();
    cmd->add_option("b", *b, "Second file")->required();
    cmd->add_option("--tolerance", *tolerance, "Largest allowed absolute sample difference")
        ->capture_default_str();
    cmd->add_flag("--json", *asJson, "Print JSON");
    cmd->callback([&exitCode, a, b, tolerance, asJson] {
        PcmAudio pa, pb;
        std::string error;
        if (!readPcm(*a, pa, error) || !readPcm(*b, pb, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            exitCode = 2;
            return;
        }
        const CompareResult r = compareAudio(pa, pb);
        const bool pass = r.shapeMatches && r.maxAbsDiff <= *tolerance;
        if (*asJson) {
            json j;
            j["pass"] = pass;
            j["maxAbsDiff"] = r.maxAbsDiff;
            j["firstDiffFrame"] = r.firstDiffFrame;
            j["shapeMatches"] = r.shapeMatches;
            j["notes"] = r.notes;
            std::cout << j.dump(2) << "\n";
        } else {
            for (const auto& n : r.notes)
                std::printf("note: %s\n", n.c_str());
            std::printf("max abs diff %.9g at frame %zu (tolerance %.9g): %s\n", r.maxAbsDiff,
                        r.firstDiffFrame, *tolerance, pass ? "PASS" : "FAIL");
        }
        exitCode = pass ? 0 : 1;
    });
}

} // namespace dsptb::cli
