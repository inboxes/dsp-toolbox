#pragma once

#include <dsptb/core/offline_run.hpp>
#include <dsptb/core/worker_pool.hpp>

#include <memory>
#include <dsptb/core/audio_session.hpp>
#include <dsptb/core/backend.hpp>
#include <dsptb/core/logging.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/notifications.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/settings.hpp>

#include <imgui.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsptb::ui {

class CanvasHost;
class SessionController;

enum Panel { kModuleBrowser, kChain, kTransport, kLog, kPanelCount };
inline constexpr const char* kPanelNames[kPanelCount] = {"Module Browser", "Chain", "Transport", "Log"};

// State of the module panels (UI thread only).
struct ModuleUiState {
    std::map<uint64_t, float> editStart;  // (instance << 16 | param) -> value when a drag began
    std::map<InstanceId, bool> panelOpen; // instance panel visible (absent = open)
    InstanceId focusRequest = 0;          // bring this instance's panel to the front next frame
    InstanceId lastAdded = 0;             // target of --test-action param:<id>:<value>
    InstanceId renameId = 0;              // instance being renamed (0 = none)
    char renameBuf[64] = {};
    bool renameFocus = false;
    float typedValue = 0.0f; // the right-click "type a value" popup
    // Offline modules: one worker, so runs queue up. A finished run is dropped on the UI thread, which is
    // where the module must be destroyed.
    std::map<InstanceId, std::shared_ptr<dsptb::OfflineRun>> offlineRuns;
    std::unique_ptr<dsptb::WorkerPool> offlinePool;
};

// Everything the shell (dockspace, menus, panels, status bar, banners) needs for one frame.
struct ShellContext {
    dsptb::Settings* settings = nullptr;
    dsptb::LogBuffer* log = nullptr;
    dsptb::NotificationCenter* notices = nullptr;
    dsptb::AudioSession* audio = nullptr;
    dsptb::ModuleRegistry* registry = nullptr;
    dsptb::SessionModel* model = nullptr;
    CanvasHost* canvas = nullptr;
    SessionController* session = nullptr;         // sessions, export, recording (session_ui.hpp)
    std::vector<dsptb::ScanFolder> moduleFolders; // scanned at start and on Rescan
    ModuleUiState moduleUi;

    bool panelOpen[kPanelCount] = {true, true, true, true};
    bool defaultLayoutNeeded = true; // no saved layout: build the default one on the next frame
    bool showAbout = false;
    bool quitRequested = false;
    bool openFileRequested = false; // handled by the app after the frame (the dialog is modal)
    std::string openFilePath;       // set by "Recent": open this file without a dialog
    bool showAudioSettings = false;
    float fps = 0.0f;

    // Internal state for the two-step "undock" (queue the undock, then place the floating window).
    int undockPhase = 0;
    int undockPanel = -1;

    // The ASIO Compatible logo, in builds with ASIO. Null if it couldn't be loaded.
    ImTextureID asioLogo{};
    ImVec2 asioLogoSize{};

    // Audio Settings dialog: a working copy of the configuration plus the device lists.
    std::vector<dsptb::DeviceInfo> devices;
    dsptb::BackendKind devicesBackend =
        dsptb::BackendKind::WasapiShared; // which backend `devices` was listed for
    bool devicesLoaded = false;
    dsptb::AudioConfig editConfig;
    bool editConfigInit = false;

    // Transport waveform interaction: 0 none, 1 seeking, 2 dragging the loop start, 3 dragging the loop end.
    int waveformDrag = 0;
    double scrubSeconds = 0.0; // where the playhead is while the mouse is held down on the waveform
    bool resumeAfterScrub = false;

    // Log panel state (cached snapshot refreshed only when the buffer version changes).
    std::vector<dsptb::LogEntry> logCache;
    uint64_t logCacheVersion = ~0ull;
    int logMinLevel = 2; // index into the level combo: Trace, Debug, Info, Warning, Error
    bool logAutoScroll = true;
};

// Call once after the saved layout (if any) has been loaded into ImGui: decides whether the default layout
// must be built.
void initLayout(ShellContext& ctx);

// Requests the default dock layout (View > Reset Layout). All panels are reopened.
void requestResetLayout(ShellContext& ctx);

// Starts the undock of the panel with this name (no-op if the name is unknown).
void requestUndock(ShellContext& ctx, const char* panelName);

// Draws one complete frame of UI. Call between ImGui::NewFrame() and ImGui::Render().
void drawShell(ShellContext& ctx);

// Copies the panel-open flags from/to Settings::panelsOpen.
void loadPanelState(ShellContext& ctx, const dsptb::Settings& s);
void storePanelState(const ShellContext& ctx, dsptb::Settings& s);

} // namespace dsptb::ui
