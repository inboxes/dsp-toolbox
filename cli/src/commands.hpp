#pragma once

#include <dsptb/core/backend.hpp>

#include <CLI/CLI.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace dsptb::cli {

// "wasapi-shared, wasapi-exclusive, ..." for the backends this build offers (help and error text).
inline std::string backendList() {
    std::string s;
    for (BackendKind k : availableBackends())
        s += (s.empty() ? "" : ", ") + std::string(backendName(k));
    return s;
}

// Each registers one subcommand. The callback stores the process exit code in `exitCode`.
void addDevicesCommand(CLI::App& app, int& exitCode);
void addGenCommand(CLI::App& app, int& exitCode);
void addAnalyzeCommand(CLI::App& app, int& exitCode);
void addCompareCommand(CLI::App& app, int& exitCode);
void addBenchCommand(CLI::App& app, int& exitCode);
void addProbeCommand(CLI::App& app, int& exitCode);
void addLatencyCommand(CLI::App& app, int& exitCode);
void addValidateCommand(CLI::App& app, int& exitCode);
void addRenderCommand(CLI::App& app, int& exitCode);

// `analyze --module`: runs an analyzer module offline over a file and writes its frame dump.
struct ModuleAnalyzeArgs {
    std::string moduleId, input, dumpPath;
    std::vector<std::string> params; // "id=value"
    std::vector<std::string> moduleDirs;
    int block = 1024;
    bool tapPre = false;
    uint32_t sampleRate = 0; // 0 = the file's rate
};
void runModuleAnalyze(const ModuleAnalyzeArgs& args, int& exitCode);

} // namespace dsptb::cli
