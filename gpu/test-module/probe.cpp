// test.gpu-probe: runs a tiny network through the GPU pipeline so every part of docs/GPU_RUNTIME.md section 7 is
// exercised without a real model. y = 0.5 x, delayed by exactly latency_frames. A TEST_MODULE: never packaged.
#include <dsptb/gpu/cuda_util.hpp>
#include <dsptb/gpu/ort_stage.hpp>
#include <dsptb/gpu/pipeline.hpp>
#include <dsptb/gpu/runtime_api.hpp>
#include <dsptb/gpu/torch_stage.hpp>
#include <dsptb/sdk.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

DSPTB_DEFINE_RT_GUARD();

namespace {

// Wraps the real stage with the test hooks: an artificial load delay and a failure on request.
class ProbeStage : public dsptb::gpu::Stage {
public:
    ProbeStage(std::unique_ptr<dsptb::gpu::Stage> inner, int loadDelayMs, std::atomic<int>& failNext)
        : inner_(std::move(inner)), loadDelayMs_(loadDelayMs), failNext_(failNext) {}
    void prepare() override {
        if (loadDelayMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(loadDelayMs_));
        inner_->prepare();
    }
    void run(const float* window, uint32_t windowFrames, float* out, uint32_t hop) override {
        const int fail = failNext_.exchange(0);
        if (fail == 1) throw std::runtime_error("injected failure (fail_next)");
        if (fail == 2) {
            // A real CUDA error (an invalid copy), to prove the path from a dead call to a Failed pipeline.
            DSPTB_CUDA_CHECK(cudaMemcpy(nullptr, nullptr, 16, cudaMemcpyDeviceToDevice));
        }
        inner_->run(window, windowFrames, out, hop);
    }
    bool graphed() const override { return inner_->graphed(); }
    void teardown() override { inner_->teardown(); }

private:
    std::unique_ptr<dsptb::gpu::Stage> inner_;
    int loadDelayMs_;
    std::atomic<int>& failNext_;
};

} // namespace

class GpuProbe final : public dsptb::Processor<GpuProbe> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.gpu-probe",
                                            .name = "GPU probe (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description = "Runs a tiny network through the GPU pipeline (y = 0.5 x).",
                                            .caps = DSPTB_CAP_REALTIME | DSPTB_CAP_OFFLINE};
    static constexpr const char* kBackends[] = {"ORT", "Torch"};
    static constexpr const char* kHops[] = {"10", "20", "40"};
    static constexpr const char* kModels[] = {"Gain", "ManyKernels"};
    static constexpr auto params = dsptb::params(
        dsptb::Enum("backend", "Backend", kBackends), dsptb::Bool("graphs", "CUDA graphs", true),
        dsptb::Enum("hop_ms", "Hop (ms)", kHops, 1), dsptb::Enum("model", "Model", kModels),
        dsptb::Int("fail_next", "Fail the next run (test: 1 exception, 2 CUDA error)", "", 0, 2, 0),
        dsptb::Int("load_delay_ms", "Artificial load delay (ms, test)", "ms", 0, 10000, 0),
        dsptb::Int("budget_ms", "Compute budget (ms)", "ms", 5, 100, 25));
    enum Param : uint32_t { kBackend, kGraphs, kHop, kModel, kFailNext, kLoadDelay, kBudget };

    ~GpuProbe() override { pipe_.stop(); }

    // Shape-changing settings are read here only (docs/GPU_RUNTIME.md section 7.5, rule 5). The models are sized in frames,
    // so any sample rate works; the hop names are nominal (milliseconds at 48 kHz).
    bool prepare(const dsptb::PrepareInfo& p) override {
        pipe_.stop();
        using namespace dsptb::gpu;
        const int backend = static_cast<int>(host().getParam(kBackend) + 0.5f);
        const bool graphs = host().getParam(kGraphs) >= 0.5f;
        const uint32_t hopMs = kHopMs[std::min(2, std::max(0, static_cast<int>(host().getParam(kHop) + 0.5f)))];
        const bool many = static_cast<int>(host().getParam(kModel) + 0.5f) == 1;
        const int delay = static_cast<int>(host().getParam(kLoadDelay) + 0.5f);

        const uint32_t hop = hopMs * 48, context = 960;
        std::string base = std::string(host().moduleDir()) + "/assets/" + (many ? "many_kernels" : "gain") + "_h" + std::to_string(hopMs);
        std::unique_ptr<Stage> stage;
        if (backend == 0) {
            OrtOptions o;
            o.graph = graphs;
            stage = std::make_unique<OrtStage>(base + ".onnx", context + hop, hop, o);
        } else {
            TorchOptions o;
            o.graph = graphs;
            stage = std::make_unique<TorchStage>(base + ".ts", context + hop, hop, o);
        }
        failNext_.store(0);
        PipelineConfig cfg;
        cfg.sampleRate = p.sampleRate;
        cfg.hopFrames = hop;
        cfg.contextFrames = context;
        cfg.lookaheadFrames = 0;
        cfg.budgetFrames = static_cast<uint32_t>(host().getParam(kBudget) + 0.5f) * 48; // 25 ms by default: measured compute max 9 ms + 2 blocks + 1 ms is 15.5 ms, so 15 ms missed about once in ten seconds
        cfg.maxBlockFrames = p.maxBlockFrames;
        pipe_.setCaptureLock(runtimeCaptureLock());
        pipe_.configure(cfg, std::make_unique<ProbeStage>(std::move(stage), delay, failNext_), p.offline);
        mono_.assign(p.maxBlockFrames, 0.0f);
        wet_.assign(p.maxBlockFrames, 0.0f);
        pipe_.start();
        return true;
    }

    void reset() override { pipe_.reset(); }
    uint32_t latencyFrames() const override { return pipe_.latencyFrames(); }

    void process(dsptb::ProcessBlock& b) override {
        const uint32_t n = std::min<uint32_t>(b.frames(), static_cast<uint32_t>(mono_.size()));
        if (const int fail = b.paramInt(kFailNext)) failNext_.store(fail, std::memory_order_relaxed);
        const uint32_t channels = b.numChannels();
        for (uint32_t i = 0; i < n; ++i) {
            float sum = 0.0f;
            for (uint32_t c = 0; c < channels; ++c) sum += b.channel(c)[i];
            mono_[i] = channels ? sum / static_cast<float>(channels) : 0.0f;
        }
        pipe_.push(mono_.data(), n);
        pipe_.pull(wet_.data(), n, mono_.data());
        for (uint32_t c = 0; c < channels; ++c)
            for (uint32_t i = 0; i < n; ++i) b.channel(c)[i] = wet_[i];
    }

    // For tests: the pipeline's numbers as JSON (UI thread).
    std::string saveState() override {
        const auto s = pipe_.stats();
        nlohmann::json j;
        j["state"] = stateName();
        j["runs"] = s.runs;
        j["misses"] = s.misses;
        j["consecutiveMisses"] = s.consecutiveMisses;
        j["lastMissFrame"] = s.lastMissFrame;
        j["p50Us"] = s.computeP50Us;
        j["p99Us"] = s.computeP99Us;
        j["maxUs"] = s.computeMaxUs;
        j["tooSlow"] = s.tooSlow;
        j["graphed"] = s.graphed;
        j["latencyFrames"] = pipe_.latencyFrames();
        j["error"] = pipe_.lastError();
        return j.dump();
    }
    bool loadState(std::string_view) override { return true; }

    // draw() only reads stats() and state() (rule 2).
    void draw(dsptb::Canvas& c) override {
        const auto s = pipe_.stats();
        char buf[160];
        c.label((std::string("State: ") + stateName()).c_str());
        std::snprintf(buf, sizeof buf, "%s, runs %llu, misses %llu, compute p50 %.2f / p99 %.2f / max %.2f ms",
                      s.graphed ? "graphed" : "not graphed", static_cast<unsigned long long>(s.runs),
                      static_cast<unsigned long long>(s.misses), s.computeP50Us / 1000.0, s.computeP99Us / 1000.0,
                      s.computeMaxUs / 1000.0);
        c.label(buf);
        if (pipe_.state() == dsptb::gpu::State::Loading) c.label("Loading model...");
        if (s.tooSlow) c.label("GPU too slow for this latency setting");
        if (pipe_.state() == dsptb::gpu::State::Failed) c.label(("Failed: " + pipe_.lastError()).c_str());
    }

private:
    static constexpr uint32_t kHopMs[3] = {10, 20, 40};
    const char* stateName() const {
        switch (pipe_.state()) {
        case dsptb::gpu::State::Idle: return "Idle";
        case dsptb::gpu::State::Loading: return "Loading";
        case dsptb::gpu::State::Running: return "Running";
        case dsptb::gpu::State::Failed: return "Failed";
        }
        return "?";
    }

    dsptb::gpu::Pipeline pipe_;
    std::atomic<int> failNext_{0};
    std::vector<float> mono_, wet_;
};

DSPTB_REGISTER_MODULES(GpuProbe);
