#include "dsptb/core/module_instance.hpp"

#include "dsptb/core/seh_guard.hpp"
#include "dsptb/core/version.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsptb {

// ----------------------------------------------------------------------------------------------------------
// ParamBlock
// ----------------------------------------------------------------------------------------------------------

void ParamBlock::init(const dsptb_param_desc* descs, uint32_t count) {
    descs_ = descs;
    count_ = count;
    values_ = std::make_unique<std::atomic<float>[]>(count);
    rt_.assign(count, 0.0f);
    resetToDefaults();
    copyToRt();
}

void ParamBlock::resetToDefaults() {
    for (uint32_t i = 0; i < count_; ++i)
        values_[i].store(descs_[i].default_value, std::memory_order_relaxed);
}

int ParamBlock::indexOf(const std::string& id) const {
    for (uint32_t i = 0; i < count_; ++i)
        if (id == descs_[i].id)
            return static_cast<int>(i);
    return -1;
}

float ParamBlock::set(uint32_t i, float v) {
    if (i >= count_)
        return 0.0f;
    const dsptb_param_desc& d = descs_[i];
    if (!std::isfinite(v))
        v = d.default_value;
    v = std::clamp(v, d.min_value, d.max_value);
    if (d.type != DSPTB_PARAM_FLOAT)
        v = std::round(v);
    values_[i].store(v, std::memory_order_relaxed);
    return v;
}

// ----------------------------------------------------------------------------------------------------------
// host API callbacks handed to modules
// ----------------------------------------------------------------------------------------------------------

namespace {

ModuleInstance* inst(void* token) {
    return static_cast<ModuleInstance*>(token);
}

void hostLog(void*, void* token, dsptb_log_level level, const char* utf8) {
    ModuleInstance* m = inst(token);
    if (m && m->host()) {
        m->host()->moduleLog(*m, level, utf8 ? utf8 : "");
        return;
    }
    SimpleModuleHost().moduleLog(*m, level, utf8 ? utf8 : "");
}
float hostGetParam(void*, void* token, uint32_t index) {
    ModuleInstance* m = inst(token);
    return (m && index < m->params().size()) ? m->params().get(index) : 0.0f;
}
void hostSetParam(void*, void* token, uint32_t index, float v) {
    ModuleInstance* m = inst(token);
    if (!m || index >= m->params().size())
        return;
    if (m->host())
        m->host()->moduleSetParam(*m, index, v);
    else
        m->params().set(index, v);
}
void hostMarkDirty(void*, void* token, const char* label) {
    ModuleInstance* m = inst(token);
    if (m && m->host())
        m->host()->moduleMarkStateDirty(*m, label ? label : "");
}
void* hostGpuDevice(void*) {
    return nullptr; // no module in v1 asks for the GPU yet
}
void hostGpuNoop(void*) {}

} // namespace

void SimpleModuleHost::moduleLog(ModuleInstance& who, dsptb_log_level level, const char* utf8) {
    switch (level) {
    case DSPTB_LOG_DEBUG:
        spdlog::debug("[{}] {}", who.moduleId(), utf8);
        break;
    case DSPTB_LOG_INFO:
        spdlog::info("[{}] {}", who.moduleId(), utf8);
        break;
    case DSPTB_LOG_WARN:
        spdlog::warn("[{}] {}", who.moduleId(), utf8);
        break;
    default:
        spdlog::error("[{}] {}", who.moduleId(), utf8);
        break;
    }
}

// ----------------------------------------------------------------------------------------------------------
// ModuleInstance
// ----------------------------------------------------------------------------------------------------------

std::shared_ptr<ModuleInstance> ModuleInstance::create(InstanceId id, ModulePackage& pkg,
                                                       const dsptb_module_desc& desc, IModuleHost* host,
                                                       std::string& error) {
    std::shared_ptr<ModuleInstance> m(new ModuleInstance());
    m->id_ = id;
    m->moduleId_ = desc.id;
    m->pkg_ = &pkg;
    m->desc_ = &desc;
    m->host_ = host;
    const auto dir = pkg.folder.u8string();
    m->moduleDirUtf8_.assign(dir.begin(), dir.end());
    m->params_.init(desc.params, desc.num_params);

    dsptb_host_api& h = m->hostApi_;
    h.struct_size = sizeof(h);
    h.abi_version = DSPTB_ABI_VERSION;
    h.host = m.get();
    h.instance_token = m.get();
    h.host_version = versionString();
    h.module_dir_utf8 = m->moduleDirUtf8_.c_str();
    h.log = &hostLog;
    h.get_param = &hostGetParam;
    h.set_param = &hostSetParam;
    h.mark_state_dirty = &hostMarkDirty;
    h.gpu_d3d11_device = &hostGpuDevice;
    h.gpu_lock = &hostGpuNoop;
    h.gpu_unlock = &hostGpuNoop;

    void* self = nullptr;
    const bool ok = guarded([&] { self = desc.create(&m->hostApi_); });
    if (!ok || !self) {
        error = ok ? "the module's create() returned null" : "the module crashed in create()";
        m->self_ = nullptr; // nothing to destroy
        return nullptr;
    }
    m->self_ = self;
    return m;
}

ModuleInstance::~ModuleInstance() {
    if (self_ && desc_ && desc_->destroy) {
        void* self = self_;
        const auto destroy = desc_->destroy;
        guarded([&] { destroy(self); });
    }
}

bool ModuleInstance::prepare(double sampleRate, uint32_t maxBlockFrames, bool offline, std::string& error) {
    dsptb_prepare_info info{};
    info.struct_size = sizeof(info);
    info.sample_rate = sampleRate;
    info.max_block_frames = maxBlockFrames;
    info.num_channels = 2;
    info.offline = offline ? 1u : 0u;
    int rc = -1;
    if (!guarded([&] { rc = desc_->prepare(self_, &info); })) {
        markFaulted(FaultReason::Exception);
        error = "the module crashed in prepare()";
        return false;
    }
    if (rc != 0) {
        error = "prepare() failed (code " + std::to_string(rc) + ")";
        return false;
    }
    uint32_t latency = 0;
    if (desc_->kind == DSPTB_KIND_PROCESSOR && desc_->latency_frames) {
        if (!guarded([&] { latency = desc_->latency_frames(self_); })) {
            markFaulted(FaultReason::Exception);
            error = "the module crashed in latency_frames()";
            return false;
        }
    }
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlockFrames;
    latency_ = latency;
    rt.dryL.assign(maxBlockFrames, 0.0f);
    rt.dryR.assign(maxBlockFrames, 0.0f);
    rt.delayL.assign(latency, 0.0f);
    rt.delayR.assign(latency, 0.0f);
    rt.delayPos = 0;
    rt.rampStep = static_cast<float>(1.0 / (0.020 * sampleRate)); // 20 ms bypass crossfade
    rt.nonFiniteRun = 0;
    prepared_ = true;
    return true;
}

uint32_t ModuleInstance::queryLatencyFrames() const {
    uint32_t v = 0;
    if (desc_->latency_frames)
        guarded([&] { v = desc_->latency_frames(self_); });
    return v;
}

uint32_t ModuleInstance::tailFrames() const {
    if (!desc_->tail_frames)
        return 0;
    uint32_t t = 0;
    guarded([&] { t = desc_->tail_frames(self_); });
    return t;
}

bool ModuleInstance::saveState(std::string& json) const {
    if (!desc_->save_state)
        return false;
    json.clear();
    const auto writer = [](void* tok, const char* text, size_t len) {
        static_cast<std::string*>(tok)->append(text, len);
    };
    int rc = 1;
    if (!guarded([&] { rc = desc_->save_state(self_, writer, &json); }))
        return false;
    return rc == 0;
}

bool ModuleInstance::loadState(const std::string& json) {
    if (!desc_->load_state)
        return true;
    int rc = 1;
    if (!guarded([&] { rc = desc_->load_state(self_, json.data(), json.size()); })) {
        markFaulted(FaultReason::Exception);
        return false;
    }
    return rc == 0;
}

void ModuleInstance::draw(const dsptb_canvas_api* canvas) {
    if (!desc_->draw || faulted())
        return;
    if (!guarded([&] { desc_->draw(self_, canvas); }))
        markFaulted(FaultReason::Exception);
}

bool ModuleInstance::callProcess(float* l, float* r, uint32_t frames,
                                 const dsptb_transport& transport) noexcept {
    float* ch[2] = {l, r};
    dsptb_process_ctx ctx{};
    ctx.struct_size = sizeof(ctx);
    ctx.channels = ch;
    ctx.num_channels = 2;
    ctx.num_frames = frames;
    ctx.params = params_.rtValues();
    ctx.transport = &transport;
    return guarded([&] { desc_->process(self_, &ctx); });
}

bool ModuleInstance::callReset() noexcept {
    return guarded([&] { desc_->reset(self_); });
}

bool ModuleInstance::callAnalyze(const float* l, const float* r, uint32_t frames, uint64_t firstFrameClock,
                                 uint32_t droppedFrames, dsptb_tap tap) noexcept {
    const float* ch[2] = {l, r};
    params_.copyToRt(); // only the analysing thread writes rt_ for an analyzer
    dsptb_analyze_ctx ctx{};
    ctx.struct_size = sizeof(ctx);
    ctx.channels = ch;
    ctx.num_channels = 2;
    ctx.num_frames = frames;
    ctx.sample_rate = sampleRate_;
    ctx.first_frame_clock = firstFrameClock;
    ctx.dropped_frames = droppedFrames;
    ctx.params = params_.rtValues();
    ctx.tap = tap;
    return guarded([&] { desc_->analyze(self_, &ctx); });
}

bool ModuleInstance::dumpFrames(std::string& json) const {
    if (!pkg_->dumpFrames)
        return false;
    json.clear();
    const auto writer = [](void* tok, const char* text, size_t len) {
        static_cast<std::string*>(tok)->append(text, len);
    };
    int rc = 1;
    if (!guarded([&] { rc = pkg_->dumpFrames(self_, writer, &json); }))
        return false;
    return rc == 0;
}

} // namespace dsptb
