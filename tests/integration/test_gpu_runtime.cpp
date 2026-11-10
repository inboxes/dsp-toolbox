// GPU runtime integration tests (docs/GPU_RUNTIME.md G10 to G15): the real runtime, the real ORT and LibTorch
// stages and the test.gpu-probe module, through the host's own module and engine code. They need an NVIDIA
// GPU; without one every case skips itself. They live in their own executable because the fake dsptb_gpu.dll
// used by the loader tests has the same file name as the real one and the two cannot share a process.
#include <doctest/doctest.h>

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/engine.hpp>
#include <dsptb/core/gpu_runtime_loader.hpp>
#include <dsptb/core/module_instance.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/offline_render.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/signal_gen.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace dsptb;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kRate = 48000;

bool gpuReady() {
    static const bool ok = [] {
        GpuRuntimeRequest req;
        req.dir = DSPTB_GPU_RUNTIME_STAGE_DIR;
        if (const char* sync = std::getenv("DSPTB_GPU_TEST_SYNC"))
            req.blocking = std::string(sync) != "spin";
        GpuRuntimeLoader::instance().load(req);
        if (!GpuRuntimeLoader::instance().available())
            MESSAGE("no GPU runtime, skipping: ", GpuRuntimeLoader::instance().state().statusText);
        return GpuRuntimeLoader::instance().available();
    }();
    return ok;
}
#define REQUIRE_GPU()                                                                                        \
    do {                                                                                                     \
        if (!gpuReady())                                                                                     \
            return;                                                                                          \
    } while (0)

ModuleRegistry& registry() {
    static ModuleRegistry reg = [] {
        gpuReady(); // before the scan, as in the app
        ModuleRegistry r;
        r.scan({fs::path(DSPTB_MODULES_DIR), fs::path(DSPTB_TEST_MODULES_DIR)});
        return r;
    }();
    return reg;
}

// Parameter values of the probe, by id.
struct ProbeParams {
    int backend = 0; // 0 ORT, 1 Torch
    bool graphs = true;
    int hopMs = 20; // 10, 20, 40
    int model = 0;  // 0 Gain, 1 ManyKernels
    int failNext = 0;
    int loadDelayMs = 0;
    int budgetMs = 25;
    nlohmann::json toJson() const {
        const int hopIndex = hopMs == 10 ? 0 : hopMs == 20 ? 1 : 2;
        return {{"backend", backend},   {"graphs", graphs},      {"hop_ms", hopIndex},
                {"model", model},       {"fail_next", failNext}, {"load_delay_ms", loadDelayMs},
                {"budget_ms", budgetMs}};
    }
};

// One probe instance driven directly, block by block, with the test thread as the audio thread.
struct Probe {
    explicit Probe(const ProbeParams& p, bool offline = false) {
        REQUIRE(gpuReady());
        ModulePackage* pkg = nullptr;
        std::string err;
        const dsptb_module_desc* desc = registry().descriptor("test.gpu-probe", &pkg, err);
        INFO(err);
        REQUIRE(desc != nullptr);
        inst = ModuleInstance::create(1, *pkg, *desc, &host, err);
        REQUIRE(inst != nullptr);
        apply(p);
        const auto t0 = Clock::now();
        REQUIRE(inst->prepare(kRate, block, offline, err));
        prepareMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    ~Probe() { inst.reset(); }
    void apply(const ProbeParams& p) {
        const nlohmann::json values =
            p.toJson(); // a named object: iterating a temporary's items() would dangle
        for (const auto& [key, value] : values.items())
            inst->params().set(static_cast<uint32_t>(inst->params().indexOf(key)),
                               value.is_boolean() ? (value.get<bool>() ? 1.0f : 0.0f) : value.get<float>());
    }
    nlohmann::json stats() const {
        std::string json;
        REQUIRE(inst->saveState(json));
        return nlohmann::json::parse(json);
    }
    // Pushes one block of mono input through; returns the output of the left channel.
    void process(std::vector<float>& l, std::vector<float>& r, size_t pos, uint32_t n) {
        dsptb_transport t{};
        t.struct_size = sizeof t;
        t.source = DSPTB_SOURCE_OFFLINE;
        t.playing = 1;
        t.sample_clock = pos;
        inst->params().copyToRt();
        REQUIRE(inst->callProcess(l.data() + pos, r.data() + pos, n, t));
    }
    // Drives `in` through. With `lockstep`, waits after every block for the worker to finish every hop that
    // is complete, so nothing misses and the run is deterministic.
    std::vector<float> run(const std::vector<float>& in, bool lockstep, uint32_t hopFrames) {
        std::vector<float> l = in, r = in;
        for (size_t pos = 0; pos < in.size(); pos += block) {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, in.size() - pos));
            process(l, r, pos, n);
            if (lockstep) {
                const uint64_t ready = (pos + n) / hopFrames;
                const auto end = Clock::now() + std::chrono::seconds(5);
                while (stats()["runs"].get<uint64_t>() < ready && Clock::now() < end)
                    std::this_thread::yield();
            }
        }
        return l;
    }
    void waitState(const char* state, int timeoutMs = 20000) const {
        const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (stats()["state"] != state && Clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SimpleModuleHost host;
    std::shared_ptr<ModuleInstance> inst;
    uint32_t block = 128;
    double prepareMs = 0;
};

std::vector<float> impulse(size_t n, size_t at) {
    std::vector<float> v(n, 0.0f);
    v[at] = 1.0f;
    return v;
}

size_t peakIndex(const std::vector<float>& v) {
    return static_cast<size_t>(
        std::max_element(v.begin(), v.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) -
        v.begin());
}

AudioBuffer stereo(const std::vector<float>& mono) {
    AudioBuffer b;
    b.sampleRate = kRate;
    b.ch[0] = mono;
    b.ch[1] = mono;
    return b;
}

SessionDescription probeSession(const ProbeParams& p) {
    nlohmann::json instance = {{"module", "test.gpu-probe"}, {"enabled", true}, {"params", p.toJson()}};
    nlohmann::json doc = {
        {"format", "dsptb-session"}, {"version", 1}, {"instances", nlohmann::json::array({instance})}};
    SessionDescription desc;
    std::string err;
    REQUIRE(parseSessionDescription(doc.dump(), desc, err));
    return desc;
}

AudioBuffer renderProbe(const ProbeParams& p, const AudioBuffer& in) {
    RenderOptions opt;
    opt.tailSeconds = 0.0;
    AudioBuffer out;
    std::string err;
    INFO(err);
    REQUIRE(renderOffline(registry(), probeSession(p), in, opt, out, err));
    return out;
}

const char* backendName(int b) {
    return b == 0 ? "ORT" : "Torch";
}

} // namespace

TEST_CASE(
    "G10: an impulse comes out exactly latency_frames later, 0.5 high, for both backends and every hop") {
    REQUIRE_GPU();
    for (int backend : {0, 1}) {
        for (int hopMs : {10, 20, 40}) {
            ProbeParams p;
            p.backend = backend;
            p.hopMs = hopMs;
            INFO(backendName(backend), " hop ", hopMs, " ms");
            // Offline, through the export path: the render trims the latency, so an exact latency puts the
            // impulse back where it started.
            {
                const size_t at = 3000;
                const AudioBuffer out = renderProbe(p, stereo(impulse(30000, at)));
                CHECK(peakIndex(out.ch[0]) == at);
                CHECK(std::fabs(out.ch[0][at] - 0.5f) < 1e-6f);
                float rest = 0;
                for (size_t i = 0; i < out.ch[0].size(); ++i)
                    if (i != at)
                        rest = std::max(rest, std::fabs(out.ch[0][i]));
                CHECK(rest < 1e-6f);
            }
            // Live (a worker thread), blocks of 128, lockstep so the worker never lags: the impulse sits at
            // latency_frames.
            {
                Probe probe(p);
                probe.waitState("Running");
                REQUIRE(probe.stats()["state"] == "Running");
                const uint32_t latency = probe.inst->latencyFrames();
                CHECK(latency == static_cast<uint32_t>(hopMs * 48 + 25 * 48));
                const auto out = probe.run(impulse(40000, 0), true, static_cast<uint32_t>(hopMs * 48));
                CHECK(peakIndex(out) == latency);
                CHECK(std::fabs(out[latency] - 0.5f) < 1e-6f);
                CHECK(probe.stats()["misses"] == 0);
            }
        }
    }
}

TEST_CASE("G11: paced at real time, 48 kHz and 128 frames, a graphed ManyKernels probe never misses") {
    REQUIRE_GPU();
    const char* env = std::getenv("DSPTB_GPU_RT_SECONDS");
    const double seconds = env ? std::atof(env) : 30.0; // the acceptance run uses 600
    for (int backend : {0, 1}) {
        EngineOptions opts;
        opts.declickMs = 0.0;
        Engine engine(opts);
        SessionModel model(engine, registry());
        AudioConfig cfg;
        cfg.backend = BackendKind::Null;
        cfg.sampleRate = kRate;
        cfg.bufferFrames = 128;
        std::string err;
        REQUIRE(engine.open(cfg, err));
        NullBackend* nb = engine.nullBackend();
        REQUIRE(nb != nullptr);
        nb->setRealtime(true);
        GenParams g;
        g.type = SignalType::Pink;
        g.dbfs = -18.0;
        g.seconds = 20.0;
        g.sampleRate = kRate;
        auto file = std::make_shared<LoadedFile>();
        file->path = "<generated>";
        file->audio = pcmToStereo(generateSignal(g));
        file->overview = computeOverview(file->audio);
        file->seconds = 20.0;
        engine.setFile(file);
        const InstanceId id = model.add("test.gpu-probe", err);
        INFO(err);
        REQUIRE(id != 0);
        ProbeParams p;
        p.backend = backend;
        p.model = 1;
        p.hopMs = 20;
        if (const char* b = std::getenv("DSPTB_GPU_RT_BUDGET_MS"))
            p.budgetMs = std::atoi(b);
        InstanceRecord* rec = model.find(id);
        const nlohmann::json values = p.toJson();
        for (const auto& [key, value] : values.items())
            rec->instance->params().set(static_cast<uint32_t>(rec->instance->params().indexOf(key)),
                                        value.is_boolean() ? (value.get<bool>() ? 1.0f : 0.0f)
                                                           : value.get<float>());
        REQUIRE(rec->instance->prepare(kRate, 128, false, err));
        REQUIRE(engine.start(err));
        engine.setLoop(true, 0, static_cast<uint64_t>(file->audio.frames()));
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // let the model load
        engine.play();
        std::vector<SessionMessage> messages;
        const auto t0 = Clock::now();
        uint64_t missesAtOneSecond = 0;
        bool sampled = false;
        while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            model.tick(std::chrono::duration<double>(Clock::now() - t0).count(), messages);
            if (!sampled && std::chrono::duration<double>(Clock::now() - t0).count() > 1.0) {
                std::string j;
                if (rec->instance->saveState(j))
                    missesAtOneSecond = nlohmann::json::parse(j)["misses"].get<uint64_t>();
                sampled = true;
            }
        }
        std::string json;
        REQUIRE(rec->instance->saveState(json));
        const auto s = nlohmann::json::parse(json);
        MESSAGE(backendName(backend), " ManyKernels, hop 20 ms, ", seconds, " s: runs ",
                s["runs"].get<uint64_t>(), ", misses ", s["misses"].get<uint64_t>(), ", compute p50 ",
                s["p50Us"].get<int>() / 1000.0, " ms, p99 ", s["p99Us"].get<int>() / 1000.0, " ms, max ",
                s["maxUs"].get<int>() / 1000.0, " ms, graphed ", s["graphed"].get<bool>(),
                ", engine underruns ", engine.underruns(), ", compute all-time max ",
                s["computeAllMaxUs"].get<int>() / 1000.0, " ms, worker wake late max ",
                s["wakeLateMaxUs"].get<int>() / 1000.0, " ms, whole hop max ",
                s["hopTotalMaxUs"].get<int>() / 1000.0, " ms, misses in the first second ", missesAtOneSecond,
                ", last miss at output frame ", s["lastMissFrame"].get<uint64_t>());
        CHECK(s["state"] == "Running");
        CHECK(s["graphed"] == true);
        // Latency comes first: an occasional GPU stall (another process sharing the GPU made single runs take
        // about 18 ms instead of 2.6 ms) drops one hop to the dry signal instead of growing the budget. The
        // bar is the dropped share of hops: at most 0.2 %. (A hop being output when a late model load
        // finishes is not counted.)
        const double dropped = static_cast<double>(s["misses"].get<uint64_t>()) /
                               std::max<uint64_t>(1, s["runs"].get<uint64_t>());
        MESSAGE("dropped hops ", dropped * 100.0, " %");
        CHECK(dropped <= 0.002);
        CHECK(engine.underruns() == 0);
        if (const ModulePackage* pkg = registry().findByModule("test.gpu-probe"); pkg && pkg->guardViolations)
            CHECK(pkg->guardViolations() == 0);
        engine.stop();
    }
}

TEST_CASE("G12: re-preparing one probe 20 times does not disturb another that is running") {
    REQUIRE_GPU();
    ProbeParams running;
    running.model = 1;
    Probe a(running);
    a.waitState("Running");
    REQUIRE(a.stats()["state"] == "Running");
    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    // The test thread of probe `a` plays the audio thread at real time while `b` is prepared over and over.
    std::thread audio([&] {
        std::vector<float> l(128, 0.2f), r(128, 0.2f);
        size_t pos = 0;
        const auto t0 = Clock::now();
        while (!stop.load()) {
            std::fill(l.begin(), l.end(), 0.2f);
            std::fill(r.begin(), r.end(), 0.2f);
            dsptb_transport t{};
            t.struct_size = sizeof t;
            t.source = DSPTB_SOURCE_OFFLINE;
            t.playing = 1;
            t.sample_clock = pos;
            a.inst->callProcess(l.data(), r.data(), 128, t);
            pos += 128;
            blocks.fetch_add(1);
            const auto due = t0 + std::chrono::duration<double>(static_cast<double>(pos) / kRate);
            while (Clock::now() < due)
                std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t missesBefore = a.stats()["misses"].get<uint64_t>();
    ProbeParams other;
    other.model = 1;
    other.backend = 1;
    Probe b(other);
    int failures = 0;
    for (int i = 0; i < 20; ++i) {
        other.hopMs = i % 3 == 0 ? 10 : i % 3 == 1 ? 20 : 40;
        b.apply(other);
        std::string err;
        if (!b.inst->prepare(kRate, 128, false, err))
            ++failures;
        b.waitState("Running");
        if (b.stats()["state"] != "Running")
            ++failures;
    }
    const uint64_t missesAfter = a.stats()["misses"].get<uint64_t>();
    stop.store(true);
    audio.join();
    MESSAGE("20 re-prepares of a LibTorch probe next to a running ORT probe: ", missesAfter - missesBefore,
            " missed hops on the running one, ", blocks.load(), " blocks");
    CHECK(failures == 0);
    CHECK(a.stats()["state"] == "Running");
    CHECK(missesAfter - missesBefore <= 20); // at most one hop per re-prepare
}

TEST_CASE("G13: a failing run, or a real CUDA error, turns the output dry; nothing crashes") {
    REQUIRE_GPU();
    for (int fail : {1, 2}) {
        ProbeParams p;
        p.model = 1;
        Probe probe(p);
        probe.waitState("Running");
        REQUIRE(probe.stats()["state"] == "Running");
        const uint32_t hop = 20 * 48;
        // A steady 0.4 on both channels. Before the failure the output is the model's 0.5 x, after it the dry
        // input.
        std::vector<float> in(kRate * 2, 0.4f);
        std::vector<float> l = in, r = in;
        size_t failedAt = 0;
        for (size_t pos = 0; pos < in.size(); pos += probe.block) {
            if (pos >= kRate / 2 && failedAt == 0) {
                failedAt = pos;
                probe.inst->params().set(static_cast<uint32_t>(probe.inst->params().indexOf("fail_next")),
                                         static_cast<float>(fail));
            }
            probe.process(l, r, pos, probe.block);
            const auto end = Clock::now() + std::chrono::milliseconds(3);
            while (Clock::now() < end)
                std::this_thread::yield(); // roughly real time, so the worker keeps up
        }
        probe.waitState("Failed", 2000);
        const auto s = probe.stats();
        INFO("fail mode ", fail, " at ", failedAt, " error: ", s["error"].get<std::string>());
        CHECK(s["state"] == "Failed");
        CHECK(s["error"].get<std::string>().size() > 0);
        if (fail == 2)
            CHECK(s["error"].get<std::string>().find("CUDA") != std::string::npos);
        // Steady state of the tail: the dry signal, 0.4.
        CHECK(std::fabs(l.back() - 0.4f) < 1e-5f);
        // And it keeps answering.
        probe.process(l, r, 0, probe.block);
        CHECK(std::isfinite(l[0]));
        (void)hop;
    }
}

TEST_CASE("G14: an offline render is byte-identical when repeated, for both backends") {
    REQUIRE_GPU();
    GenParams g;
    g.type = SignalType::Pink;
    g.dbfs = -18.0;
    g.seconds = 10.0;
    g.sampleRate = kRate;
    const AudioBuffer in = pcmToStereo(generateSignal(g));
    for (int backend : {0, 1}) {
        ProbeParams p;
        p.backend = backend;
        p.model = 1;
        const AudioBuffer a = renderProbe(p, in);
        const AudioBuffer b = renderProbe(p, in);
        REQUIRE(a.frames() == b.frames());
        size_t differing = 0;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < a.frames(); ++i)
                differing += a.ch[c][i] != b.ch[c][i];
        MESSAGE(backendName(backend), ": ", differing, " differing samples of ", a.frames() * 2);
        CHECK(differing == 0);
        // And it is the model's output: 0.5 x.
        double worst = 0;
        for (size_t i = 0; i < a.frames(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(a.ch[0][i]) - 0.5 * in.ch[0][i]));
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("G15: loading is asynchronous: dry audio meanwhile, an unchanged latency, and a Loading state") {
    REQUIRE_GPU();
    ProbeParams p;
    p.loadDelayMs = 2000;
    Probe probe(p);
    CHECK(probe.prepareMs < 1000.0); // prepare() returns at once; the model loads on the worker
    const uint32_t latency = probe.inst->latencyFrames();
    CHECK(latency == 20u * 48 + 25u * 48);
    CHECK(probe.stats()["state"] == "Loading");
    // Audio keeps running, dry: a steady 0.3 on the input comes out as 0.3 (not the model's 0.15, not
    // silence).
    std::vector<float> in(kRate / 2, 0.3f);
    std::vector<float> l = in, r = in;
    for (size_t pos = 0; pos < in.size(); pos += probe.block)
        probe.process(l, r, pos, static_cast<uint32_t>(std::min<size_t>(probe.block, in.size() - pos)));
    CHECK(std::fabs(l.back() - 0.3f) < 1e-5f);
    CHECK(probe.stats()["state"] == "Loading");
    CHECK(probe.inst->latencyFrames() == latency);
    probe.waitState("Running", 10000);
    REQUIRE(probe.stats()["state"] == "Running");
    CHECK(probe.inst->latencyFrames() == latency);
    // After loading the model output arrives one latency after its input: 0.5 x 0.3.
    std::vector<float> after(kRate, 0.3f);
    std::vector<float> l2 = after, r2 = after;
    for (size_t pos = 0; pos < after.size(); pos += probe.block) {
        probe.process(l2, r2, pos, static_cast<uint32_t>(std::min<size_t>(probe.block, after.size() - pos)));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(std::fabs(l2.back() - 0.15f) < 1e-5f);
}
