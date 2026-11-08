#pragma once

#include "analysis_host.hpp"
#include "engine.hpp"
#include "module_instance.hpp"
#include "module_registry.hpp"
#include "notifications.hpp"
#include "session_file.hpp"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace dsptb {

// A banner the UI should show.
struct SessionMessage {
    NoticeLevel level = NoticeLevel::Info;
    std::string text;
};

struct InstanceRecord {
    InstanceId id = 0;
    std::shared_ptr<ModuleInstance> instance;
    bool enabled = true;
    bool removing = false; // ramping out; dropped from the chain when `removeAt` passes
    double removeAt = 0.0;
    bool faultReported = false;
    bool panelOpen = true;
    std::string label;     // user-chosen name shown in the chain and the panel title; empty = module name
    std::string lastState; // module state as of the last undo step (for state undo)
    // Analyzers only: where they listen, and the ring the audio thread fills for them.
    dsptb_tap tap = DSPTB_TAP_POST;
    std::shared_ptr<TapRing> ring;
    bool isAnalyzer() const { return instance && instance->kind() == DSPTB_KIND_ANALYZER; }
    bool isProcessor() const { return instance && instance->kind() == DSPTB_KIND_PROCESSOR; }
    bool isOffline() const { return instance && instance->kind() == DSPTB_KIND_OFFLINE; }
};

// UI-thread owner of the session's module instances. Every structural change builds a new
// immutable ChainSnapshot and publishes it to the engine through RtSwap; instances are shared with the old
// and the new snapshot and only destroyed once the audio thread has let go of the last snapshot that holds
// them.
//
// Time: the model never reads a clock. tick(now) takes any monotonically increasing time in seconds, so the
// timed transitions (remove ramp, reorder dip) are deterministic in tests.
class SessionModel final : public IModuleHost {
public:
    // `analysisThread` false: analyzers only run when analysis().pumpOnce() is called (deterministic tests).
    SessionModel(Engine& engine, ModuleRegistry& registry, bool analysisThread = true);
    ~SessionModel() override;

    // ---- structure ----
    // Creates, prepares and appends an instance (enabled). Returns 0 and `error` on failure.
    InstanceId add(const std::string& moduleId, std::string& error);
    // Ramps the instance out (20 ms) and then drops it. Immediate when the engine is not running.
    bool remove(InstanceId id);
    // Processors only (analyzers are not part of the chain order). `newIndex` counts processors.
    // Moves an instance to `newIndex` in the chain with the reorder dip: the chain output fades out (10 ms),
    // the new order is swapped in, then it fades back in. Immediate when the engine is not running.
    bool move(InstanceId id, size_t newIndex);
    bool setEnabled(InstanceId id, bool enabled);
    // Analyzers: listen before (PRE) or after (POST) the processor chain.
    bool setTap(InstanceId id, dsptb_tap tap);
    // Names an instance (at most 48 characters; empty restores the module name).
    bool setLabel(InstanceId id, std::string label);

    const std::vector<InstanceRecord>& instances() const { return records_; }
    AnalysisHost& analysis() { return analysis_; }
    const AnalysisHost& analysis() const { return analysis_; }
    InstanceRecord* find(InstanceId id);
    // Total latency of the processors in the chain, in frames (a bypassed slot still delays its dry signal).
    uint32_t totalLatencyFrames() const;
    bool reorderPending() const { return reorderPending_; }
    size_t pendingRemovals() const;

    // ---- parameters and undo ----
    // Live edit while a control is being dragged: no undo step.
    void setParam(InstanceId id, uint32_t index, float value);
    // Records one undo step for a finished edit (old -> new). No-op when equal.
    void commitParam(InstanceId id, uint32_t index, float oldValue, float newValue);
    // Sets the value and records an undo step in one go (typed value, reset to default).
    void setParamUndoable(InstanceId id, uint32_t index, float value);
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    static constexpr size_t kMaxUndo = 100;

    // ---- persistence ----
    // An instance whose module is not installed (or failed to load). It keeps its saved data so saving again
    // loses nothing; it is not part of the chain and makes no sound.
    struct Placeholder {
        SessionInstanceDoc doc;
        size_t slot = 0; // position among all instances when it was loaded; kept stable on save
        std::string reason;
    };
    const std::vector<Placeholder>& placeholders() const { return placeholders_; }
    bool removePlaceholder(size_t index);
    // Every instance (placeholders included) as session data, in file order.
    std::vector<SessionInstanceDoc> capture() const;
    // Replaces the whole chain with `docs`. Unknown parameters are logged and skipped, missing ones keep
    // the defaults, a module that is missing or fails to create becomes a placeholder. Clears the undo
    // history. Problems are appended to `messages`.
    void restore(const std::vector<SessionInstanceDoc>& docs, std::vector<SessionMessage>& messages);
    // Removes everything at once (New session).
    void clear();
    // The recorder's tap: the engine fills `ring` with the PRE or POST signal. Null stops
    // it.
    void setRecordTap(std::shared_ptr<TapRing> ring, dsptb_tap tap);

    // ---- housekeeping ----
    // Call every UI frame. Finishes timed transitions, handles faulted instances (banner + bypass), frees
    // retired snapshots.
    void tick(double now, std::vector<SessionMessage>& messages);
    // Prepares every instance for the engine's current format and republishes the chain. Called when the
    // stream (re)opens, before it starts (also hooked automatically).
    void prepareAll();

    // ---- IModuleHost ----
    void moduleLog(ModuleInstance& who, dsptb_log_level level, const char* utf8) override;
    void moduleSetParam(ModuleInstance& who, uint32_t index, float value) override;
    void moduleMarkStateDirty(ModuleInstance& who, const char* undoLabel) override;
    // <dataDir>/module-data/<module id>
    std::filesystem::path moduleDataDir(const std::string& moduleId) override;
    void moduleBeginGesture(ModuleInstance& who, const char* undoLabel) override;
    void moduleEndGesture(ModuleInstance& who) override;
    // Ends an open gesture (undo, redo, removing its instance, or the UI closing the panel).
    void endGesture();
    // Ends the open gesture only if it belongs to this instance.
    void endGestureOf(InstanceId id) {
        if (gesture_.open && gesture_.id == id)
            endGesture();
    }
    uint64_t workingFileGeneration() const override { return workingFile_.generation(); }
    std::shared_ptr<const AudioBuffer> acquireWorkingFile(uint64_t& generation) override {
        return workingFile_.acquire(generation);
    }
    // The file the transport plays (UI thread, cheap to call every frame). Modules see a new generation when it
    // changes, including a reload at a new sample rate.
    void setWorkingFile(const std::shared_ptr<const LoadedFile>& file) {
        workingFile_.set(file ? std::shared_ptr<const AudioBuffer>(file, &file->audio) : nullptr);
    }

private:
    struct UndoStep {
        bool isState = false;
        InstanceId id = 0;
        uint32_t index = 0;
        float oldValue = 0.0f, newValue = 0.0f;
        std::string oldState, newState;
        std::string label;
        std::vector<UndoStep> group; // a gesture: these steps undo and redo together
    };
    // The gesture in progress: its changes so far, merged per parameter.
    struct OpenGesture {
        bool open = false;
        InstanceId id = 0;
        std::string label;
        std::vector<UndoStep> steps;
    };

    bool engineLive() const { return engine_.isRunning(); }
    double transitionSeconds(double rampMs) const;
    void publish();
    void pushUndo(UndoStep step);
    void applyStep(const UndoStep& s, bool forward);
    InstanceRecord* findRecord(InstanceId id) { return find(id); }

    Engine& engine_;
    ModuleRegistry& registry_;
    std::vector<InstanceRecord> records_;
    std::vector<Placeholder> placeholders_;
    std::shared_ptr<TapRing> recordRing_;
    dsptb_tap recordTap_ = DSPTB_TAP_POST;
    AnalysisHost analysis_; // after records_: its threads stop before the records go
    std::vector<InstanceId>
        audioOrder_; // order of the snapshot last published (differs during a reorder dip)
    InstanceId nextId_ = 1;
    double now_ = 0.0;
    bool reorderPending_ = false;
    double reorderAt_ = 0.0;
    std::deque<UndoStep> undo_, redo_;
    OpenGesture gesture_;
    WorkingFileSlot workingFile_;
};

} // namespace dsptb
