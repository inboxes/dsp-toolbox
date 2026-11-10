// dsptb-gpu-bench: times chains of ONNX Runtime and LibTorch stages on the GPU with the production stage classes, so the
// numbers compare with the Python measurements (docs/GPU_RUNTIME.md section 9.3).
//
//   dsptb-gpu-bench --config chain.json [--iterations 300] [--warmup 30] [--graphs on|off|both] [--sync spin|blocking]
//                   [--paced-hop-ms 40 --seconds 25 --budget-ms 25] [--gpu-load 0..100] [--json]
//
// chain.json lists the stages in order. Glue between stages (a pitch decode, resampling) is out of scope: every stage
// gets its own fixed input, so this measures the runtime, not a module.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dsptb/gpu/cuda_util.hpp>
#include <dsptb/gpu/ort_stage.hpp>
#include <dsptb/gpu/runtime_api.hpp>
#include <dsptb/gpu/torch_stage.hpp>
#include <dsptb/gpu/worker.hpp>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gpu_load.hpp"

using nlohmann::json;
using namespace dsptb::gpu;

namespace {

// Deterministic noise (seed 1), the same on every run and every machine.
class Lcg {
public:
    double next() { // uniform in [0, 1)
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state_ >> 11) / 9007199254740992.0;
    }

private:
    uint64_t state_ = 1;
};

struct InputSpec {
    TensorSpec tensor;
    bool hasConst = false;
    double constant = 0;
    double lo = -1, hi = 1; // uniform range
};

TensorSpec parseTensor(const json& j, bool isInput, InputSpec* in) {
    TensorSpec t;
    t.name = j.value("name", std::string());
    t.shape = j.at("shape").get<std::vector<int64_t>>();
    t.type = j.value("type", std::string("float32")) == "int64" ? TensorType::Int64 : TensorType::Float32;
    if (isInput && in) {
        in->tensor = t;
        if (j.contains("fill")) {
            const json& f = j["fill"];
            if (f.contains("const")) {
                in->hasConst = true;
                in->constant = f["const"].get<double>();
            }
            if (f.contains("range")) {
                in->lo = f["range"][0].get<double>();
                in->hi = f["range"][1].get<double>();
            }
        }
    }
    return t;
}

void fillInput(void* dst, const InputSpec& in, Lcg& rng) {
    const size_t n = in.tensor.elements();
    if (in.tensor.type == TensorType::Float32) {
        float* p = static_cast<float*>(dst);
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<float>(in.hasConst ? in.constant : in.lo + (in.hi - in.lo) * rng.next());
    } else {
        int64_t* p = static_cast<int64_t*>(dst);
        for (size_t i = 0; i < n; ++i)
            p[i] = in.hasConst ? static_cast<int64_t>(in.constant) : static_cast<int64_t>(in.lo + (in.hi - in.lo) * rng.next());
    }
}

// One stage of the chain, whichever backend.
class Runner {
public:
    virtual ~Runner() = default;
    virtual void prepare(CudaStream& stream) = 0;
    virtual void run(CudaStream& stream) = 0;
    virtual bool graphed() const = 0;
    virtual void teardown() = 0;
    std::string name, backend;
};

class OrtRunner : public Runner {
public:
    OrtRunner(const std::string& path, std::vector<InputSpec> ins, std::vector<TensorSpec> outs, bool graph) : ins_(std::move(ins)) {
        std::vector<TensorSpec> inSpecs;
        for (const InputSpec& i : ins_) inSpecs.push_back(i.tensor);
        OrtOptions o;
        o.graph = graph;
        model_ = std::make_unique<OrtModel>(path, inSpecs, std::move(outs), o);
    }
    void prepare(CudaStream& s) override {
        model_->prepare(s);
        Lcg rng;
        for (size_t i = 0; i < ins_.size(); ++i) fillInput(model_->hostInput(i), ins_[i], rng);
    }
    void run(CudaStream& s) override { model_->run(s); }
    bool graphed() const override { return model_->graphed(); }
    void teardown() override { model_.reset(); }

private:
    std::vector<InputSpec> ins_;
    std::unique_ptr<OrtModel> model_;
};

class TorchRunner : public Runner {
public:
    TorchRunner(const std::string& path, std::vector<InputSpec> ins, bool graph, bool loadOnDevice) : ins_(std::move(ins)) {
        std::vector<TensorSpec> inSpecs;
        for (const InputSpec& i : ins_) inSpecs.push_back(i.tensor);
        TorchOptions o;
        o.graph = graph;
        o.loadOnDevice = loadOnDevice;
        model_ = std::make_unique<TorchModel>(path, inSpecs, o);
    }
    void prepare(CudaStream& s) override {
        // run() copies the host buffers to the device every time, so filling them after prepare() is enough.
        model_->prepare(s);
        Lcg rng;
        for (size_t i = 0; i < ins_.size(); ++i) fillInput(model_->hostInput(i), ins_[i], rng);
    }
    void run(CudaStream& s) override { model_->run(s); }
    bool graphed() const override { return model_->graphed(); }
    void teardown() override { model_.reset(); }

private:
    std::vector<InputSpec> ins_;
    std::unique_ptr<TorchModel> model_;
};

struct StageConfig {
    std::string name, backend, model;
    std::vector<InputSpec> inputs;
    std::vector<TensorSpec> outputs;
    bool graph = true;
    bool loadOnDevice = false; // torch only, see TorchOptions::loadOnDevice
};

std::vector<StageConfig> readConfig(const std::filesystem::path& path, std::string& chainName) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path.string());
    const json j = json::parse(f);
    chainName = j.value("name", path.stem().string());
    std::vector<StageConfig> stages;
    for (const json& s : j.at("stages")) {
        StageConfig c;
        c.name = s.at("name").get<std::string>();
        c.backend = s.at("backend").get<std::string>();
        c.model = s.at("model").get<std::string>();
        c.graph = s.value("graph", true);
        c.loadOnDevice = s.value("load_on_device", false);
        for (const json& in : s.at("inputs")) {
            InputSpec spec;
            parseTensor(in, true, &spec);
            c.inputs.push_back(std::move(spec));
        }
        if (s.contains("outputs"))
            for (const json& out : s["outputs"]) c.outputs.push_back(parseTensor(out, false, nullptr));
        if (c.backend != "ort" && c.backend != "torch") throw std::runtime_error("stage " + c.name + ": backend must be ort or torch");
        if (c.backend == "ort" && c.outputs.empty()) throw std::runtime_error("stage " + c.name + ": an ort stage needs its outputs (name and shape)");
        stages.push_back(std::move(c));
    }
    return stages;
}

struct Summary {
    double median = 0, p95 = 0, p99 = 0, max = 0;
};

Summary summarize(std::vector<double> v) {
    Summary s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto pick = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
    s.median = pick(0.5);
    s.p95 = pick(0.95);
    s.p99 = pick(0.99);
    s.max = v.back();
    return s;
}

double processSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto sec = [](const FILETIME& f) {
        ULARGE_INTEGER x;
        x.LowPart = f.dwLowDateTime;
        x.HighPart = f.dwHighDateTime;
        return static_cast<double>(x.QuadPart) * 1e-7;
    };
    return sec(k) + sec(u);
}

struct Options {
    int iterations = 300, warmup = 30;
    double pacedHopMs = 0, seconds = 25, budgetMs = 0;
    bool asJson = false;
};

struct Result {
    std::string label; // "graphs on" etc.
    std::vector<std::string> names;
    std::vector<bool> graphed;
    std::vector<Summary> perStage;
    Summary total;
    double cpuPercent = 0;
    size_t samples = 0;
    size_t misses = 0; // paced only: iterations whose compute exceeded the budget
    std::string error;
};

Result runChain(const std::vector<StageConfig>& stages, const std::string& label, int graphMode, int device, const Options& opt, int gpuLoad) {
    Result r;
    r.label = label;
    std::vector<std::unique_ptr<Runner>> runners;
    try {
        CudaStream stream(device);
        for (const StageConfig& c : stages) {
            const bool graph = graphMode < 0 ? c.graph : graphMode == 1;
            std::unique_ptr<Runner> run;
            if (c.backend == "ort") run = std::make_unique<OrtRunner>(c.model, c.inputs, c.outputs, graph);
            else run = std::make_unique<TorchRunner>(c.model, c.inputs, graph, c.loadOnDevice);
            run->name = c.name;
            run->backend = c.backend;
            run->prepare(stream);
            r.names.push_back(c.name + " (" + c.backend + ")");
            r.graphed.push_back(run->graphed());
            runners.push_back(std::move(run));
        }
        // The background load starts only now: loading models and capturing graphs must not overlap another thread's CUDA
        // work (a graph capture in global mode forbids it), and the real competitor, a game, is another process anyway.
        std::unique_ptr<GpuLoad> load;
        if (gpuLoad > 0) {
            load = std::make_unique<GpuLoad>(device, gpuLoad);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        const CaptureLock lock = runtimeCaptureLock();
        std::vector<std::vector<double>> per(runners.size());
        std::vector<double> total;
        auto iteration = [&](bool record) {
            double sum = 0;
            for (size_t i = 0; i < runners.size(); ++i) {
                if (lock.lockShared) lock.lockShared();
                const int64_t t0 = nowMicros();
                runners[i]->run(stream);
                const double ms = static_cast<double>(nowMicros() - t0) / 1000.0;
                if (lock.unlockShared) lock.unlockShared();
                sum += ms;
                if (record) per[i].push_back(ms);
            }
            if (record) total.push_back(sum);
            return sum;
        };
        double cpu0 = 0, wall0 = 0;
        auto wall = [] { return static_cast<double>(nowMicros()) / 1e6; };
        if (opt.pacedHopMs <= 0) {
            for (int i = 0; i < opt.warmup; ++i) iteration(false);
            cpu0 = processSeconds();
            wall0 = wall();
            for (int i = 0; i < opt.iterations; ++i) iteration(true);
        } else {
            // One iteration per hop on a high-resolution timer, like the real worker. Five seconds of warm-up first.
            HighResTimer timer;
            const double hop = opt.pacedHopMs / 1000.0;
            const double start = wall();
            const double warmEnd = start + 5.0, end = warmEnd + opt.seconds;
            double next = start;
            bool started = false;
            while (next < end) {
                const bool record = next >= warmEnd;
                if (record && !started) {
                    started = true;
                    cpu0 = processSeconds();
                    wall0 = wall();
                }
                const double ms = iteration(record);
                if (record && opt.budgetMs > 0 && ms > opt.budgetMs) ++r.misses;
                next += hop;
                const double wait = next - wall();
                if (wait > 0.0005) timer.wait(static_cast<int64_t>(wait * 1e6));
                while (wall() < next) std::this_thread::yield();
            }
        }
        const double wallSec = wall() - wall0;
        r.cpuPercent = wallSec > 0 ? 100.0 * (processSeconds() - cpu0) / wallSec : 0;
        for (auto& v : per) r.perStage.push_back(summarize(v));
        r.total = summarize(total);
        r.samples = total.size();
        for (auto& run : runners) run->teardown();
    } catch (const std::exception& e) {
        r.error = e.what();
        for (auto& run : runners) {
            try { run->teardown(); } catch (...) {}
        }
    }
    return r;
}

void print(const std::string& chain, const Result& r, const Options& opt) {
    std::printf("\n%s, %s: ", chain.c_str(), r.label.c_str());
    if (!r.error.empty()) {
        std::printf("FAILED: %s\n", r.error.c_str());
        return;
    }
    std::printf("%zu iterations%s\n", r.samples, opt.pacedHopMs > 0 ? (", paced at " + std::to_string(opt.pacedHopMs) + " ms").c_str() : "");
    std::printf("  %-26s %8s %8s %8s %8s  %s\n", "stage", "median", "p95", "p99", "max", "graphed");
    for (size_t i = 0; i < r.names.size(); ++i)
        std::printf("  %-26s %8.2f %8.2f %8.2f %8.2f  %s\n", r.names[i].c_str(), r.perStage[i].median, r.perStage[i].p95, r.perStage[i].p99,
                    r.perStage[i].max, r.graphed[i] ? "yes" : "no");
    std::printf("  %-26s %8.2f %8.2f %8.2f %8.2f  (ms; process CPU %.0f %% of one core)\n", "TOTAL", r.total.median, r.total.p95, r.total.p99,
                r.total.max, r.cpuPercent);
    if (opt.pacedHopMs > 0 && opt.budgetMs > 0)
        std::printf("  budget %.1f ms: %zu of %zu hops over it\n", opt.budgetMs, r.misses, r.samples);
}

json toJson(const std::string& chain, const Result& r, const Options& opt) {
    json j = {{"chain", chain}, {"label", r.label}, {"error", r.error}, {"samples", r.samples}, {"cpuPercentOfOneCore", r.cpuPercent}};
    auto s = [](const Summary& x) { return json{{"median", x.median}, {"p95", x.p95}, {"p99", x.p99}, {"max", x.max}}; };
    j["stages"] = json::array();
    for (size_t i = 0; i < r.names.size(); ++i) {
        json st = s(r.perStage[i]);
        st["name"] = r.names[i];
        st["graphed"] = static_cast<bool>(r.graphed[i]);
        j["stages"].push_back(st);
    }
    j["total"] = s(r.total);
    if (opt.pacedHopMs > 0) {
        j["pacedHopMs"] = opt.pacedHopMs;
        j["budgetMs"] = opt.budgetMs;
        j["misses"] = r.misses;
    }
    return j;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"dsptb-gpu-bench: times chains of ORT and LibTorch stages on the GPU"};
    std::string configPath, graphs = "config", sync = "blocking", runtimeDir;
    Options opt;
    int gpuLoad = 0, device = 0;
    app.add_option("--config", configPath, "Chain description (JSON)")->required();
    app.add_option("--iterations", opt.iterations, "Timed iterations (not paced)")->capture_default_str();
    app.add_option("--warmup", opt.warmup, "Warm-up iterations (not paced)")->capture_default_str();
    app.add_option("--graphs", graphs, "on, off, both, or config (use each stage's own setting)")->capture_default_str();
    app.add_option("--sync", sync, "spin or blocking")->capture_default_str();
    app.add_option("--device", device, "CUDA device index")->capture_default_str();
    app.add_option("--paced-hop-ms", opt.pacedHopMs, "Run one iteration per hop on a high-resolution timer");
    app.add_option("--seconds", opt.seconds, "Paced run length after 5 s of warm-up")->capture_default_str();
    app.add_option("--budget-ms", opt.budgetMs, "Paced: count iterations whose compute exceeds this");
    app.add_option("--gpu-load", gpuLoad, "Keep the GPU about N % busy from another thread (0 = off)")->capture_default_str();
    app.add_option("--runtime-dir", runtimeDir, "The gpu-runtime folder (default: next to this executable's folder)");
    app.add_flag("--json", opt.asJson, "Print JSON");
    CLI11_PARSE(app, argc, argv);

    try {
        std::filesystem::path dir = runtimeDir;
        if (dir.empty()) {
            wchar_t exe[MAX_PATH * 2];
            const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
            dir = std::filesystem::path(std::wstring(exe, n)).parent_path().parent_path(); // <runtime>/bin/this.exe
        }
        dsptb_gpu_runtime_info info{};
        if (loadRuntime(dir, device, sync != "spin", info) != DSPTB_GPU_OK) {
            std::fprintf(stderr, "GPU runtime not available: %s\n", info.status_text);
            return 1;
        }
        if (!opt.asJson) std::printf("%s\n", info.status_text);
        std::string chainName;
        const std::vector<StageConfig> stages = readConfig(configPath, chainName);
        std::vector<std::pair<std::string, int>> modes;
        if (graphs == "on") modes = {{"graphs on", 1}};
        else if (graphs == "off") modes = {{"graphs off", 0}};
        else if (graphs == "both") modes = {{"graphs off", 0}, {"graphs on", 1}};
        else modes = {{"graphs as configured", -1}};
        json all = json::array();
        bool failed = false;
        for (const auto& [label, mode] : modes) {
            const Result r = runChain(stages, label + (gpuLoad > 0 ? ", gpu load " + std::to_string(gpuLoad) + " %" : ""), mode, selectedDevice(), opt, gpuLoad);
            failed = failed || !r.error.empty();
            if (opt.asJson) all.push_back(toJson(chainName, r, opt));
            else print(chainName, r, opt);
        }
        if (opt.asJson) std::printf("%s\n", all.dump(2).c_str());
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
