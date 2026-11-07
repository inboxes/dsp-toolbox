// validate, render
#include "commands.hpp"

#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/module_validator.hpp>
#include <dsptb/core/offline_analyze.hpp>
#include <dsptb/core/offline_render.hpp>
#include <dsptb/core/paths.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dsptb::cli {

namespace fs = std::filesystem;

void addValidateCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand("validate", "Run the module conformance suite on a package folder or DLL");
    auto target = std::make_shared<std::string>();
    auto asJson = std::make_shared<bool>(false);
    cmd->add_option("package", *target, "Package folder (with module.json) or the module DLL")->required();
    cmd->add_flag("--json", *asJson, "Print JSON");
    cmd->callback([&exitCode, target, asJson] {
        std::error_code ec;
        if (!fs::exists(fs::path(*target), ec)) {
            std::fprintf(stderr, "error: %s does not exist\n", target->c_str());
            exitCode = 2;
            return;
        }
        ModuleRegistry registry;
        const ValidationReport rep = validateModulePackage(registry, fs::path(*target));
        if (*asJson)
            std::cout << rep.toJson() << "\n";
        else
            std::cout << rep.toText();
        exitCode = rep.passed() ? 0 : 1;
    });
}

void addRenderCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand("render", "Render a WAV through a session's processors, offline");
    auto session = std::make_shared<std::string>(), in = std::make_shared<std::string>(),
         out = std::make_shared<std::string>(), format = std::make_shared<std::string>("f32");
    auto block = std::make_shared<int>(512);
    auto tail = std::make_shared<double>(2.0);
    auto moduleDirs = std::make_shared<std::vector<std::string>>();
    cmd->add_option("--session", *session, "Session JSON (dsptb-session) listing the instances")->required();
    cmd->add_option("--in", *in, "Input audio file (default: the audio file named in the session)");
    cmd->add_option("--out", *out, "Output WAV")->required();
    cmd->add_option("--block", *block, "Block size in frames")->capture_default_str();
    cmd->add_option("--tail", *tail, "Seconds appended after the input")->capture_default_str();
    auto workingFile = std::make_shared<std::string>();
    cmd->add_option("--working-file", *workingFile,
                    "Audio file modules see as the working file, such as a backing track (default: the input)");
    cmd->add_option("--format", *format, "f32, s24 or s16")->capture_default_str();
    cmd->add_option(
        "--module-dir", *moduleDirs,
        "Extra folder to scan for module packages (repeatable); the app's own folders are always scanned");
    cmd->callback([&exitCode, session, in, out, format, block, tail, moduleDirs, workingFile] {
        const auto fail = [&](const std::string& msg, int code = 2) {
            std::fprintf(stderr, "error: %s\n", msg.c_str());
            exitCode = code;
        };
        std::ifstream sf(*session, std::ios::binary);
        if (!sf)
            return fail("cannot read " + *session);
        std::stringstream ss;
        ss << sf.rdbuf();
        SessionDoc doc;
        std::string error;
        if (!sessionFromJson(ss.str(), doc, error))
            return fail(error);
        const SessionDescription desc = descriptionFromSession(doc);
        // Without --in the session's own audio file is used (absolute path, else next to the session).
        fs::path inputPath = *in; // argv is in the code page, as before
        if (inputPath.empty()) {
            const fs::path found = resolveSessionAudio(fs::path(*session), doc.source);
            if (found.empty())
                return fail("the session names no audio file that exists; pass --in");
            inputPath = found; // keep it a path: string() depends on the code page
        }
        WavFormat wavFormat;
        if (!parseWavFormat(*format, wavFormat))
            return fail("unknown format " + *format);

        // The session's rate decides the working rate; the input is resampled to it (same code as
        // export).
        AudioBuffer input;
        if (!decodeToStereo(inputPath, desc.sampleRate, input, error))
            return fail(error);

        ModuleRegistry registry;
        std::vector<fs::path> folders{modulesDir()};
        for (const auto& d : *moduleDirs)
            folders.push_back(fs::path(d));
        registry.scan(folders);

        RenderOptions opt;
        opt.blockFrames = static_cast<uint32_t>(std::max(32, *block));
        opt.tailSeconds = *tail;
        if (!workingFile->empty()) {
            auto backing = std::make_shared<AudioBuffer>();
            if (!decodeToStereo(fs::path(*workingFile), input.sampleRate, *backing, error))
                return fail(*workingFile + ": " + error);
            opt.workingFile = backing;
        }
        AudioBuffer rendered;
        if (!renderOffline(registry, desc, input, opt, rendered, error))
            return fail(error, 1);

        PcmAudio pcm;
        pcm.channels = 2;
        pcm.sampleRate = rendered.sampleRate;
        pcm.samples.resize(rendered.frames() * 2);
        for (size_t i = 0; i < rendered.frames(); ++i) {
            pcm.samples[2 * i] = rendered.ch[0][i];
            pcm.samples[2 * i + 1] = rendered.ch[1][i];
        }
        if (!writeWav(*out, pcm, wavFormat, error))
            return fail(error);
        std::printf("rendered %zu frames at %u Hz to %s\n", rendered.frames(), rendered.sampleRate,
                    out->c_str());
        exitCode = 0;
    });
}

void runModuleAnalyze(const ModuleAnalyzeArgs& args, int& exitCode) {
    const auto fail = [&](const std::string& msg, int code = 2) {
        std::fprintf(stderr, "error: %s\n", msg.c_str());
        exitCode = code;
    };
    if (args.input.empty())
        return fail("--module needs an input file (--in <file>)");
    AnalyzeModuleOptions opt;
    opt.moduleId = args.moduleId;
    opt.blockFrames = static_cast<uint32_t>(std::max(32, args.block));
    opt.tap = args.tapPre ? DSPTB_TAP_PRE : DSPTB_TAP_POST;
    for (const std::string& kv : args.params) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0)
            return fail("--params expects id=value, got \"" + kv + "\"");
        try {
            opt.params.emplace_back(kv.substr(0, eq), std::stof(kv.substr(eq + 1)));
        } catch (...) {
            return fail("bad number in --params \"" + kv + "\"");
        }
    }

    // A file's own rate is kept: the analyzer sees what a tap would deliver at that rate.
    PcmAudio pcm;
    std::string error;
    if (!readPcm(args.input, pcm, error))
        return fail(error);
    AudioBuffer input;
    if (!decodeToStereo(args.input, args.sampleRate ? args.sampleRate : pcm.sampleRate, input, error))
        return fail(error);

    ModuleRegistry registry;
    std::vector<fs::path> folders{modulesDir()};
    for (const auto& d : args.moduleDirs)
        folders.push_back(fs::path(d));
    registry.scan(folders);

    std::string dump;
    if (!analyzeWithModule(registry, opt, input, dump, error))
        return fail(error, 1);
    if (args.dumpPath.empty() || args.dumpPath == "-") {
        std::cout << dump << "\n";
    } else {
        std::ofstream out(args.dumpPath, std::ios::binary);
        if (!out)
            return fail("cannot write " + args.dumpPath);
        out << dump;
        std::printf("wrote %s (%zu bytes)\n", args.dumpPath.c_str(), dump.size());
    }
    exitCode = 0;
}

} // namespace dsptb::cli
