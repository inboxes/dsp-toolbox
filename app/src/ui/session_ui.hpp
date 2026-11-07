#pragma once

// Everything the UI does with session files, export and recording: the current file and
// its unsaved-changes state, the menu actions, the prompts, autosave and restore, the export dialog and the
// recorder. The system file dialogs are modal, so menu actions only record a request; `processPending` runs
// them between frames.

#include "shell.hpp"

#include <dsptb/core/export_job.hpp>
#include <dsptb/core/recorder.hpp>
#include <dsptb/core/session_io.hpp>
#include <dsptb/core/worker_pool.hpp>

#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

namespace dsptb::ui {

class SessionController {
public:
    // `persistent` false (test modes): no autosave, no restore prompt, nothing written to the user's profile.
    SessionController(ShellContext& ctx, bool persistent);
    ~SessionController();

    // ---- per frame ----
    void tick(double now); // before drawShell: dirty check, autosave, export and recorder bookkeeping
    void drawDialogs();    // from drawShell: the prompts and the export dialog
    void processPending(HWND owner); // after the frame: runs file dialogs and the actions waiting on them

    // ---- requests (menu, shortcuts, test actions) ----
    void requestNew();
    void requestOpen();
    void requestOpenPath(const std::string& utf8Path);
    void requestSave();
    void requestSaveAs();
    void requestExit(); // the window's close button
    void requestExport();
    void requestChooseRecordFolder();
    void startRecording(dsptb_tap tap);
    void stopRecording();

    // ---- direct operations (no dialogs; used by the requests and by test actions) ----
    bool saveTo(const std::filesystem::path& file);
    // `remember` = add to Recent Sessions (false for the autosave).
    bool loadFrom(const std::filesystem::path& file, bool remember = true);

    // ---- state ----
    bool dirty() const { return dirty_; }
    std::string windowTitle() const;
    bool exitConfirmed() const { return exitConfirmed_; }
    bool canExport() const;
    bool exporting() const { return export_.job && !export_.job->done(); }
    Recorder& recorder() { return recorder_; }
    std::filesystem::path recordFolder() const;
    const std::filesystem::path& currentFile() const { return file_; }

private:
    enum class Next { None, New, Open, OpenPath, Exit };
    struct ExportUi {
        bool open = false;
        bool wholeFile = true;
        float tailSeconds = 2.0f;
        int format = 1; // 0 = 16-bit, 1 = 24-bit, 2 = 32-bit float
        std::filesystem::path out;
        std::shared_ptr<ExportJob> job;
        bool choosePath = false;
    };

    std::string snapshot(); // the session as JSON without the layout, for the unsaved-changes check
    SessionDoc capture(const std::filesystem::path& forFile, bool withLayout);
    void syncPanelFlags(); // module panel open/closed -> records, before a capture
    void guard(Next next, const std::string& path = {});
    void runNext(HWND owner, Next next, const std::string& path);
    bool saveAs(HWND owner);
    void afterLoad(const ApplyResult& r, const std::filesystem::path& file, bool remember);
    void autosaveNow();
    void removeAutosave();
    void drawUnsavedPrompt();
    void drawLocatePrompt();
    void drawRestorePrompt();
    void drawExportDialog();
    void finishExportIfDone();
    void startExport();
    void notice(NoticeLevel level, const std::string& text);

    ShellContext& ctx_;
    const bool persistent_;
    WorkerPool pool_{1};
    Recorder recorder_;

    std::filesystem::path file_;   // empty = untitled
    std::string baseline_;         // snapshot at the last save / load / new
    bool baselinePending_ = false; // waiting for the audio file to finish loading before taking it
    bool dirty_ = false;
    double lastDirtyCheck_ = -1.0;
    double lastAutosave_ = 0.0;
    double now_ = 0.0;

    // Requests waiting for the end of the frame.
    Next pendingNext_ = Next::None;
    std::string pendingPath_;
    bool wantSave_ = false, wantSaveAs_ = false;
    bool chooseFolder_ = false;
    bool locateFile_ = false;
    // After "Save" in the unsaved prompt: what to do once the save worked.
    Next afterSave_ = Next::None;
    std::string afterSavePath_;

    // Prompts.
    bool showUnsaved_ = false;
    Next unsavedNext_ = Next::None;
    std::string unsavedPath_;
    bool showLocate_ = false;
    std::string missingPath_;
    bool showRestore_ = false;

    bool exitConfirmed_ = false;
    ExportUi export_;
    bool exportJustFinished_ = false;
};

} // namespace dsptb::ui
