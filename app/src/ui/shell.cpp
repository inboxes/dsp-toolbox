#include "shell_internal.hpp"

#include "module_ui.hpp"
#include "session_ui.hpp"

#include <dsptb/branding.h>
#include <dsptb/core/paths.hpp>
#include <dsptb/core/version.hpp>

#include <imgui_internal.h> // DockBuilder, BeginViewportSideBar, undock queue

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

namespace dsptb::ui {

namespace {

// Fixed ID so the saved dock layout in settings.json stays valid between runs.
constexpr ImGuiID kDockSpaceId = 0x44535042;

// ---------------------------------------------------------------------------------------------------------
// Dock layout: Module Browser left, module panels centre, Chain right, Transport + Log bottom.
// ---------------------------------------------------------------------------------------------------------
void buildDefaultLayout(ShellContext& ctx) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(kDockSpaceId);
    ImGui::DockBuilderAddNode(kDockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(kDockSpaceId, vp->WorkSize);

    ImGuiID centre = kDockSpaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, nullptr, &centre);
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.25f, nullptr, &centre);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.32f, nullptr, &centre);
    const ImGuiID bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.42f, nullptr, &bottom);

    ImGui::DockBuilderDockWindow(kPanelNames[kModuleBrowser], left);
    ImGui::DockBuilderDockWindow(kPanelNames[kChain], right);
    ImGui::DockBuilderDockWindow(kPanelNames[kTransport], bottom);
    ImGui::DockBuilderDockWindow(kPanelNames[kLog], bottomRight);
    ImGui::DockBuilderFinish(kDockSpaceId);
    ctx.defaultLayoutNeeded = false;
}

// Two-step undock: ImGui processes a queued undock at the start of the next frame, after which the window is
// floating and can be given a sensible position and size.
void advanceUndock(ShellContext& ctx) {
    if (ctx.undockPhase == 0 || ctx.undockPanel < 0)
        return;
    const char* name = kPanelNames[ctx.undockPanel];
    if (ctx.undockPhase == 1) {
        if (ImGuiWindow* w = ImGui::FindWindowByName(name))
            ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), w);
        ctx.undockPhase = 2;
    } else {
        const float s = dpiScale();
        const ImVec2 origin = ImGui::GetMainViewport()->WorkPos;
        const float step = 32.0f * s * static_cast<float>(ctx.undockPanel);
        ImGui::SetWindowPos(name, ImVec2(origin.x + 90.0f * s + step, origin.y + 90.0f * s + step),
                            ImGuiCond_Always);
        ImGui::SetWindowSize(name, ImVec2(400.0f * s, 300.0f * s), ImGuiCond_Always);
        ctx.undockPhase = 0;
        ctx.undockPanel = -1;
    }
}

void startUndock(ShellContext& ctx, int panel) {
    ctx.undockPanel = panel;
    ctx.undockPhase = 1;
}

// ---------------------------------------------------------------------------------------------------------
// Menus. Items whose feature does not exist yet are shown disabled.
// ---------------------------------------------------------------------------------------------------------
void disabledItem(const char* label) {
    ImGui::MenuItem(label, nullptr, false, false);
}

void openLogFolder() {
    const auto dir = dsptb::logDir();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void drawMenus(ShellContext& ctx) {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        SessionController& sc = *ctx.session;
        if (ImGui::MenuItem("New", "Ctrl+N"))
            sc.requestNew();
        if (ImGui::MenuItem("Open...", "Ctrl+Shift+O"))
            sc.requestOpen();
        if (ImGui::MenuItem("Save", "Ctrl+S"))
            sc.requestSave();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
            sc.requestSaveAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Open Audio File...", "Ctrl+O"))
            ctx.openFileRequested = true;
        if (ImGui::MenuItem("Export Processed WAV...", "Ctrl+E", false, sc.canExport()))
            sc.requestExport();
        ImGui::Separator();
        if (ImGui::BeginMenu("Recent Audio Files", !ctx.settings->recentAudioFiles.empty())) {
            for (const auto& r : ctx.settings->recentAudioFiles)
                if (ImGui::MenuItem(r.c_str()))
                    ctx.openFilePath = r;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Recent Sessions", !ctx.settings->recentSessions.empty())) {
            for (const auto& r : ctx.settings->recentSessions)
                if (ImGui::MenuItem(r.c_str()))
                    sc.requestOpenPath(r);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            ctx.quitRequested = true;
        ImGui::EndMenu();
    }
    drawEditMenu(ctx);
    if (ImGui::BeginMenu("Audio")) {
        if (ImGui::MenuItem("Settings..."))
            ctx.showAudioSettings = true;
        if (ImGui::BeginMenu("Record")) {
            SessionController& sc = *ctx.session;
            const bool rec = sc.recorder().active();
            if (ImGui::MenuItem("Record output (POST)", nullptr, false, !rec))
                sc.startRecording(DSPTB_TAP_POST);
            if (ImGui::MenuItem("Record input (PRE)", nullptr, false, !rec))
                sc.startRecording(DSPTB_TAP_PRE);
            if (ImGui::MenuItem("Stop recording", nullptr, false, rec))
                sc.stopRecording();
            ImGui::Separator();
            const std::string folder = toUtf8(sc.recordFolder());
            ImGui::TextDisabled("Folder: %s", folder.c_str());
            if (ImGui::MenuItem("Choose folder..."))
                sc.requestChooseRecordFolder();
            if (ImGui::MenuItem("Open folder")) {
                std::error_code ec;
                std::filesystem::create_directories(sc.recordFolder(), ec);
                ShellExecuteW(nullptr, L"open", sc.recordFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        Engine& engine = ctx.audio->engine();
        if (ImGui::MenuItem("Mute output (Esc)", nullptr, engine.muted()))
            engine.setMute(!engine.muted());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Modules")) {
        drawModulesMenuItems(ctx);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (int i = 0; i < kPanelCount; ++i)
            ImGui::MenuItem(kPanelNames[i], nullptr, &ctx.panelOpen[i]);
        if (ImGui::BeginMenu("Undock panel")) {
            for (int i = 0; i < kPanelCount; ++i) {
                const ImGuiWindow* w = ImGui::FindWindowByName(kPanelNames[i]);
                const bool docked = w && w->DockIsActive;
                if (ImGui::MenuItem(kPanelNames[i], nullptr, false, ctx.panelOpen[i] && docked))
                    startUndock(ctx, i);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
            requestResetLayout(ctx);
        ImGui::Separator();
        if (ImGui::BeginMenu("UI Scale")) {
            for (int pct : {100, 125, 150, 200}) {
                const std::string label = std::to_string(pct) + "%";
                if (ImGui::MenuItem(label.c_str(), nullptr, ctx.settings->uiScalePercent == pct))
                    ctx.settings->uiScalePercent = pct;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About"))
            ctx.showAbout = true;
        disabledItem("Licenses");
        ImGui::Separator();
        if (ImGui::MenuItem("Open log folder"))
            openLogFolder();
        disabledItem("SDK docs");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void drawAbout(ShellContext& ctx) {
    if (!ctx.showAbout)
        return;
    if (ImGui::Begin("About", &ctx.showAbout,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%s", DSPTB_APP_NAME);
        ImGui::TextDisabled("Version %s", dsptb::versionString());
        ImGui::Separator();
        ImGui::TextUnformatted("A modular real-time audio DSP workbench for Windows.");
        ImGui::TextUnformatted("Licensed under GPL-3.0-only. Built with Dear ImGui " IMGUI_VERSION ".");
        ImGui::TextUnformatted("Third-party licences are in THIRD_PARTY_NOTICES.md, next to the app.");
#ifdef DSPTB_HAS_ASIO
        ImGui::Spacing();
        if (ctx.asioLogo) {
            const float logoWidth = 72.0f * ImGui::GetFontSize() / 15.0f;
            ImGui::Image(ctx.asioLogo,
                         ImVec2(logoWidth, logoWidth * ctx.asioLogoSize.y / ctx.asioLogoSize.x));
        }
        ImGui::TextUnformatted("ASIO is a registered trademark of Steinberg Media Technologies GmbH.");
#endif
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------------------------------------

// Right-click anywhere in a panel to undock it, close it or reset the layout.
void panelContextMenu(ShellContext& ctx, int panel) {
    const bool docked = ImGui::IsWindowDocked(); // read before the popup window becomes current
    if (ImGui::BeginPopupContextWindow("##panelMenu")) {
        if (ImGui::MenuItem("Undock", nullptr, false, docked))
            startUndock(ctx, panel);
        if (ImGui::MenuItem("Close panel"))
            ctx.panelOpen[panel] = false;
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
            requestResetLayout(ctx);
        ImGui::EndPopup();
    }
}

// Returns true if the panel's contents should be drawn. The caller must call ImGui::End() in that case.
bool beginPanelImpl(ShellContext& ctx, int panel) {
    if (!ctx.panelOpen[panel])
        return false;
    const bool visible = ImGui::Begin(kPanelNames[panel], &ctx.panelOpen[panel]);
    if (!visible) {
        ImGui::End(); // collapsed or hidden behind another tab
        return false;
    }
    panelContextMenu(ctx, panel);
    return true;
}

ImVec4 levelColor(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace:
    case spdlog::level::debug:
        return ImVec4(0.55f, 0.58f, 0.62f, 1.0f);
    case spdlog::level::warn:
        return ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
    case spdlog::level::err:
    case spdlog::level::critical:
        return ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

void drawLog(ShellContext& ctx) {
    if (!beginPanelImpl(ctx, kLog))
        return;

    if (ctx.log->version() != ctx.logCacheVersion) {
        ctx.logCacheVersion = ctx.log->version();
        ctx.log->snapshot(ctx.logCache);
    }

    static const char* kLevels[] = {"Trace", "Debug", "Info", "Warning", "Error"};
    ImGui::SetNextItemWidth(90.0f * dpiScale());
    ImGui::Combo("##level", &ctx.logMinLevel, kLevels, IM_ARRAYSIZE(kLevels));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show messages at or above this level");
    ImGui::SameLine();
    const bool copy = ImGui::Button("Copy");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        ctx.log->clear();
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &ctx.logAutoScroll);

    std::vector<const dsptb::LogEntry*> shown;
    shown.reserve(ctx.logCache.size());
    for (const auto& e : ctx.logCache)
        if (static_cast<int>(e.level) >= ctx.logMinLevel)
            shown.push_back(&e);

    if (copy) {
        std::string all;
        for (const auto* e : shown)
            all += e->text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }

    if (ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& e = *shown[static_cast<size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, levelColor(e.level));
                ImGui::TextUnformatted(e.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (ctx.logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------
// Status bar, banners, empty-centre hint
// ---------------------------------------------------------------------------------------------------------
void drawStatusBar(ShellContext& ctx) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            drawAudioStatusFields(ctx);

            char right[64];
            std::snprintf(right, sizeof right, "%.0f fps  |  v%s", static_cast<double>(ctx.fps),
                          dsptb::versionString());
            const float w = ImGui::CalcTextSize(right).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x * 2.0f);
            ImGui::TextDisabled("%s", right);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

ImVec4 noticeColor(dsptb::NoticeLevel level) {
    switch (level) {
    case dsptb::NoticeLevel::Warning:
        return ImVec4(0.45f, 0.33f, 0.08f, 0.96f);
    case dsptb::NoticeLevel::Error:
        return ImVec4(0.50f, 0.14f, 0.14f, 0.96f);
    default:
        return ImVec4(0.14f, 0.28f, 0.46f, 0.96f);
    }
}

// Non-modal banners at the top of the dockspace; click one to dismiss it.
void drawBanners(ShellContext& ctx) {
    ctx.notices->update(ImGui::GetTime());
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = dpiScale();
    float y = vp->WorkPos.y + 8.0f * s;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoMove;
    // Copy the list: dismissing while iterating would invalidate it.
    const auto notices = ctx.notices->active();
    int index = 0;
    for (const auto& n : notices) {
        const std::string name = "##banner" + std::to_string(index++);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, y), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, noticeColor(n.level));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 8.0f * s));
        if (ImGui::Begin(name.c_str(), nullptr, flags)) {
            ImGui::TextUnformatted(n.text.c_str());
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                ctx.notices->dismiss(n.id);
            y += ImGui::GetWindowHeight() + 6.0f * s;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

void drawCentreHint() {
    const ImGuiDockNode* node = ImGui::DockBuilderGetCentralNode(kDockSpaceId);
    if (!node)
        return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const char* title = DSPTB_APP_NAME;
    const char* hint = "Module panels will appear here.";
    const char* tip = "Tip: drag a panel's title to undock it, or right-click inside a panel.";
    const ImVec2 ts = ImGui::CalcTextSize(title);
    const ImVec2 hs = ImGui::CalcTextSize(hint);
    const ImVec2 c(node->Pos.x + node->Size.x * 0.5f, node->Pos.y + node->Size.y * 0.5f);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y), col, title);
    dl->AddText(ImVec2(c.x - hs.x * 0.5f, c.y + 4.0f * dpiScale()), col, hint);
    const ImVec2 tps = ImGui::CalcTextSize(tip);
    dl->AddText(ImVec2(c.x - tps.x * 0.5f, c.y + hs.y + 14.0f * dpiScale()), col, tip);
}

} // namespace

ImGuiID centralDockId() {
    const ImGuiDockNode* node = ImGui::DockBuilderGetCentralNode(kDockSpaceId);
    return node ? node->ID : kDockSpaceId;
}

float dpiScale() {
    return ImGui::GetStyle().FontScaleDpi;
}

bool beginPanel(ShellContext& ctx, int panel) {
    return beginPanelImpl(ctx, panel);
}

void notify(ShellContext& ctx, dsptb::NoticeLevel level, const std::string& text) {
    ctx.notices->push(level, text, ImGui::GetTime());
}

void requestResetLayout(ShellContext& ctx) {
    for (bool& open : ctx.panelOpen)
        open = true;
    ctx.defaultLayoutNeeded = true;
    ctx.undockPhase = 0;
    ctx.undockPanel = -1;
}

void initLayout(ShellContext& ctx) {
    // After LoadIniSettingsFromMemory the saved dock nodes (if any) already exist.
    ctx.defaultLayoutNeeded = ImGui::DockBuilderGetNode(kDockSpaceId) == nullptr;
}

void requestUndock(ShellContext& ctx, const char* panelName) {
    for (int i = 0; i < kPanelCount; ++i)
        if (std::string(kPanelNames[i]) == panelName)
            startUndock(ctx, i);
}

void loadPanelState(ShellContext& ctx, const dsptb::Settings& s) {
    for (int i = 0; i < kPanelCount; ++i) {
        const auto it = s.panelsOpen.find(kPanelNames[i]);
        ctx.panelOpen[i] = it == s.panelsOpen.end() ? true : it->second;
    }
}

void storePanelState(const ShellContext& ctx, dsptb::Settings& s) {
    for (int i = 0; i < kPanelCount; ++i)
        s.panelsOpen[kPanelNames[i]] = ctx.panelOpen[i];
}

// Ctrl+N new, Ctrl+Shift+O open, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+E export. (Ctrl+O opens audio.)
void handleSessionShortcuts(ShellContext& ctx) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !io.KeyCtrl || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        return;
    SessionController& sc = *ctx.session;
    if (ImGui::IsKeyPressed(ImGuiKey_N, false))
        sc.requestNew();
    else if (ImGui::IsKeyPressed(ImGuiKey_O, false) && io.KeyShift)
        sc.requestOpen();
    else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
        io.KeyShift ? sc.requestSaveAs() : sc.requestSave();
    else if (ImGui::IsKeyPressed(ImGuiKey_E, false) && sc.canExport())
        sc.requestExport();
}

void drawShell(ShellContext& ctx) {
    handleSessionShortcuts(ctx);
    handleAudioShortcuts(ctx);
    handleModuleShortcuts(ctx);
    advanceUndock(ctx);
    drawMenus(ctx);
    drawStatusBar(ctx);

    ImGui::DockSpaceOverViewport(kDockSpaceId, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);
    if (ctx.defaultLayoutNeeded)
        buildDefaultLayout(ctx);
    drawCentreHint();

    drawModuleBrowserPanel(ctx);
    drawChainPanel(ctx);
    drawTransportPanel(ctx);
    drawLog(ctx);
    drawInstancePanels(ctx);
    drawAudioSettingsWindow(ctx);
    drawAbout(ctx);
    drawInstallDialog();
    ctx.session->drawDialogs();
    drawBanners(ctx);
}

} // namespace dsptb::ui
