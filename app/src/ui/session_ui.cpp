#include "session_ui.hpp"

#include "file_dialog.hpp"
#include "shell_internal.hpp"

#include <dsptb/branding.h>
#include <dsptb/core/paths.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace dsptb::ui {

namespace fs = std::filesystem;

namespace {
constexpr double kDirtyCheckSeconds = 0.5;
constexpr double kAutosaveSeconds = 120.0;
constexpr const char* kSessionExt = ".dsptb.json";

fs::path autosavePath() {
    return dataDir() / "autosave.dsptb.json";
}

std::wstring widenUtf8(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void centerNextWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace

SessionController::SessionController(ShellContext& ctx, bool persistent)
    : ctx_(ctx), persistent_(persistent) {
    std::error_code ec;
    if (persistent_ && fs::exists(autosavePath(), ec))
        showRestore_ = true;
    baseline_ = snapshot(); // an empty session
}

SessionController::~SessionController() {
    if (export_.job)
        export_.job->cancel();
    if (recorder_.active() && ctx_.model)
        recorder_.stop(*ctx_.model);
}

void SessionController::notice(NoticeLevel level, const std::string& text) {
    ctx_.notices->push(level, text, ImGui::GetTime());
}

// ---------------------------------------------------------------------------------------------------------
// state capture
// ---------------------------------------------------------------------------------------------------------

void SessionController::syncPanelFlags() {
    for (const auto& r : ctx_.model->instances()) {
        const auto it = ctx_.moduleUi.panelOpen.find(r.id);
        if (it != ctx_.moduleUi.panelOpen.end())
            if (InstanceRecord* rec = ctx_.model->find(r.id))
                rec->panelOpen = it->second;
    }
}

SessionDoc SessionController::capture(const fs::path& forFile, bool withLayout) {
    syncPanelFlags();
    SessionDoc doc = captureSession(*ctx_.audio, *ctx_.model, forFile);
    if (withLayout) {
        const char* ini = ImGui::SaveIniSettingsToMemory();
        doc.layout = ini ? ini : "";
    }
    return doc;
}

std::string SessionController::snapshot() {
    return sessionToJson(capture(file_, false));
}

std::string SessionController::windowTitle() const {
    const std::string name = file_.empty() ? "Untitled" : toUtf8(file_.filename());
    return name + (dirty_ ? "*" : "") + " - " + DSPTB_APP_NAME;
}

std::filesystem::path SessionController::recordFolder() const {
    if (ctx_.settings->recordFolder.empty())
        return dataDir() / "recordings";
    return fromUtf8(ctx_.settings->recordFolder);
}

// ---------------------------------------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------------------------------------

void SessionController::tick(double now) {
    now_ = now;
    if (baselinePending_ && !ctx_.audio->loading()) {
        baseline_ = snapshot(); // the file is in, so its path and length are part of the baseline
        baselinePending_ = false;
    }
    if (!baselinePending_ && now - lastDirtyCheck_ >= kDirtyCheckSeconds) {
        lastDirtyCheck_ = now;
        dirty_ = snapshot() != baseline_;
    }
    if (persistent_) {
        if (!dirty_)
            lastAutosave_ = now;
        else if (now - lastAutosave_ >= kAutosaveSeconds) {
            lastAutosave_ = now;
            autosaveNow();
        }
    }

    if (recorder_.active()) {
        const Engine& e = ctx_.audio->engine();
        if (recorder_.failed()) {
            const std::string why = recorder_.failure();
            stopRecording();
            notice(NoticeLevel::Error, "Recording stopped: " + why);
        } else if (!e.isRunning() || ctx_.audio->deviceLost()) {
            stopRecording();
            notice(NoticeLevel::Warning, "Recording stopped because the audio stream stopped.");
        }
    }
    finishExportIfDone();
}

void SessionController::autosaveNow() {
    std::error_code ec;
    fs::create_directories(autosavePath().parent_path(), ec);
    std::string err;
    if (!writeSessionFile(autosavePath(), capture(autosavePath(), true), err))
        spdlog::warn("Autosave failed: {}", err);
    else
        spdlog::info("Autosaved the session");
}

void SessionController::removeAutosave() {
    std::error_code ec;
    fs::remove(autosavePath(), ec);
}

// ---------------------------------------------------------------------------------------------------------
// requests
// ---------------------------------------------------------------------------------------------------------

void SessionController::guard(Next next, const std::string& path) {
    if (!baselinePending_)
        dirty_ = snapshot() != baseline_;
    if (!dirty_) {
        pendingNext_ = next;
        pendingPath_ = path;
        return;
    }
    showUnsaved_ = true;
    unsavedNext_ = next;
    unsavedPath_ = path;
}

void SessionController::requestNew() {
    guard(Next::New);
}
void SessionController::requestOpen() {
    guard(Next::Open);
}
void SessionController::requestOpenPath(const std::string& utf8Path) {
    guard(Next::OpenPath, utf8Path);
}
void SessionController::requestExit() {
    guard(Next::Exit);
}
void SessionController::requestSave() {
    wantSave_ = true;
}
void SessionController::requestSaveAs() {
    wantSaveAs_ = true;
}
void SessionController::requestChooseRecordFolder() {
    chooseFolder_ = true;
}

void SessionController::requestExport() {
    if (!canExport())
        return;
    if (export_.out.empty()) {
        const fs::path src = fromUtf8(ctx_.audio->file()->path);
        export_.out = src.parent_path() / (src.stem().wstring() + L"_processed.wav");
    }
    export_.open = true;
}

// ---------------------------------------------------------------------------------------------------------
// save and load
// ---------------------------------------------------------------------------------------------------------

bool SessionController::saveTo(const fs::path& file) {
    SessionDoc doc = capture(file, true);
    std::string err;
    if (!writeSessionFile(file, doc, err)) {
        notice(NoticeLevel::Error, "Could not save the session: " + err);
        return false;
    }
    file_ = file;
    doc.layout.clear();
    baseline_ = sessionToJson(doc); // same as snapshot() now that file_ is set
    baselinePending_ = false;
    dirty_ = false;
    Settings::pushRecent(ctx_.settings->recentSessions, toUtf8(file));
    removeAutosave();
    spdlog::info("Saved session {}", toUtf8(file));
    notice(NoticeLevel::Info, "Saved " + toUtf8(file.filename()));
    return true;
}

bool SessionController::saveAs(HWND owner) {
    fs::path start = file_.empty() ? fs::path() : file_.parent_path();
    if (start.empty() && ctx_.audio->file())
        start = fromUtf8(ctx_.audio->file()->path).parent_path();
    const std::wstring name = file_.empty() ? L"Untitled.dsptb.json" : file_.filename().wstring();
    fs::path chosen;
    if (!pickSessionToSave(owner, start, name, chosen))
        return false;
    if (chosen.extension() != ".json")
        chosen += kSessionExt;
    return saveTo(chosen);
}

bool SessionController::loadFrom(const fs::path& file, bool remember) {
    SessionDoc doc;
    std::string err;
    if (!readSessionFile(file, doc, err)) {
        notice(NoticeLevel::Error, "Could not open the session: " + err);
        return false;
    }
    std::vector<SessionMessage> messages;
    const ApplyResult r = applySession(doc, file, *ctx_.audio, *ctx_.model, messages);
    for (const auto& m : messages)
        notice(m.level, m.text);
    ctx_.moduleUi.panelOpen.clear();
    for (const auto& rec : ctx_.model->instances())
        if (!rec.removing)
            ctx_.moduleUi.panelOpen[rec.id] = rec.panelOpen;
    if (!doc.layout.empty())
        ImGui::LoadIniSettingsFromMemory(doc.layout.c_str(), doc.layout.size());
    spdlog::info("Loaded session {}", toUtf8(file));
    afterLoad(r, file, remember);
    return true;
}

void SessionController::afterLoad(const ApplyResult& r, const fs::path& file, bool remember) {
    file_ = file;
    if (remember)
        Settings::pushRecent(ctx_.settings->recentSessions, toUtf8(file));
    dirty_ = false;
    if (r.fileMissing) {
        baseline_ = snapshot();
        baselinePending_ = false;
        showLocate_ = true;
        missingPath_ = r.missingPath;
    } else {
        baselinePending_ = r.wantsFile; // wait for the audio file so its length is in the baseline
        if (!r.wantsFile)
            baseline_ = snapshot();
    }
}

void SessionController::runNext(HWND owner, Next next, const std::string& path) {
    switch (next) {
    case Next::New:
        resetSession(*ctx_.audio, *ctx_.model);
        ctx_.moduleUi.panelOpen.clear();
        file_.clear();
        baseline_ = snapshot();
        baselinePending_ = false;
        dirty_ = false;
        break;
    case Next::Open: {
        fs::path chosen;
        if (pickSessionToOpen(owner, chosen))
            loadFrom(chosen);
        break;
    }
    case Next::OpenPath:
        loadFrom(fromUtf8(path));
        break;
    case Next::Exit:
        if (recorder_.active())
            stopRecording();
        removeAutosave();
        exitConfirmed_ = true;
        break;
    case Next::None:
        break;
    }
}

void SessionController::processPending(HWND owner) {
    if (wantSave_ || wantSaveAs_) {
        const bool as = wantSaveAs_ || file_.empty();
        wantSave_ = wantSaveAs_ = false;
        const bool ok = as ? saveAs(owner) : saveTo(file_);
        if (ok && afterSave_ != Next::None) {
            pendingNext_ = afterSave_;
            pendingPath_ = afterSavePath_;
        }
        afterSave_ = Next::None;
    }
    if (chooseFolder_) {
        chooseFolder_ = false;
        fs::path p;
        if (pickFolder(owner, p))
            ctx_.settings->recordFolder = toUtf8(p);
    }
    if (export_.choosePath) {
        export_.choosePath = false;
        fs::path p;
        if (pickWavToSave(owner, export_.out.parent_path(), export_.out.filename().wstring(), p))
            export_.out = p;
    }
    if (locateFile_) {
        locateFile_ = false;
        fs::path p;
        if (pickAudioFile(owner, p)) {
            ctx_.audio->openFile(p);
            Settings::pushRecent(ctx_.settings->recentAudioFiles, toUtf8(p));
        }
    }
    if (pendingNext_ != Next::None) {
        const Next next = pendingNext_;
        const std::string path = pendingPath_;
        pendingNext_ = Next::None;
        pendingPath_.clear();
        runNext(owner, next, path);
    }
}

// ---------------------------------------------------------------------------------------------------------
// prompts
// ---------------------------------------------------------------------------------------------------------

void SessionController::drawUnsavedPrompt() {
    if (showUnsaved_) {
        ImGui::OpenPopup("Unsaved changes");
        showUnsaved_ = false;
    }
    centerNextWindow();
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const std::string name = file_.empty() ? "Untitled" : toUtf8(file_.filename());
    ImGui::Text("Save the changes to \"%s\"?", name.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Save", ImVec2(90.0f * dpiScale(), 0.0f))) {
        wantSave_ = true;
        afterSave_ = unsavedNext_;
        afterSavePath_ = unsavedPath_;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(90.0f * dpiScale(), 0.0f))) {
        pendingNext_ = unsavedNext_;
        pendingPath_ = unsavedPath_;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f * dpiScale(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SessionController::drawLocatePrompt() {
    if (showLocate_) {
        ImGui::OpenPopup("Audio file not found");
        showLocate_ = false;
    }
    centerNextWindow();
    if (!ImGui::BeginPopupModal("Audio file not found", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextWrapped("The session's audio file could not be found:");
    ImGui::TextDisabled("%s", missingPath_.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Locate...", ImVec2(110.0f * dpiScale(), 0.0f))) {
        locateFile_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(110.0f * dpiScale(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SessionController::drawRestorePrompt() {
    if (showRestore_) {
        ImGui::OpenPopup("Restore session");
        showRestore_ = false;
    }
    centerNextWindow();
    if (!ImGui::BeginPopupModal("Restore session", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextWrapped("The app did not close normally last time. Restore the session that was open?");
    ImGui::Spacing();
    if (ImGui::Button("Restore", ImVec2(110.0f * dpiScale(), 0.0f))) {
        if (loadFrom(autosavePath(), /*remember=*/false)) {
            file_.clear(); // it is an unsaved session
            baseline_.clear();
            baselinePending_ = false;
            dirty_ = true;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(110.0f * dpiScale(), 0.0f))) {
        removeAutosave();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------
// export
// ---------------------------------------------------------------------------------------------------------

bool SessionController::canExport() const {
    return ctx_.audio->source() == SourceKind::File && ctx_.audio->file() && !ctx_.audio->loading() &&
           !exporting();
}

void SessionController::startExport() {
    const auto file = ctx_.audio->file();
    if (!file)
        return;
    SessionDoc doc = capture(file_, false);
    ExportOptions opt;
    opt.out = export_.out;
    opt.tailSeconds = static_cast<double>(export_.tailSeconds);
    opt.format = export_.format == 0 ? WavFormat::S16 : export_.format == 1 ? WavFormat::S24 : WavFormat::F32;
    opt.loopRegionOnly = !export_.wholeFile && ctx_.audio->loopEnabled();
    opt.regionStartSeconds = ctx_.audio->loopStartSeconds();
    opt.regionEndSeconds = ctx_.audio->loopEndSeconds();
    std::string err;
    export_.job = ExportJob::start(pool_, *ctx_.registry, descriptionFromSession(doc), file->audio, opt, err);
    if (!export_.job) {
        notice(NoticeLevel::Error, "Cannot export: " + err);
        export_.open = false;
        return;
    }
    spdlog::info("Export started: {}", toUtf8(opt.out));
}

void SessionController::finishExportIfDone() {
    if (!export_.job || !export_.job->done())
        return;
    const auto job = export_.job;
    export_.job.reset();
    export_.open = false;
    if (job->succeeded()) {
        spdlog::info("Exported {}", toUtf8(job->output()));
        notice(NoticeLevel::Info, "Exported " + toUtf8(job->output().filename()));
    } else if (job->cancelled()) {
        notice(NoticeLevel::Info, "Export cancelled.");
    } else {
        notice(NoticeLevel::Error, "Export failed: " + job->error());
    }
}

void SessionController::drawExportDialog() {
    if (export_.open && !ImGui::IsPopupOpen("Export Processed WAV"))
        ImGui::OpenPopup("Export Processed WAV");
    centerNextWindow();
    bool open = true;
    if (!ImGui::BeginPopupModal("Export Processed WAV", &open, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const float s = dpiScale();
    if (!export_.open) { // finished while the dialog was up
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (export_.job) {
        ImGui::Text("Exporting %s", toUtf8(export_.out.filename()).c_str());
        ImGui::ProgressBar(export_.job->progress(), ImVec2(320.0f * s, 0.0f));
        if (ImGui::Button("Cancel", ImVec2(100.0f * s, 0.0f)))
            export_.job->cancel();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("Renders the file through the enabled processors, faster than real time. "
                       "Playback is not affected.");
    ImGui::Spacing();
    const bool hasLoop = ctx_.audio->loopEnabled();
    if (!hasLoop)
        export_.wholeFile = true;
    if (ImGui::RadioButton("Whole file", export_.wholeFile))
        export_.wholeFile = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasLoop);
    if (ImGui::RadioButton("Loop region", !export_.wholeFile))
        export_.wholeFile = false;
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(220.0f * s);
    ImGui::SliderFloat("Tail (s)", &export_.tailSeconds, 0.0f, 10.0f, "%.1f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(
            "Silence appended after the audio so reverbs and other effect tails are not cut off.");
    ImGui::SetNextItemWidth(220.0f * s);
    const char* formats[] = {"16-bit PCM (dithered)", "24-bit PCM", "32-bit float"};
    ImGui::Combo("Format", &export_.format, formats, 3);

    ImGui::Spacing();
    const std::string out = toUtf8(export_.out);
    ImGui::TextDisabled("%s", out.c_str());
    if (ImGui::Button("Choose file...", ImVec2(130.0f * s, 0.0f)))
        export_.choosePath = true;

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Export", ImVec2(100.0f * s, 0.0f)))
        startExport();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100.0f * s, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        export_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (!open)
        export_.open = false;
}

// ---------------------------------------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------------------------------------

void SessionController::startRecording(dsptb_tap tap) {
    const Engine& e = ctx_.audio->engine();
    std::string err;
    if (!recorder_.start(*ctx_.model, e.isRunning() ? e.sampleRate() : 0, tap, recordFolder(), err)) {
        notice(NoticeLevel::Error, "Cannot record: " + err);
        return;
    }
    ctx_.settings->recordTap = tap == DSPTB_TAP_PRE ? "pre" : "post";
}

void SessionController::stopRecording() {
    if (!recorder_.active())
        return;
    const fs::path file = recorder_.stop(*ctx_.model);
    const uint64_t dropped = recorder_.overrunFrames();
    char text[256];
    std::snprintf(text, sizeof text, "Saved recording %s (%.1f s)", toUtf8(file.filename()).c_str(),
                  recorder_.elapsedSeconds());
    notice(NoticeLevel::Info, text);
    if (dropped > 0)
        notice(NoticeLevel::Warning, "The recording has gaps: " + std::to_string(dropped) +
                                         " frames were dropped because the disk could not keep up.");
}

void SessionController::drawDialogs() {
    drawUnsavedPrompt();
    drawLocatePrompt();
    drawRestorePrompt();
    drawExportDialog();
}

} // namespace dsptb::ui
