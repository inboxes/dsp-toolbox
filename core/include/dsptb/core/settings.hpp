#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace dsptb {

// User settings persisted to <dataDir>/settings.json (schema version 1). UI-agnostic plain data.
struct Settings {
    struct Window {
        int x = 100;
        int y = 100;
        int w = 1280;
        int h = 800;
        bool maximized = false;
        bool valid = false; // false until a position has been saved at least once
    };

    // Last audio configuration. Device ids are opaque strings from the backend; empty = default.
    struct Audio {
        std::string backend = "wasapi-shared";
        std::string inputDeviceId;
        std::string outputDeviceId;
        int sampleRate = 48000;
        int bufferFrames = 256;
    };

    // GPU runtime (docs/GPU_RUNTIME.md section 6.3). Changes apply after a restart.
    struct Gpu {
        std::string runtimeDir;         // UTF-8; empty = <exe dir>/gpu-runtime
        int device = 0;                 // CUDA device index
        std::string sync = "blocking";  // "blocking" (sleeps while waiting for the GPU) or "spin"
    };

    Window window;
    Audio audio;
    Gpu gpu;
    bool micMonitorWarningShown = false; // the one-time "use headphones" warning
    bool micGateEnabled = false;         // microphone noise gate
    float micGateThresholdDb = -50.0f;
    int uiScalePercent = 100; // 100, 125, 150 or 200
    std::vector<std::string> recentSessions;
    std::vector<std::string> recentAudioFiles;
    std::string recordFolder;               // where recordings go (UTF-8); empty = <data dir>/recordings
    std::string recordTap = "post";         // "pre" (input) or "post" (output)
    std::string layoutIni;                  // ImGui dock layout (SaveIniSettingsToMemory string)
    std::map<std::string, bool> panelsOpen; // panel name -> visible; missing means "open"

    static constexpr int kMaxRecent = 10;
    static constexpr int kVersion = 1;

    // Moves `path` to the front of a recent list, removing duplicates and trimming to kMaxRecent.
    static void pushRecent(std::vector<std::string>& list, const std::string& path);

    std::string toJsonString() const;
    // Returns false (and leaves *this at defaults) if the text is not valid settings JSON.
    bool fromJsonString(const std::string& text);

    // Atomic write: <path>.tmp then MoveFileExW replace. Returns false on I/O failure.
    bool save(const std::filesystem::path& path) const;
    enum class LoadResult { Loaded, Missing, Corrupt };
    LoadResult load(const std::filesystem::path& path);
};

// Snaps a requested UI scale to one of the supported steps (100/125/150/200).
int snapUiScalePercent(int percent);

} // namespace dsptb
