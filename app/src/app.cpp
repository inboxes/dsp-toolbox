#include "app.hpp"

#include "d3d11_context.hpp"
#include "png_reader.hpp"
#include "png_writer.hpp"
#include "ui/canvas_impl.hpp"
#include "ui/file_dialog.hpp"
#include "ui/module_ui.hpp"
#include "ui/session_ui.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"

#include <dsptb/branding.h>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/audio_session.hpp>
#include <dsptb/core/logging.hpp>
#include <dsptb/core/module_install.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/notifications.hpp>
#include <dsptb/core/paths.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/settings.hpp>
#include <dsptb/core/signal_gen.hpp>
#include <dsptb/core/version.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <shellapi.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace dsptb::app {

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr wchar_t kWindowClass[] = L"dsptb_main_window";

std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// State the window procedure needs. Only touched on the UI thread.
struct WindowState {
    D3D11Context* d3d = nullptr;
    bool testMode = false;
    bool focused = true;
    float dpiScale = 1.0f;
    Settings::Window placement; // captured on WM_CLOSE, while the window still exists
    bool placementCaptured = false;
    // The close button does not close right away: unsaved changes are asked about first.
    bool closeRequested = false;
    bool allowClose = false;
};
WindowState* gWindow = nullptr;

void capturePlacement(HWND hwnd) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!gWindow || !GetWindowPlacement(hwnd, &wp))
        return;
    gWindow->placement.x = wp.rcNormalPosition.left;
    gWindow->placement.y = wp.rcNormalPosition.top;
    gWindow->placement.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    gWindow->placement.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    gWindow->placement.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    gWindow->placement.valid = true;
    gWindow->placementCaptured = true;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;
    switch (msg) {
    case WM_SIZE:
        if (gWindow && gWindow->d3d && wParam != SIZE_MINIMIZED)
            gWindow->d3d->resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DPICHANGED:
        if (gWindow) {
            gWindow->dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            if (!gWindow->testMode) {
                const RECT* r = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (gWindow)
            gWindow->focused = wParam != FALSE;
        spdlog::debug("app {}", wParam != FALSE ? "activated" : "deactivated");
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0; // no Alt menu; ImGui owns the menu bar
        break;
    case WM_CLOSE:
        if (gWindow && !gWindow->allowClose && !gWindow->testMode) {
            gWindow->closeRequested = true; // the main loop asks about unsaved changes, then closes for real
            return 0;
        }
        capturePlacement(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Keeps a restored window on a visible monitor (the saved monitor may have been unplugged).
bool placementIsVisible(const Settings::Window& w) {
    const RECT r{w.x, w.y, w.x + w.w, w.y + w.h};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr && w.w >= 480 && w.h >= 320;
}

void attachParentConsole() {
    // If stdout/stderr were redirected to a file or pipe, keep them: the CRT already picked them up.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE)
        return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

void printUsage() {
    std::printf(
        "%s %s\n\n"
        "Usage: %s.exe [options]\n"
        "  --smoke-test [frames]       run N frames (default 300) and exit 0 if there were no errors\n"
        "  --screenshot <png>          render frames, write the backbuffer to a PNG and exit\n"
        "    --frames N                frames to render before capturing (default 120)\n"
        "    --session <file>          session to load first\n"
        "  --null-audio                force the null audio backend (no audio backend yet; accepted)\n"
        "  --ui-scale <100|125|150|200>  override the UI scale (test modes)\n"
        "  --test-action reset|undock:<Panel>|notice:<text>|tone|play|loop:A:B|seek:S|source:mic  test "
        "modes: trigger a UI action at frame 20 "
        "(repeatable)\n"
        "  --use-settings              test modes: read (never write) the user's settings.json\n"
        "  --help\n",
        DSPTB_APP_NAME, dsptb::versionString(), DSPTB_EXE_NAME);
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// A 6 s stereo test file for screenshots and the smoke test: a rising sweep under a slow amplitude envelope,
// so the waveform overview has visible shape.
std::filesystem::path writeToneFile() {
    dsptb::GenParams g;
    g.type = dsptb::SignalType::Sweep;
    g.freq = 110.0;
    g.freqEnd = 3520.0;
    g.dbfs = -8.0;
    g.seconds = 6.0;
    dsptb::PcmAudio tone = dsptb::generateSignal(g);
    for (size_t i = 0; i < tone.frames(); ++i) {
        const double t = static_cast<double>(i) / tone.sampleRate;
        const double env = 0.15 + 0.85 * std::pow(std::sin(3.14159265358979 * 0.5 * t * 1.3), 2.0);
        for (uint32_t c = 0; c < tone.channels; ++c)
            tone.samples[i * tone.channels + c] *= static_cast<float>(env);
    }
    const auto file = std::filesystem::temp_directory_path() / "dsptb_test_tone.wav";
    std::string err;
    if (!dsptb::writeWav(file, tone, dsptb::WavFormat::F32, err))
        spdlog::error("Could not write test tone: {}", err);
    return file;
}

bool parseInt(const wchar_t* text, int& out) {
    wchar_t* end = nullptr;
    const long v = std::wcstol(text, &end, 10);
    if (end == text || *end != L'\0')
        return false;
    out = static_cast<int>(v);
    return true;
}

} // namespace

Options parseCommandLine() {
    Options o;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return o;
    for (int i = 1; i < argc && o.error.empty(); ++i) {
        const std::wstring a = argv[i];
        auto needValue = [&](const char* name) -> const wchar_t* {
            if (i + 1 >= argc) {
                o.error = std::string(name) + " needs a value";
                return nullptr;
            }
            return argv[++i];
        };
        auto needInt = [&](const char* name, int& out) {
            if (const wchar_t* v = needValue(name))
                if (!parseInt(v, out) || out <= 0)
                    o.error = std::string(name) + " needs a positive number";
        };
        if (a == L"--smoke-test") {
            o.smokeTest = true;
            if (i + 1 < argc) { // optional frame count
                int n = 0;
                if (parseInt(argv[i + 1], n) && n > 0) {
                    o.smokeFrames = n;
                    ++i;
                }
            }
        } else if (a == L"--screenshot") {
            if (const wchar_t* v = needValue("--screenshot"))
                o.screenshot = v;
        } else if (a == L"--frames") {
            needInt("--frames", o.screenshotFrames);
        } else if (a == L"--session") {
            if (const wchar_t* v = needValue("--session"))
                o.session = v;
        } else if (a == L"--null-audio") {
            o.nullAudio = true;
        } else if (a == L"--ui-scale") {
            needInt("--ui-scale", o.uiScaleOverride);
        } else if (a == L"--test-action") {
            if (const wchar_t* v = needValue("--test-action"))
                o.testActions.push_back(narrow(v));
        } else if (a == L"--use-settings") {
            o.useSettings = true;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            o.showHelp = true;
        } else {
            o.error = "unknown option: " + narrow(a);
        }
    }
    LocalFree(argv);
    return o;
}

int run(HINSTANCE instance, const Options& opt) {
    const bool testMode = opt.testMode();
    if (testMode || opt.showHelp || !opt.error.empty())
        attachParentConsole();
    if (opt.showHelp) {
        printUsage();
        return 0;
    }
    if (!opt.error.empty()) {
        std::fprintf(stderr, "error: %s\n\n", opt.error.c_str());
        printUsage();
        return 2;
    }

    auto logBuffer = dsptb::initLogging(dsptb::logDir());
    spdlog::info("{} {} starting{}", DSPTB_APP_NAME, dsptb::versionString(), testMode ? " (test mode)" : "");

    dsptb::NotificationCenter notices;

    // Test modes use defaults so results never depend on the user's profile (unless --use-settings).
    dsptb::Settings settings;
    const auto settingsPath = dsptb::dataDir() / "settings.json";
    if (!testMode || opt.useSettings) {
        switch (settings.load(settingsPath)) {
        case dsptb::Settings::LoadResult::Loaded:
            spdlog::info("Loaded settings from {}", settingsPath.string());
            break;
        case dsptb::Settings::LoadResult::Missing:
            spdlog::info("No settings file yet; using defaults");
            break;
        case dsptb::Settings::LoadResult::Corrupt:
            spdlog::warn("settings.json could not be read; using defaults");
            notices.push(dsptb::NoticeLevel::Warning, "Settings file was unreadable, so defaults were used.",
                         0.0);
            break;
        }
    }
    if (opt.uiScaleOverride > 0)
        settings.uiScalePercent = dsptb::snapUiScalePercent(opt.uiScaleOverride);
    if (opt.nullAudio)
        spdlog::info("--null-audio: running without an audio device");

    // ---- window ----
    WindowState state;
    state.testMode = testMode;
    gWindow = &state;

    const std::wstring title = widen(DSPTB_APP_NAME);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));        // app/app.rc
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT rect{100, 100, 100 + kDefaultWidth, 100 + kDefaultHeight};
    bool maximize = false;
    if (testMode) {
        // Deterministic client size for screenshots.
        rect = {0, 0, kDefaultWidth, kDefaultHeight};
        AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    } else if (settings.window.valid && placementIsVisible(settings.window)) {
        rect = {settings.window.x, settings.window.y, settings.window.x + settings.window.w,
                settings.window.y + settings.window.h};
        maximize = settings.window.maximized;
    }
    HWND hwnd =
        CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, rect.left, rect.top,
                        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        spdlog::error("CreateWindowExW failed ({})", GetLastError());
        return 1;
    }

    D3D11Context d3d;
    state.d3d = &d3d;
    state.dpiScale = testMode ? 1.0f : ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (!d3d.create(hwnd)) {
        spdlog::error("Could not create a Direct3D 11 device");
        if (!testMode)
            MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", title.c_str(), MB_ICONERROR);
        DestroyWindow(hwnd);
        return 1;
    }
    spdlog::info("Direct3D 11 device created ({})", d3d.usingWarp() ? "WARP software renderer" : "hardware");

    // Screenshots render off-screen (hidden); the smoke test shows the window without stealing focus.
    if (opt.screenshot.empty())
        ShowWindow(hwnd, testMode ? SW_SHOWNOACTIVATE : (maximize ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT));
    UpdateWindow(hwnd);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true; // viewports stay off
    io.IniFilename = nullptr;                    // the layout lives in settings.json
    io.LogFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d3d.device(), d3d.context());
    const bool segoe = ui::loadFonts();
    spdlog::info("UI font: {}", segoe ? "Segoe UI" : "ImGui default");

    float appliedScale = 0.0f;
    auto currentScale = [&] { return state.dpiScale * static_cast<float>(settings.uiScalePercent) / 100.0f; };
    ui::applyTheme(currentScale());
    appliedScale = currentScale();

    ui::CanvasHost canvasHost(d3d.device(), d3d.context());
    ui::ShellContext shell;
    shell.canvas = &canvasHost;
    shell.settings = &settings;
    shell.log = logBuffer.get();
    shell.notices = &notices;
    ui::loadPanelState(shell, settings);
    if (!settings.layoutIni.empty())
        ImGui::LoadIniSettingsFromMemory(settings.layoutIni.c_str(), settings.layoutIni.size());
    ui::initLayout(shell);

    // ---- audio ----
    // Test modes always use the NullBackend (no device, no sound), as does --null-audio.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // the file dialog and the logo loader need COM
#ifdef DSPTB_HAS_ASIO
    // Outlives the frame loop: the shell draws from it. A missing file only means no logo.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> asioLogo;
    UINT asioLogoW = 0, asioLogoH = 0;
    if (loadPngTexture(d3d.device(), exeDir() / "assets" / "asio-compatible.png", asioLogo, asioLogoW,
                       asioLogoH)) {
        shell.asioLogo = reinterpret_cast<ImTextureID>(asioLogo.Get());
        shell.asioLogoSize = ImVec2(static_cast<float>(asioLogoW), static_cast<float>(asioLogoH));
    } else {
        spdlog::warn("Could not load the ASIO logo");
    }
#endif
    auto audio = std::make_unique<dsptb::AudioSession>();
    shell.audio = audio.get();

    // ---- modules: bundled, then user-installed. DLLs load lazily, on first use. ----
    dsptb::ModuleRegistry registry;
    shell.moduleFolders = {{exeDir() / "modules", false}, {dsptb::dataDir() / "modules", true}};
    dsptb::applyPendingModuleInstalls(dsptb::dataDir() /
                                      "modules"); // before the scan, while no DLL is loaded
    registry.scan(shell.moduleFolders);
    spdlog::info("Found {} module package(s)", registry.packages().size());
    // Created before the stream opens so its instances are prepared for whatever format the device gives us.
    auto model = std::make_unique<dsptb::SessionModel>(audio->engine(), registry);
    shell.registry = &registry;
    shell.model = model.get();
    auto session = std::make_unique<ui::SessionController>(shell, !testMode);
    shell.session = session.get();
    {
        dsptb::AudioConfig cfg;
        if (testMode || opt.nullAudio) {
            cfg.backend = dsptb::BackendKind::Null;
        } else {
            // A backend name this build does not know (e.g. "asio" in a build without ASIO) means the saved
            // device ids belong to it too: use the default backend with the default devices.
            const bool known = dsptb::parseBackendKind(settings.audio.backend, cfg.backend);
            if (cfg.backend == dsptb::BackendKind::Null)
                cfg.backend = dsptb::BackendKind::WasapiShared;
            if (known) {
                cfg.inputDeviceId = settings.audio.inputDeviceId;
                cfg.outputDeviceId = settings.audio.outputDeviceId;
            }
            const uint32_t rates[] = {44100, 48000, 88200, 96000};
            const uint32_t buffers[] = {64, 128, 256, 512, 1024, 2048};
            for (uint32_t r : rates)
                if (static_cast<int>(r) == settings.audio.sampleRate)
                    cfg.sampleRate = r;
            for (uint32_t b : buffers)
                if (static_cast<int>(b) == settings.audio.bufferFrames)
                    cfg.bufferFrames = b;
        }
        std::string err;
        if (!audio->applyConfig(cfg, err)) {
            spdlog::error("Audio could not be started: {}", err);
            notices.push(dsptb::NoticeLevel::Error,
                         "Audio could not be started: " + err + " Retrying every 2 seconds.", 0.0);
        }
    }
    audio->engine().setMicGate(settings.micGateEnabled, settings.micGateThresholdDb);
    if (!opt.session.empty())
        session->loadFrom(opt.session);
    std::string shownTitle;
    size_t nextTestAction = 0;
    bool waitingForTestFile = false;

    const bool persist = !testMode;
    auto saveSettings = [&](bool withWindow) {
        if (!persist)
            return;
        if (withWindow && state.placementCaptured)
            settings.window = state.placement;
        settings.layoutIni = ImGui::SaveIniSettingsToMemory();
        ui::storePanelState(shell, settings);
        if (!settings.save(settingsPath))
            spdlog::warn("Could not write {}", settingsPath.string());
    };

    // ---- main loop ----
    const int totalFrames = !opt.screenshot.empty() ? opt.screenshotFrames : opt.smokeFrames;
    int frame = 0;
    int exitCode = 0;
    bool running = true;
    bool closeSent = false;
    const auto loopStart = std::chrono::steady_clock::now();
    while (running) {
        const bool minimized = IsIconic(hwnd) != FALSE;
        // Anything moving on screen (playhead, meters, banners, retry status) keeps the UI at full rate.
        const bool animating = audio->engine().playing() || audio->loading() ||
                               audio->source() == dsptb::SourceKind::Mic || audio->deviceLost() ||
                               !notices.empty();
        // Idle: 10 fps when minimized or unfocused and nothing is animating. Test modes never throttle.
        if (!testMode && (minimized || !state.focused) && !animating)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;
        if (minimized)
            continue;

        if (currentScale() != appliedScale) {
            ui::applyTheme(currentScale());
            appliedScale = currentScale();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        {
            std::vector<dsptb::AudioMessage> audioMessages;
            audio->tick(ImGui::GetTime(), audioMessages);
            for (const auto& m : audioMessages)
                notices.push(m.level, m.text, ImGui::GetTime());
        }

        {
            std::vector<dsptb::SessionMessage> sessionMessages;
            model->tick(ImGui::GetTime(), sessionMessages);
            for (const auto& m : sessionMessages)
                notices.push(m.level, m.text, ImGui::GetTime());
        }
        session->tick(ImGui::GetTime());
        if (!testMode) {
            const std::string wanted = session->windowTitle();
            if (wanted != shownTitle) {
                shownTitle = wanted;
                SetWindowTextW(hwnd, widen(wanted.c_str()).c_str());
            }
        }

        // Smoke test: add one instance of every installed module, run, then remove them
        // again.
        if (opt.smokeTest && frame == 8) {
            for (const auto& pkg : registry.packages())
                for (const auto& mm : pkg->manifest.modules)
                    ui::addModuleInstance(shell, mm.id);
            spdlog::info("smoke test: {} module instance(s) added", model->instances().size());
        }
        if (opt.smokeTest && frame == totalFrames - 60) {
            std::vector<dsptb::InstanceId> ids;
            for (const auto& r : model->instances())
                ids.push_back(r.id);
            for (auto id : ids)
                model->remove(id);
        }
        if (opt.smokeTest && frame == 3)
            notices.push(dsptb::NoticeLevel::Info, "Smoke test banner", ImGui::GetTime());
        if (opt.smokeTest && frame == 5) { // exercise load + playback + loop through the real paths
            audio->openFile(writeToneFile());
            waitingForTestFile = true;
        }
        if (opt.smokeTest && waitingForTestFile && audio->file()) {
            waitingForTestFile = false;
            audio->setLoopSeconds(true, 1.0, 3.0);
            audio->engine().play();
        }
        // Scripted UI actions for screenshots and tests: one per frame, waiting for a file load to finish.
        if (testMode && frame >= 20 && nextTestAction < opt.testActions.size() && !audio->loading() &&
            !(waitingForTestFile && !audio->file())) {
            waitingForTestFile = false;
            const std::string& action = opt.testActions[nextTestAction++];
            if (action == "reset") {
                ui::requestResetLayout(shell);
            } else if (action.rfind("notice:", 0) == 0) {
                notices.push(dsptb::NoticeLevel::Warning, action.substr(7), ImGui::GetTime());
                spdlog::error("test error line (from --test-action notice)");
            } else if (action.rfind("undock:", 0) == 0) {
                ui::requestUndock(shell, action.c_str() + 7);
            } else if (action == "tone") {
                audio->openFile(writeToneFile());
                waitingForTestFile = true;
            } else if (action.rfind("open:", 0) == 0) {
                audio->openFile(std::filesystem::path(action.substr(5)));
                waitingForTestFile = true;
            } else if (action == "play") {
                audio->engine().play();
            } else if (action.rfind("loop:", 0) == 0) {
                double a = 0.0, b = 0.0;
                if (std::sscanf(action.c_str() + 5, "%lf:%lf", &a, &b) == 2)
                    audio->setLoopSeconds(true, a, b);
            } else if (action.rfind("seek:", 0) == 0) {
                audio->seekSeconds(std::atof(action.c_str() + 5));
            } else if (action == "source:mic") {
                std::string err;
                audio->setSource(dsptb::SourceKind::Mic, err);
            } else if (action == "gate") {
                audio->engine().setMicGate(true, -50.0f);
            } else if (action == "audio-settings") {
                shell.showAudioSettings = true;
            } else if (action == "about") {
                shell.showAbout = true;
            } else if (action == "install-dialog") {
                ui::requestModuleInstall();
            } else if (action.rfind("add:", 0) == 0) {
                ui::addModuleInstance(shell, action.substr(4));
            } else if (action.rfind("save:", 0) == 0) {
                session->saveTo(fromUtf8(action.substr(5)));
            } else if (action.rfind("load:", 0) == 0) {
                session->loadFrom(fromUtf8(action.substr(5)));
            } else if (action == "record:post" || action == "record:pre") {
                session->startRecording(action == "record:pre" ? DSPTB_TAP_PRE : DSPTB_TAP_POST);
            } else if (action == "record-stop") {
                session->stopRecording();
            } else if (action == "export-dialog") {
                session->requestExport();
            } else if (action.rfind("state:", 0) == 0) {
                // state:<json> loads module state into the most recently added instance (as a session would).
                if (auto* rec = model->find(shell.moduleUi.lastAdded)) {
                    rec->instance->loadState(action.substr(6));
                    rec->instance->saveState(rec->lastState);
                }
            } else if (action == "tap:pre" || action == "tap:post") {
                // Moves the most recently added analyzer to the PRE or POST tap.
                model->setTap(shell.moduleUi.lastAdded, action == "tap:pre" ? DSPTB_TAP_PRE : DSPTB_TAP_POST);
            } else if (action == "run") {
                // Starts the most recently added offline module on the loaded file.
                if (!ui::startOfflineRun(shell, shell.moduleUi.lastAdded))
                    spdlog::warn("--test-action run: nothing to run");
            } else if (action.rfind("label:", 0) == 0) {
                // label:<text> names the most recently added instance.
                model->setLabel(shell.moduleUi.lastAdded, action.substr(6));
            } else if (action.rfind("param:", 0) == 0) {
                // param:<paramId>:<value> acts on the most recently added instance.
                const std::string rest = action.substr(6);
                const size_t colon = rest.find(':');
                if (colon != std::string::npos) {
                    if (auto* rec = model->find(shell.moduleUi.lastAdded)) {
                        const int idx = rec->instance->params().indexOf(rest.substr(0, colon));
                        if (idx >= 0)
                            model->setParam(rec->id, static_cast<uint32_t>(idx),
                                            static_cast<float>(std::atof(rest.c_str() + colon + 1)));
                    }
                }
            } else {
                spdlog::warn("unknown --test-action '{}'", action);
            }
        }
        shell.fps = io.Framerate;
        ui::drawShell(shell);
        if (shell.quitRequested) {
            shell.quitRequested = false;
            PostMessageW(hwnd, WM_CLOSE, 0, 0); // goes through the unsaved-changes check in wndProc
        }
        if (state.closeRequested) {
            state.closeRequested = false;
            session->requestExit();
        }
        if (session->exitConfirmed() && !closeSent) {
            closeSent = true;
            state.allowClose = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (io.WantSaveIniSettings) {
            io.WantSaveIniSettings = false;
            saveSettings(false);
        }

        ImGui::Render();
        const float clear[4] = {0.075f, 0.082f, 0.094f, 1.0f};
        d3d.beginFrame(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        ++frame;
        if (!opt.screenshot.empty() && frame >= totalFrames) {
            std::vector<uint8_t> pixels;
            UINT w = 0, h = 0;
            if (d3d.readBackbuffer(pixels, w, h) && writePng(opt.screenshot, pixels, w, h)) {
                spdlog::info("Wrote screenshot {} ({}x{}, ui scale {}%)", opt.screenshot.string(), w, h,
                             settings.uiScalePercent);
                std::printf("screenshot: %s (%ux%u)\n", opt.screenshot.string().c_str(), w, h);
            } else {
                spdlog::error("Could not write screenshot {}", opt.screenshot.string());
                std::fprintf(stderr, "error: could not write screenshot\n");
                exitCode = 1;
            }
            break;
        }
        d3d.present(/*vsync=*/!testMode);

        // Opening a file runs a modal system dialog, so it happens here, between frames.
        if (!testMode)
            session->processPending(hwnd);
        if (!testMode)
            dsptb::ui::processModuleInstall(shell, hwnd);
        if (!testMode && (shell.openFileRequested || !shell.openFilePath.empty())) {
            std::filesystem::path chosen;
            bool ok = false;
            if (!shell.openFilePath.empty()) {
                chosen = fromUtf8(shell.openFilePath);
                ok = true;
            } else {
                ok = ui::pickAudioFile(hwnd, chosen);
            }
            shell.openFileRequested = false;
            shell.openFilePath.clear();
            if (ok) {
                audio->openFile(chosen);
                dsptb::Settings::pushRecent(settings.recentAudioFiles, toUtf8(chosen));
            }
        }
        if (opt.smokeTest && frame >= totalFrames)
            break;
    }

    saveSettings(true);

    const uint64_t engineCallbacks = audio->engine().callbacks();
    const uint64_t engineUnderruns = audio->engine().underruns();
    ui::stopOfflineRuns(shell);
    shell.session = nullptr;
    session.reset(); // stops a running recording; needs the model
    shell.model = nullptr;
    model.reset(); // before the audio session: it points at the engine
    audio.reset(); // stops the audio stream before anything it logs to goes away
    CoUninitialize();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    d3d.destroy();
    if (IsWindow(hwnd))
        DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, instance);
    gWindow = nullptr;

    if (opt.smokeTest && exitCode == 0) {
        // "No faults": any error-level log line during the run fails the smoke test.
        std::vector<dsptb::LogEntry> entries;
        logBuffer->snapshot(entries);
        int errors = 0;
        for (const auto& e : entries)
            if (e.level >= spdlog::level::err)
                ++errors;
        const uint32_t guardHits = dsptb::rt::rtGuardViolations();
        if (guardHits > 0)
            std::fprintf(stderr, "RT guard: %u allocation(s) on the audio thread\n", guardHits);
        exitCode = (errors == 0 && guardHits == 0) ? 0 : 1;
        std::printf("smoke-test: %s (%d frames, %d error log lines, %llu audio callbacks, %llu underruns)\n",
                    exitCode == 0 ? "OK" : "FAILED", frame, errors,
                    static_cast<unsigned long long>(engineCallbacks),
                    static_cast<unsigned long long>(engineUnderruns));
    }
    {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loopStart).count();
        spdlog::info("rendered {} frames in {:.2f} s ({:.1f} fps average)", frame, seconds,
                     seconds > 0 ? frame / seconds : 0.0);
    }
    spdlog::info("exiting with code {}", exitCode);
    dsptb::shutdownLogging();
    return exitCode;
}

} // namespace dsptb::app
