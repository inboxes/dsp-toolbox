// dsptb-cli: test signals, analysis, offline render and validation. Exit code 0 = success, 1 = a check failed, 2 = bad
// input.
#include "commands.hpp"

#include <CLI/CLI.hpp>

#include <dsptb/branding.h>
#include <dsptb/core/version.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// Replaces global operator new/delete when DSPTB_RT_GUARD is on, so `bench` can report guard hits.
DSPTB_DEFINE_RT_GUARD();

int main(int argc, char** argv) {
    // Stdout belongs to the results: a log line there would break --json and --dump-frames -.
    spdlog::set_default_logger(spdlog::stderr_color_mt("cli"));

    CLI::App app{std::string(DSPTB_APP_NAME) + " command-line tool"};
    app.set_version_flag("--version", dsptb::versionString());
    app.require_subcommand(1);
    int exitCode = 0;

    dsptb::cli::addDevicesCommand(app, exitCode);
    dsptb::cli::addGenCommand(app, exitCode);
    dsptb::cli::addAnalyzeCommand(app, exitCode);
    dsptb::cli::addCompareCommand(app, exitCode);
    dsptb::cli::addBenchCommand(app, exitCode);
    dsptb::cli::addProbeCommand(app, exitCode);
    dsptb::cli::addLatencyCommand(app, exitCode);
    dsptb::cli::addValidateCommand(app, exitCode);
    dsptb::cli::addRenderCommand(app, exitCode);

    CLI11_PARSE(app, argc, argv);
    return exitCode;
}
