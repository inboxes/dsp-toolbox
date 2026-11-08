#pragma once

#include "audio_buffer.hpp"
#include "module_registry.hpp"

#include <dsptb/plugin_abi.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsptb {

using InstanceId = uint32_t;

// The block size an offline module is prepared with. It reads the whole file itself, so this is only a hint.
inline constexpr uint32_t kOfflineMaxBlock = 4096;

// Parameter values of one instance: the UI writes atomics, the audio thread copies them
// into a plain array at the start of each block and hands that to process(). Int, bool and enum values are
// stored as floats and kept rounded.
class ParamBlock {
public:
    void init(const dsptb_param_desc* descs, uint32_t count);
    uint32_t size() const { return count_; }
    const dsptb_param_desc& desc(uint32_t i) const { return descs_[i]; }
    int indexOf(const std::string& id) const; // -1 when unknown

    float get(uint32_t i) const { return values_[i].load(std::memory_order_relaxed); }
    // Clamps to the range and rounds discrete types. Returns the stored value.
    float set(uint32_t i, float v);
    float defaultValue(uint32_t i) const { return descs_[i].default_value; }
    void resetToDefaults();

    // audio thread
    void copyToRt() noexcept {
        for (uint32_t i = 0; i < count_; ++i)
            rt_[i] = values_[i].load(std::memory_order_relaxed);
    }
    const float* rtValues() const noexcept { return rt_.data(); }

private:
    const dsptb_param_desc* descs_ = nullptr;
    uint32_t count_ = 0;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::vector<float> rt_;
};

// What a module may ask of the host (the UI-thread half of dsptb_host_api). Implemented by SessionModel and
// by the simple hosts used offline and by `validate`.
class IModuleHost {
public:
    virtual ~IModuleHost() = default;
    virtual void moduleLog(class ModuleInstance& who, dsptb_log_level level, const char* utf8) = 0;
    virtual void moduleSetParam(class ModuleInstance& who, uint32_t index, float value) = 0;
    virtual void moduleMarkStateDirty(class ModuleInstance& who, const char* undoLabel) = 0;
    // The working file. The generation is read from any thread, the audio thread included; acquire runs on a
    // module's own threads. No file: generation 0 and a null pointer.
    virtual uint64_t workingFileGeneration() const { return 0; }
    virtual std::shared_ptr<const AudioBuffer> acquireWorkingFile(uint64_t& generation) {
        generation = 0;
        return {};
    }
    // Per-user folder for a module id, or empty when this host offers none. The instance creates it.
    virtual std::filesystem::path moduleDataDir(const std::string& moduleId) {
        (void)moduleId;
        return {};
    }
    // Param gestures: between begin and end, the instance's param and state changes form one undo step.
    virtual void moduleBeginGesture(class ModuleInstance& who, const char* undoLabel) {
        (void)who;
        (void)undoLabel;
    }
    virtual void moduleEndGesture(class ModuleInstance& who) { (void)who; }
};

// Holds the working file for a host: a lock for the pointer, an atomic for the generation the audio thread reads.
class WorkingFileSlot {
public:
    // A new pointer gets a new generation; null means no file (generation 0).
    void set(std::shared_ptr<const AudioBuffer> audio) {
        std::lock_guard lock(mutex_);
        if (audio == audio_)
            return;
        audio_ = std::move(audio);
        generation_.store(audio_ ? ++counter_ : 0, std::memory_order_release);
    }
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    std::shared_ptr<const AudioBuffer> acquire(uint64_t& generation) const {
        std::lock_guard lock(mutex_);
        generation = generation_.load(std::memory_order_relaxed);
        return audio_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const AudioBuffer> audio_;
    uint64_t counter_ = 0;
    std::atomic<uint64_t> generation_{0};
};

enum class FaultReason : int { None = 0, Exception = 1, NonFinite = 2, Create = 3 };

// One live module instance: the module's own object plus everything the host keeps next to it. Shared between
// chain snapshots with shared_ptr; destroyed (on whichever thread drops the last reference, in practice the
// UI thread during collectGarbage) only after no snapshot refers to it.
class ModuleInstance {
public:
    // Creates the module object (UI thread). Returns null with `error` on failure.
    static std::shared_ptr<ModuleInstance> create(InstanceId id, ModulePackage& pkg,
                                                  const dsptb_module_desc& desc, IModuleHost* host,
                                                  std::string& error);
    ~ModuleInstance();
    ModuleInstance(const ModuleInstance&) = delete;
    ModuleInstance& operator=(const ModuleInstance&) = delete;

    InstanceId id() const { return id_; }
    const std::string& moduleId() const { return moduleId_; }
    const dsptb_module_desc& desc() const { return *desc_; }
    ModulePackage& package() const { return *pkg_; }
    dsptb_kind kind() const { return desc_->kind; }
    ParamBlock& params() { return params_; }
    const ParamBlock& params() const { return params_; }
    void setHost(IModuleHost* host) { host_ = host; }
    IModuleHost* host() const { return host_; }
    const dsptb_host_api& hostApi() const { return hostApi_; }

    // ---- UI / worker thread ----
    // Prepares the module and sizes the host-side buffers. The instance must not be processing.
    bool prepare(double sampleRate, uint32_t maxBlockFrames, bool offline, std::string& error);
    bool prepared() const { return prepared_; }
    double sampleRate() const { return sampleRate_; }
    uint32_t maxBlockFrames() const { return maxBlock_; }
    uint32_t latencyFrames() const { return latency_; }
    // Asks the module again (for the conformance check that latency stays constant).
    uint32_t queryLatencyFrames() const;
    uint32_t tailFrames() const;
    bool saveState(std::string& json) const; // false if the module has no state or failed
    bool loadState(const std::string& json);
    void draw(const dsptb_canvas_api* canvas); // guarded; a crash faults the instance
    uint32_t rtGuardHits() const { return pkg_->guardViolations ? pkg_->guardViolations() : 0; }

    // ---- fault state (any thread) ----
    bool faulted() const { return fault_.load(std::memory_order_acquire) != 0; }
    FaultReason faultReason() const {
        return static_cast<FaultReason>(fault_.load(std::memory_order_acquire));
    }
    void markFaulted(FaultReason r) { fault_.store(static_cast<int>(r), std::memory_order_release); }

    // ---- audio thread (or the offline worker): all RT-safe ----
    // These wrap the module's callbacks in the SEH trampoline. They return false when the module faulted.
    bool callProcess(float* l, float* r, uint32_t frames, const dsptb_transport& transport) noexcept;
    bool callReset() noexcept;

    // ---- analysis thread (or the CLI): guarded like process(), but allocation is allowed ----
    // `ch` holds two planar channels of `frames` samples. Returns false when the module faulted. Parameter
    // values are snapshotted on every call.
    bool callAnalyze(const float* l, const float* r, uint32_t frames, uint64_t firstFrameClock,
                     uint32_t droppedFrames, dsptb_tap tap) noexcept;
    // ---- offline worker (or the CLI) ----
    // Runs an offline module over the whole buffer. `result` is run_offline's return value (0 = ok). Returns
    // false, and marks the instance faulted, when the module crashed. Both pointers must outlive the call.
    bool callRunOffline(const float* const* channels, uint32_t numChannels, uint64_t frames, double sampleRate,
                        std::atomic<float>* progress, const std::atomic<bool>* cancel, int& result) noexcept;

    // Optional frame dump for `analyze --dump-frames`; false when the module has none.
    bool dumpFrames(std::string& json) const;

    // State owned by the audio side: bypass ramp, dry-path delay and fault detection. Only the thread that
    // processes the chain may touch it.
    struct RtState {
        float wet = 0.0f;                  // 0 = fully bypassed, 1 = fully active; ramps over 20 ms
        float rampStep = 0.0f;             // per-sample change of `wet`
        uint32_t nonFiniteRun = 0;         // consecutive blocks with non-finite output
        std::vector<float> dryL, dryR;     // delayed dry copy of the block, max block size
        std::vector<float> delayL, delayR; // latency-sized delay lines (empty when latency == 0)
        uint32_t delayPos = 0;
    } rt;

private:
    ModuleInstance() = default;

    InstanceId id_ = 0;
    std::string moduleId_;
    std::string moduleDirUtf8_;
    std::string moduleDataDirUtf8_; // empty: the host offers no data folder (the API then passes NULL)
    ModulePackage* pkg_ = nullptr;
    const dsptb_module_desc* desc_ = nullptr;
    IModuleHost* host_ = nullptr;
    void* self_ = nullptr;
    dsptb_host_api hostApi_{};

public:
    // A working-file view a module holds: the file stays alive until the module releases it (or is destroyed).
    struct FileViewHold {
        std::shared_ptr<const AudioBuffer> audio;
        const float* channels[2] = {nullptr, nullptr};
    };
    std::mutex viewsMutex_;
    std::vector<std::unique_ptr<FileViewHold>> views_;

private:
    ParamBlock params_;
    bool prepared_ = false;
    double sampleRate_ = 0.0;
    uint32_t maxBlock_ = 0;
    uint32_t latency_ = 0;
    std::atomic<int> fault_{0};
};

// A host with no UI: logs through spdlog and applies set_param directly. Used offline and by `validate`.
class SimpleModuleHost final : public IModuleHost {
public:
    // Offline renders hand their input over as the working file, so detection runs on what is rendered.
    void setWorkingFile(std::shared_ptr<const AudioBuffer> audio) { file_.set(std::move(audio)); }
    uint64_t workingFileGeneration() const override { return file_.generation(); }
    std::shared_ptr<const AudioBuffer> acquireWorkingFile(uint64_t& generation) override {
        return file_.acquire(generation);
    }
    void moduleLog(ModuleInstance& who, dsptb_log_level level, const char* utf8) override;
    void moduleSetParam(ModuleInstance& who, uint32_t index, float value) override {
        who.params().set(index, value);
    }
    void moduleMarkStateDirty(ModuleInstance&, const char*) override {}
    // `validate` gives modules a data folder under this root; empty (the default) offers none.
    void setDataRoot(std::filesystem::path root) { dataRoot_ = std::move(root); }
    std::filesystem::path moduleDataDir(const std::string& moduleId) override {
        return dataRoot_.empty() ? std::filesystem::path() : dataRoot_ / std::filesystem::path(moduleId);
    }

private:
    WorkingFileSlot file_;
    std::filesystem::path dataRoot_;
};

} // namespace dsptb
