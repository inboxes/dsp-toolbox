#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace dsptb::app {

struct Options {
    bool smokeTest = false;
    int smokeFrames = 300;
    std::filesystem::path screenshot; // empty = no screenshot
    int screenshotFrames = 120;
    std::filesystem::path session;        // loaded at start
    bool nullAudio = false;               // use no audio device
    int uiScaleOverride = 0;              // 0 = use settings; otherwise 100/125/150/200 (test modes)
    std::vector<std::string> testActions; // test modes only: "reset" or "undock:<Panel>", run at frame 20
    bool useSettings = false;             // test modes: read the user's settings.json (never written)
    bool showHelp = false;
    std::string error; // non-empty when the command line was invalid

    bool testMode() const { return smokeTest || !screenshot.empty(); }
};

Options parseCommandLine();

// Runs the application (or a test mode) to completion and returns the process exit code.
int run(HINSTANCE instance, const Options& options);

} // namespace dsptb::app
