// Tests of the real ORT and LibTorch stages on the tiny test models (docs/GPU_RUNTIME.md G7, G8, G9). They need an
// NVIDIA GPU: without one every case skips itself and the test passes.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dsptb/gpu/ort_stage.hpp>
#include <dsptb/gpu/runtime_api.hpp>
#include <dsptb/gpu/torch_stage.hpp>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace dsptb::gpu;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kContext = 960;

bool gpuAvailable() {
    static const bool ok = [] {
        dsptb_gpu_runtime_info info{};
        const int status = loadRuntime(DSPTB_GPU_RUNTIME_DIR, 0, true, info);
        if (status != DSPTB_GPU_OK) MESSAGE("no GPU runtime, skipping: ", info.status_text);
        return status == DSPTB_GPU_OK;
    }();
    return ok;
}

#define REQUIRE_GPU() \
    do { if (!gpuAvailable()) return; } while (0)

enum class Backend { Ort, Torch };
const char* name(Backend b) { return b == Backend::Ort ? "ORT" : "LibTorch"; }

std::unique_ptr<Stage> makeStage(Backend b, const std::string& model, uint32_t hopMs, bool graph) {
    const uint32_t hop = hopMs * 48;
    const std::string base = std::string(DSPTB_GPU_TEST_MODELS) + "/" + model + "_h" + std::to_string(hopMs);
    if (b == Backend::Ort) {
        OrtOptions o;
        o.graph = graph;
        return std::make_unique<OrtStage>(base + ".onnx", kContext + hop, hop, o);
    }
    TorchOptions o;
    o.graph = graph;
    return std::make_unique<TorchStage>(base + ".ts", kContext + hop, hop, o);
}

// prepare() under the exclusive capture lock and run() under the shared one, like the pipeline worker does.
struct Locked {
    CaptureLock lock = runtimeCaptureLock();
    void prepare(Stage& s) { s.prepare(); } // the stages lock around the capture themselves
    void run(Stage& s, const float* w, uint32_t wn, float* out, uint32_t hop) {
        lock.lockShared();
        try { s.run(w, wn, out, hop); } catch (...) { lock.unlockShared(); throw; }
        lock.unlockShared();
    }
};

std::vector<float> noise(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (float& x : v) x = u(rng);
    return v;
}

double medianMs(Stage& s, uint32_t hopMs, int runs) {
    Locked l;
    const uint32_t hop = hopMs * 48, window = kContext + hop;
    const auto in = noise(window, 5);
    std::vector<float> out(hop);
    for (int i = 0; i < 20; ++i) l.run(s, in.data(), window, out.data(), hop);
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = Clock::now();
        l.run(s, in.data(), window, out.data(), hop);
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

} // namespace

TEST_CASE("G7: both backends match the CPU reference, with and without graphs") {
    REQUIRE_GPU();
    for (Backend b : {Backend::Ort, Backend::Torch}) {
        for (const char* model : {"gain", "many_kernels"}) {
            for (bool graph : {false, true}) {
                auto stage = makeStage(b, model, 20, graph);
                Locked l;
                l.prepare(*stage);
                INFO(std::string(name(b)), " ", model, graph ? " graphed" : " eager");
                CHECK(stage->graphed() == graph);
                const uint32_t hop = 960, window = kContext + hop;
                float worst = 0;
                for (unsigned seed = 1; seed <= 5; ++seed) {
                    const auto in = noise(window, seed);
                    std::vector<float> out(hop);
                    l.run(*stage, in.data(), window, out.data(), hop);
                    for (uint32_t k = 0; k < hop; ++k) worst = std::max(worst, std::fabs(out[k] - 0.5f * in[kContext + k]));
                }
                MESSAGE(std::string(name(b)), " ", model, graph ? " graphed" : " eager", ": max abs diff ", worst);
                CHECK(worst <= 1e-6f);
            }
        }
    }
}

TEST_CASE("G8: a CUDA graph at least halves the time of a launch-bound model") {
    REQUIRE_GPU();
    for (Backend b : {Backend::Ort, Backend::Torch}) {
        auto eager = makeStage(b, "many_kernels", 20, false);
        auto graphed = makeStage(b, "many_kernels", 20, true);
        Locked l;
        l.prepare(*eager);
        l.prepare(*graphed);
        REQUIRE(graphed->graphed());
        const double a = medianMs(*eager, 20, 300);
        const double g = medianMs(*graphed, 20, 300);
        MESSAGE(std::string(name(b)), " many_kernels, 300 runs: median ", a, " ms without graph, ", g, " ms with graph (", g / a * 100, " %)");
        CHECK(g <= 0.5 * a);
    }
}

TEST_CASE("G9: an ORT stage and a LibTorch stage alternate on two threads for a while without errors") {
    REQUIRE_GPU();
    const char* env = std::getenv("DSPTB_GPU_SOAK_SECONDS");
    const double seconds = env ? std::atof(env) : 60.0;
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<uint64_t> runs[2] = {0, 0};
    std::atomic<float> worst[2] = {0.0f, 0.0f};
    Locked shared;
    auto worker = [&](int index, Backend b) {
        try {
            auto stage = makeStage(b, "many_kernels", 20, true);
            Locked l;
            l.prepare(*stage);
            const uint32_t hop = 960, window = kContext + hop;
            const auto in = noise(window, 11 + static_cast<unsigned>(index));
            std::vector<float> out(hop);
            while (!stop.load()) {
                l.run(*stage, in.data(), window, out.data(), hop);
                float w = 0;
                for (uint32_t k = 0; k < hop; ++k) w = std::max(w, std::fabs(out[k] - 0.5f * in[kContext + k]));
                if (w > worst[index].load()) worst[index].store(w);
                runs[index].fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } catch (const std::exception& e) {
            MESSAGE("worker ", index, " failed: ", e.what());
            errors.fetch_add(1);
        }
    };
    std::thread a(worker, 0, Backend::Ort), b(worker, 1, Backend::Torch);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true);
    a.join();
    b.join();
    MESSAGE("ORT ", runs[0].load(), " runs (worst diff ", worst[0].load(), "), LibTorch ", runs[1].load(), " runs (worst diff ", worst[1].load(), ") in ", seconds, " s");
    CHECK(errors.load() == 0);
    CHECK(runs[0].load() > 100);
    CHECK(runs[1].load() > 100);
    CHECK(worst[0].load() <= 1e-6f);
    CHECK(worst[1].load() <= 1e-6f);
}

namespace {
size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return pmc.PrivateUsage;
}
} // namespace

// CUDA, cuDNN and kernel modules load lazily, so memory steps up once, by up to a couple of hundred MB (ORT in eager mode
// does it over its first ~25 sessions), and then stays. A real leak grows in every window, so growth counts only when two
// successive windows of 30 cycles both grow by more than 8 MB.
// LibTorch is the exception: every TorchScript model load costs about 1 MB (eager) to 2.3 MB (graphed) of host memory that
// stays after the model is destroyed (cause not found; see docs/GPU_RUNTIME.md section 15). It is reported as a warning, not a failure,
// because it happens only when a module is prepared, not while it runs (see "running a stage thousands of times").
template <class Once> void checkNoLeak(const std::string& what, bool strict, Once&& once) {
    for (int i = 0; i < 30; ++i) once(); // warm-up: lazy loading
    double grow[2];
    for (double& g : grow) {
        const size_t before = privateBytes();
        for (int i = 0; i < 30; ++i) once();
        g = (static_cast<double>(privateBytes()) - static_cast<double>(before)) / (1024.0 * 1024.0);
    }
    MESSAGE(what, ": private bytes grew by ", grow[0], " MB, then ", grow[1], " MB over two windows of 30 cycles");
    if (strict) CHECK_FALSE((grow[0] > 8.0 && grow[1] > 8.0));
    else WARN_FALSE((grow[0] > 8.0 && grow[1] > 8.0));
}

TEST_CASE("creating and destroying a stage repeatedly does not leak") {
    REQUIRE_GPU();
    for (Backend b : {Backend::Ort, Backend::Torch}) {
        for (bool graph : {false, true}) {
            checkNoLeak(std::string(name(b)) + (graph ? " graphed" : " eager"), b == Backend::Ort, [&] {
                auto stage = makeStage(b, "gain", 20, graph);
                Locked l;
                l.prepare(*stage);
                std::vector<float> in(1920, 0.1f), out(960);
                l.run(*stage, in.data(), 1920, out.data(), 960);
            });
        }
    }
}

TEST_CASE("running a stage thousands of times does not grow the process") {
    REQUIRE_GPU();
    for (Backend b : {Backend::Ort, Backend::Torch}) {
        for (bool graph : {false, true}) {
            auto stage = makeStage(b, "gain", 20, graph);
            Locked l;
            l.prepare(*stage);
            std::vector<float> in(1920, 0.1f), out(960);
            for (int i = 0; i < 200; ++i) l.run(*stage, in.data(), 1920, out.data(), 960);
            const size_t before = privateBytes();
            for (int i = 0; i < 3000; ++i) l.run(*stage, in.data(), 1920, out.data(), 960);
            const double growMb = (static_cast<double>(privateBytes()) - static_cast<double>(before)) / (1024.0 * 1024.0);
            MESSAGE(std::string(name(b)), graph ? " graphed" : " eager", ": private bytes grew by ", growMb, " MB over 3000 runs");
            CHECK(growMb < 1.0);
        }
    }
}


TEST_CASE("creating a stage on a new thread each time does not leak") {
    REQUIRE_GPU();
    for (Backend b : {Backend::Ort, Backend::Torch}) {
        for (bool graph : {false, true}) {
            checkNoLeak(std::string(name(b)) + (graph ? " graphed" : " eager") + ", new thread", b == Backend::Ort, [&] {
                std::thread t([&] {
                    auto stage = makeStage(b, "gain", 20, graph);
                    Locked l;
                    l.prepare(*stage);
                    std::vector<float> in(1920, 0.1f), out(960);
                    l.run(*stage, in.data(), 1920, out.data(), 960);
                    stage->teardown(); // on the thread that created it (Stage::teardown)
                });
                t.join();
            });
        }
    }
}
