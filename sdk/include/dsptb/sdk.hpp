#pragma once

// C++ SDK for DSP Toolbox modules (MODULE_SDK §5). Header-only. Include this one header; it pulls in the
// C ABI, parameter builders, the canvas wrapper, the real-time primitives and the dsp helpers.
//
//   class Gain final : public dsptb::Processor<Gain> { ... };
//   DSPTB_DEFINE_RT_GUARD();        // once per module, in one .cpp (see rt/rt_guard.hpp)
//   DSPTB_REGISTER_MODULES(Gain)    // generates dsptb_module_count / dsptb_module_get

#include <cstddef>

#include "canvas.hpp"
#include "dsp/db.hpp"
#include "dsp/window.hpp"
#include "params.hpp"
#include "plugin_abi.h"
#include "rt/rt_guard.hpp"
#include "rt/rt_swap.hpp"
#include "rt/smoothed_value.hpp"
#include "rt/spsc_ring.hpp"
#include "rt/triple_buffer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall RaiseException(unsigned long code, unsigned long flags,
                                                               unsigned long nargs, const void* args);
#endif

namespace dsptb {

// ---------------------------------------------------------------------------------------------------------
// value types handed to module code
// ---------------------------------------------------------------------------------------------------------

struct ModuleInfo {
    const char* id = "";
    const char* name = "";
    const char* version = "0.0.0";
    const char* vendor = "";
    const char* description = "";
    uint32_t caps = 0;
    dsptb_tap defaultTap = DSPTB_TAP_POST; // analyzers
};

struct PrepareInfo {
    double sampleRate = 48000.0;
    uint32_t maxBlockFrames = 0;
    uint32_t numChannels = 2;
    bool offline = false;
};

// Planar channel pointers; iterates as float* so `for (auto ch : b.channels())` works.
struct ChannelRange {
    float* const* first = nullptr;
    uint32_t count = 0;
    float* const* begin() const { return first; }
    float* const* end() const { return first + count; }
};

class ProcessBlock {
public:
    explicit ProcessBlock(const dsptb_process_ctx* ctx) : ctx_(ctx) {}
    uint32_t frames() const { return ctx_->num_frames; }
    uint32_t numChannels() const { return ctx_->num_channels; }
    float* channel(uint32_t i) const { return ctx_->channels[i]; }
    ChannelRange channels() const { return {ctx_->channels, ctx_->num_channels}; }
    // Parameter values: descriptor order. Int, bool and enum values are floats rounded on read.
    float param(uint32_t i) const { return ctx_->params[i]; }
    int paramInt(uint32_t i) const {
        return static_cast<int>(ctx_->params[i] + (ctx_->params[i] < 0 ? -0.5f : 0.5f));
    }
    bool paramBool(uint32_t i) const { return ctx_->params[i] >= 0.5f; }
    const dsptb_transport& transport() const { return *ctx_->transport; }

private:
    const dsptb_process_ctx* ctx_;
};

class AnalyzeBlock {
public:
    explicit AnalyzeBlock(const dsptb_analyze_ctx* ctx) : ctx_(ctx) {}
    uint32_t frames() const { return ctx_->num_frames; }
    uint32_t numChannels() const { return ctx_->num_channels; }
    const float* channel(uint32_t i) const { return ctx_->channels[i]; }
    double sampleRate() const { return ctx_->sample_rate; }
    uint64_t firstFrameClock() const { return ctx_->first_frame_clock; }
    uint32_t droppedFrames() const { return ctx_->dropped_frames; }
    float param(uint32_t i) const { return ctx_->params[i]; }
    dsptb_tap tap() const { return ctx_->tap; }

private:
    const dsptb_analyze_ctx* ctx_;
};

class OfflineBlock {
public:
    explicit OfflineBlock(const dsptb_offline_ctx* ctx) : ctx_(ctx) {}
    uint32_t numChannels() const { return ctx_->num_channels; }
    uint64_t frames() const { return ctx_->num_frames; }
    const float* channel(uint32_t i) const { return ctx_->channels[i]; }
    double sampleRate() const { return ctx_->sample_rate; }
    float param(uint32_t i) const { return ctx_->params[i]; }
    void reportProgress(float f) const { ctx_->report_progress(ctx_->host_token, f); }
    bool cancelled() const { return ctx_->is_cancelled(ctx_->host_token) != 0; }

private:
    const dsptb_offline_ctx* ctx_;
};

// Wraps dsptb_host_api (UI thread unless noted).
class Host;

// Holds a view of the working file and releases it when it goes out of scope. Never on the audio thread.
class WorkingFile;

class Host {
public:
    Host() = default;
    explicit Host(const dsptb_host_api* api) : api_(api) {}
    bool valid() const { return api_ != nullptr; }
    void log(dsptb_log_level level, const char* utf8) const { // not on the audio thread
        if (api_ && api_->log)
            api_->log(api_->host, api_->instance_token, level, utf8);
    }
    float getParam(uint32_t index) const {
        return (api_ && api_->get_param) ? api_->get_param(api_->host, api_->instance_token, index) : 0.0f;
    }
    void setParam(uint32_t index, float v) const { // creates an undo step
        if (api_ && api_->set_param)
            api_->set_param(api_->host, api_->instance_token, index, v);
    }
    void markStateDirty(const char* undoLabel) const {
        if (api_ && api_->mark_state_dirty)
            api_->mark_state_dirty(api_->host, api_->instance_token, undoLabel);
    }
    const char* moduleDir() const { return api_ ? api_->module_dir_utf8 : ""; }
    // Working file (0.3.0 hosts). Older hosts don't have it: generation 0, and acquire fails.
    bool hasWorkingFileApi() const {
        return api_ && api_->struct_size >= offsetof(dsptb_host_api, release_working_file) + sizeof(void*);
    }
    uint64_t workingFileGeneration() const { // any thread, the audio thread included
        return hasWorkingFileApi() ? api_->working_file_generation(api_->host) : 0;
    }
    bool acquireWorkingFile(dsptb_file_view& view) const { // never on the audio thread
        view = dsptb_file_view{};
        view.struct_size = static_cast<uint32_t>(sizeof(view));
        return hasWorkingFileApi() && api_->acquire_working_file(api_->host, api_->instance_token, &view) != 0;
    }
    void releaseWorkingFile(dsptb_file_view& view) const {
        if (hasWorkingFileApi() && view.handle)
            api_->release_working_file(api_->host, api_->instance_token, &view);
    }
    const char* hostVersion() const { return api_ ? api_->host_version : ""; }
    const dsptb_host_api* raw() const { return api_; }

private:
    const dsptb_host_api* api_ = nullptr;
};

class WorkingFile {
public:
    explicit WorkingFile(const Host& host) : host_(host) { ok_ = host_.acquireWorkingFile(view_); }
    ~WorkingFile() {
        if (ok_)
            host_.releaseWorkingFile(view_);
    }
    WorkingFile(const WorkingFile&) = delete;
    WorkingFile& operator=(const WorkingFile&) = delete;
    explicit operator bool() const { return ok_; }
    const dsptb_file_view& view() const { return view_; }
    uint64_t frames() const { return ok_ ? view_.num_frames : 0; }
    const float* channel(uint32_t i) const { return view_.channels[i < view_.num_channels ? i : 0]; }

private:
    Host host_;
    dsptb_file_view view_{};
    bool ok_ = false;
};

// ---------------------------------------------------------------------------------------------------------
// module base classes
// ---------------------------------------------------------------------------------------------------------

class ModuleBase {
public:
    virtual ~ModuleBase() = default;
    void bindHost(const dsptb_host_api* api) { host_ = Host(api); }

    virtual bool prepare(const PrepareInfo&) { return true; }
    virtual void draw(Canvas&) {}
    // State is a JSON object (see state_json.hpp). Default: no state.
    virtual std::string saveState() { return "{}"; }
    virtual bool loadState(std::string_view) { return true; }
    // Analyzers: a JSON document of the frames/readings produced so far, for `dsptb-cli analyze
    // --dump-frames`. Called from the thread that fed analyze(), after it has finished. Empty = no dump.
    virtual std::string dumpFrames() { return {}; }

protected:
    const Host& host() const { return host_; }

private:
    Host host_;
};

class ProcessorBase : public ModuleBase {
public:
    virtual void reset() {}                              // audio thread, RT-safe
    virtual uint32_t latencyFrames() const { return 0; } // constant after prepare
    virtual uint32_t tailFrames() const { return 0; }
    virtual void process(ProcessBlock& block) = 0; // audio thread, RT-safe, in place
    static constexpr dsptb_kind kind = DSPTB_KIND_PROCESSOR;
};

class AnalyzerBase : public ModuleBase {
public:
    virtual void analyze(AnalyzeBlock& block) = 0;
    static constexpr dsptb_kind kind = DSPTB_KIND_ANALYZER;
};

class OfflineJobBase : public ModuleBase {
public:
    virtual bool runOffline(OfflineBlock& job) = 0;
    static constexpr dsptb_kind kind = DSPTB_KIND_OFFLINE;
};

// CRTP tags: `class Gain final : public dsptb::Processor<Gain>`. T supplies `info` and `params`.
template <class T> class Processor : public ProcessorBase {};
template <class T> class Analyzer : public AnalyzerBase {};
template <class T> class OfflineJob : public OfflineJobBase {};

// ---------------------------------------------------------------------------------------------------------
// C trampolines. C++ exceptions never cross the boundary: they are caught here. Because process() cannot
// return an error, a caught exception is re-raised as a structured exception that the host's SEH wrapper
// turns into "module Faulted", after the block has been silenced.
// ---------------------------------------------------------------------------------------------------------

namespace detail {

constexpr unsigned long kFaultCode = 0xE0445350UL; // 'DSP' + 0x50: "module raised a C++ exception"

[[noreturn]] inline void raiseModuleFault() {
#if defined(_WIN32)
    RaiseException(kFaultCode, 0, 0, nullptr);
#endif
    std::abort();
}

template <class T> struct Trampolines {
    using Base = ModuleBase;
    static Base* base(void* self) { return static_cast<Base*>(self); }

    static void* create(const dsptb_host_api* host) {
        try {
            T* m = new T();
            m->bindHost(host);
            return static_cast<Base*>(m);
        } catch (...) {
            return nullptr;
        }
    }
    static void destroy(void* self) {
        try {
            delete base(self);
        } catch (...) {
        }
    }
    static int prepare(void* self, const dsptb_prepare_info* info) {
        try {
            PrepareInfo p;
            p.sampleRate = info->sample_rate;
            p.maxBlockFrames = info->max_block_frames;
            p.numChannels = info->num_channels;
            p.offline = info->offline != 0;
            return base(self)->prepare(p) ? 0 : 1;
        } catch (...) {
            return 2;
        }
    }
    static void reset(void* self) {
        rt::RtScope scope;
        try {
            static_cast<ProcessorBase*>(self)->reset();
        } catch (...) {
            raiseModuleFault();
        }
    }
    static uint32_t latencyFrames(void* self) {
        try {
            return static_cast<ProcessorBase*>(self)->latencyFrames();
        } catch (...) {
            return 0;
        }
    }
    static uint32_t tailFrames(void* self) {
        try {
            return static_cast<ProcessorBase*>(self)->tailFrames();
        } catch (...) {
            return 0;
        }
    }
    static void process(void* self, const dsptb_process_ctx* ctx) {
        rt::RtScope scope; // the module's own debug allocation guard (each DLL has its own operator new)
        bool failed = false;
        try {
            ProcessBlock block(ctx);
            static_cast<ProcessorBase*>(self)->process(block);
        } catch (...) {
            failed = true;
        }
        if (failed) {
            for (uint32_t c = 0; c < ctx->num_channels; ++c)
                std::memset(ctx->channels[c], 0, sizeof(float) * ctx->num_frames);
            raiseModuleFault();
        }
    }
    static void analyze(void* self, const dsptb_analyze_ctx* ctx) {
        try {
            AnalyzeBlock block(ctx);
            static_cast<AnalyzerBase*>(self)->analyze(block);
        } catch (...) {
            raiseModuleFault();
        }
    }
    static int runOffline(void* self, const dsptb_offline_ctx* ctx) {
        try {
            OfflineBlock job(ctx);
            return static_cast<OfflineJobBase*>(self)->runOffline(job) ? 0 : 1;
        } catch (...) {
            return 2;
        }
    }
    static void draw(void* self, const dsptb_canvas_api* api) {
        try {
            Canvas canvas(api);
            base(self)->draw(canvas);
        } catch (...) {
        }
    }
    static int saveState(void* self, void (*write)(void*, const char*, size_t), void* tok) {
        try {
            const std::string s = base(self)->saveState();
            write(tok, s.data(), s.size());
            return 0;
        } catch (...) {
            return 1;
        }
    }
    static int loadState(void* self, const char* utf8, size_t len) {
        try {
            return base(self)->loadState(std::string_view(utf8, len)) ? 0 : 1;
        } catch (...) {
            return 1;
        }
    }

    static dsptb_module_desc makeDesc() {
        dsptb_module_desc d{};
        d.struct_size = sizeof(d);
        d.abi_version = DSPTB_ABI_VERSION;
        d.id = T::info.id;
        d.name = T::info.name;
        d.version = T::info.version;
        d.vendor = T::info.vendor;
        d.description = T::info.description;
        d.kind = T::kind;
        d.caps = T::info.caps;
        d.default_tap = T::info.defaultTap;
        d.params = T::params.data();
        d.num_params = static_cast<uint32_t>(T::params.size());
        d.create = &create;
        d.destroy = &destroy;
        d.prepare = &prepare;
        d.draw = &draw;
        d.save_state = &saveState;
        d.load_state = &loadState;
        if constexpr (std::is_base_of_v<ProcessorBase, T>) {
            d.reset = &reset;
            d.latency_frames = &latencyFrames;
            d.tail_frames = &tailFrames;
            d.process = &process;
        } else if constexpr (std::is_base_of_v<AnalyzerBase, T>) {
            d.analyze = &analyze;
        } else {
            d.run_offline = &runOffline;
        }
        return d;
    }
};

// Backs the optional `dsptb_dump_frames` export (not part of plugin_abi.h; found with GetProcAddress like the
// allocation-guard counters). Returns 0 with the JSON written, 1 when the module has nothing to dump.
inline int dumpFramesExport(void* self, void (*write)(void*, const char*, size_t), void* tok) {
    try {
        const std::string s = static_cast<ModuleBase*>(self)->dumpFrames();
        if (s.empty())
            return 1;
        write(tok, s.data(), s.size());
        return 0;
    } catch (...) {
        return 2;
    }
}

template <class T> const dsptb_module_desc* descriptorOf() {
    static const dsptb_module_desc desc = Trampolines<T>::makeDesc();
    return &desc;
}

template <class... Ts> struct ModuleList {
    static uint32_t count() { return static_cast<uint32_t>(sizeof...(Ts)); }
    static const dsptb_module_desc* get(uint32_t index, uint32_t hostAbi) {
        if (hostAbi != DSPTB_ABI_VERSION)
            return nullptr; // refuse a host speaking a different ABI major version
        const dsptb_module_desc* all[] = {descriptorOf<Ts>()...};
        return index < sizeof...(Ts) ? all[index] : nullptr;
    }
};

} // namespace detail
} // namespace dsptb

// Generates the two required DLL exports, plus two optional debug exports the host and `dsptb-cli validate`
// use to read this DLL's allocation-guard counter (each DLL has its own, see rt_guard.hpp).
#define DSPTB_REGISTER_MODULES(...)                                                                          \
    extern "C" {                                                                                             \
    DSPTB_EXPORT uint32_t dsptb_module_count(void) {                                                         \
        return ::dsptb::detail::ModuleList<__VA_ARGS__>::count();                                            \
    }                                                                                                        \
    DSPTB_EXPORT const dsptb_module_desc* dsptb_module_get(uint32_t index, uint32_t host_abi_version) {      \
        return ::dsptb::detail::ModuleList<__VA_ARGS__>::get(index, host_abi_version);                       \
    }                                                                                                        \
    DSPTB_EXPORT uint32_t dsptb_rt_guard_violations(void) {                                                  \
        return ::dsptb::rt::rtGuardViolations();                                                             \
    }                                                                                                        \
    DSPTB_EXPORT void dsptb_rt_guard_reset(void) {                                                           \
        ::dsptb::rt::resetRtGuardViolations();                                                               \
    }                                                                                                        \
    DSPTB_EXPORT int dsptb_dump_frames(void* self, void (*write)(void*, const char*, size_t), void* tok) {   \
        return ::dsptb::detail::dumpFramesExport(self, write, tok);                                          \
    }                                                                                                        \
    }                                                                                                        \
    static_assert(true, "")
