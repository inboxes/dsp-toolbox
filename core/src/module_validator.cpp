#include "dsptb/core/module_validator.hpp"

#include "dsptb/core/module_instance.hpp"
#include "dsptb/core/recording_canvas.hpp"

#include <nlohmann/json.hpp>

#include <Windows.h>
// psapi.h must follow Windows.h
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <sstream>

namespace dsptb {

namespace fs = std::filesystem;

bool ModuleValidation::passed() const {
    return std::all_of(checks.begin(), checks.end(), [](const ValidationCheck& c) { return c.passed; });
}

bool ValidationReport::passed() const {
    return !skipped && std::all_of(packageChecks.begin(), packageChecks.end(),
                       [](const ValidationCheck& c) { return c.passed; }) &&
           std::all_of(modules.begin(), modules.end(),
                       [](const ModuleValidation& m) { return m.passed(); }) &&
           rtGuardHits == 0;
}

namespace {

size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return static_cast<size_t>(pmc.PrivateUsage);
}

enum class Signal { Silence, Noise, Impulse, Square, Denormal };

const char* signalName(Signal s) {
    switch (s) {
    case Signal::Silence:
        return "silence";
    case Signal::Noise:
        return "white noise -6 dBFS";
    case Signal::Impulse:
        return "impulse";
    case Signal::Square:
        return "full-scale square";
    case Signal::Denormal:
        return "denormal noise (1e-30)";
    }
    return "?";
}

// Fills n frames of `sig`; `clock` is the running frame counter of the stream.
void fillSignal(Signal sig, std::mt19937& rng, float* l, float* r, uint32_t n, uint64_t clock) {
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    for (uint32_t i = 0; i < n; ++i) {
        float a = 0.0f, b = 0.0f;
        switch (sig) {
        case Signal::Silence:
            break;
        case Signal::Noise:
            a = 0.5f * uni(rng);
            b = 0.5f * uni(rng);
            break;
        case Signal::Impulse:
            a = b = (clock + i == 0) ? 1.0f : 0.0f;
            break;
        case Signal::Square:
            a = b = (((clock + i) / 32) & 1u) ? -1.0f : 1.0f;
            break;
        case Signal::Denormal:
            a = 1e-30f * uni(rng);
            b = 1e-30f * uni(rng);
            break;
        }
        l[i] = a;
        r[i] = b;
    }
}

bool finite(const float* x, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (!std::isfinite(x[i]))
            return false;
    return true;
}

double rmsDb(const float* x, uint32_t n) {
    if (n == 0)
        return -300.0;
    double acc = 0.0;
    for (uint32_t i = 0; i < n; ++i)
        acc += static_cast<double>(x[i]) * x[i];
    const double rms = std::sqrt(acc / n);
    return rms > 1e-15 ? 20.0 * std::log10(rms) : -300.0;
}

// Drives one processor instance block by block.
struct Driver {
    Driver(ModuleInstance& m, uint32_t maxBlock, uint32_t seed)
        : inst(m), rng(seed), l(maxBlock), r(maxBlock) {}

    // Returns false if the module crashed.
    bool block(Signal sig, uint32_t n) {
        fillSignal(sig, rng, l.data(), r.data(), n, clock);
        dsptb_transport t{};
        t.struct_size = sizeof(t);
        t.source = DSPTB_SOURCE_OFFLINE;
        t.playing = 1;
        t.position_seconds = static_cast<double>(clock) / inst.sampleRate();
        t.sample_clock = clock;
        inst.params().copyToRt();
        const bool ok = inst.callProcess(l.data(), r.data(), n, t);
        clock += n;
        return ok;
    }
    bool outputFinite(uint32_t n) const { return finite(l.data(), n) && finite(r.data(), n); }

    ModuleInstance& inst;
    std::mt19937 rng;
    std::vector<float> l, r;
    uint64_t clock = 0;
};

struct Checker {
    std::vector<ValidationCheck>& out;
    void add(const std::string& name, bool ok, const std::string& detail = {}) {
        out.push_back({name, ok, ok ? std::string() : detail});
    }
};

std::shared_ptr<ModuleInstance> make(ModulePackage& pkg, const dsptb_module_desc& d, SimpleModuleHost& host,
                                     std::string& error) {
    return ModuleInstance::create(1, pkg, d, &host, error);
}

// create -> prepare -> 200 blocks of random size -> reset -> 50 blocks -> destroy.
bool lifecycle(ModulePackage& pkg, const dsptb_module_desc& d, double sr, uint32_t maxBlock,
               std::string& why) {
    SimpleModuleHost host;
    std::string err;
    auto inst = make(pkg, d, host, err);
    if (!inst) {
        why = "create failed: " + err;
        return false;
    }
    if (!inst->prepare(sr, maxBlock, false, err)) {
        why = "prepare failed: " + err;
        return false;
    }
    if (d.kind != DSPTB_KIND_PROCESSOR)
        return true;
    Driver drv(*inst, maxBlock, 7);
    std::mt19937 sizes(11);
    std::uniform_int_distribution<uint32_t> pick(0, maxBlock);
    for (int i = 0; i < 200; ++i) {
        const uint32_t n = pick(sizes);
        if (!drv.block(Signal::Noise, n)) {
            why = "process() crashed";
            return false;
        }
        if (!drv.outputFinite(n)) {
            why = "non-finite output";
            return false;
        }
    }
    if (!inst->callReset()) {
        why = "reset() crashed";
        return false;
    }
    for (int i = 0; i < 50; ++i) {
        const uint32_t n = pick(sizes);
        if (!drv.block(Signal::Noise, n) || !drv.outputFinite(n)) {
            why = "failure after reset()";
            return false;
        }
    }
    return true;
}

void checkProcessor(ModulePackage& pkg, const dsptb_module_desc& d, std::vector<ValidationCheck>& out) {
    Checker chk{out};
    SimpleModuleHost host;
    std::string err;

    // --- lifecycle matrix ---
    {
        std::string failure;
        for (double sr : {44100.0, 48000.0, 96000.0})
            for (uint32_t block : {32u, 256u, 2048u}) {
                std::string why;
                if (failure.empty() && !lifecycle(pkg, d, sr, block, why))
                    failure = std::to_string(static_cast<int>(sr)) + " Hz, max block " +
                              std::to_string(block) + ": " + why;
            }
        chk.add("lifecycle (3 rates x 3 block sizes, 200 random blocks, reset, 50 blocks)", failure.empty(),
                failure);
    }

    // --- create/destroy 50x, memory growth under 1 MB ---
    // A module that needs the GPU runtime loads CUDA, cuDNN and kernel modules lazily on first use. That shows up as a
    // one-time step of tens to hundreds of MB (measured: ORT, 200 MB, reached within 25 cycles), not as a leak per
    // instance. So such a module gets a longer warm-up and a looser bound. Only a real leak per instance grows with
    // the number of cycles.
    {
        const bool gpu = pkg.manifest.requiresGpuRuntime > 0;
        const int warmUp = gpu ? 40 : 5;
        const double limitMb = gpu ? 16.0 : 1.0;
        for (int i = 0; i < warmUp; ++i) { // warm up allocators and lazy statics first
            auto m = make(pkg, d, host, err);
            if (m)
                m->prepare(48000.0, 256, false, err);
        }
        const size_t before = privateBytes();
        bool created = true;
        for (int i = 0; i < 50 && created; ++i) {
            auto m = make(pkg, d, host, err);
            created = m && m->prepare(48000.0, 256, false, err);
        }
        const size_t after = privateBytes();
        const double grewMb = (static_cast<double>(after) - static_cast<double>(before)) / (1024.0 * 1024.0);
        chk.add(gpu ? "create/destroy x50 does not leak (< 16 MB growth after a longer warm-up)"
                    : "create/destroy x50 does not leak (< 1 MB growth)",
                created && grewMb < limitMb,
                created ? "private bytes grew by " + std::to_string(grewMb) + " MB"
                        : "create/prepare failed: " + err);
    }

    auto inst = make(pkg, d, host, err);
    if (!inst || !inst->prepare(48000.0, 512, false, err)) {
        chk.add("instance for signal tests", false, err);
        return;
    }

    // --- latency constant while processing ---
    {
        Driver drv(*inst, 512, 3);
        const uint32_t before = inst->queryLatencyFrames();
        bool ok = true;
        for (int i = 0; i < 20 && ok; ++i)
            ok = drv.block(Signal::Noise, 256);
        chk.add("latency_frames is constant while processing",
                ok && before == inst->queryLatencyFrames() && before == inst->latencyFrames(),
                "latency changed from " + std::to_string(before) + " to " +
                    std::to_string(inst->queryLatencyFrames()));
    }

    // --- inputs ---
    for (Signal sig : {Signal::Silence, Signal::Noise, Signal::Impulse, Signal::Square, Signal::Denormal}) {
        auto m = make(pkg, d, host, err);
        m->prepare(48000.0, 512, false, err);
        Driver drv(*m, 512, 5);
        bool ok = true;
        for (int i = 0; i < 100 && ok; ++i)
            ok = drv.block(sig, 256) && drv.outputFinite(256);
        chk.add(std::string("input: ") + signalName(sig) + " -> finite output", ok,
                "crash or non-finite output");
    }

    // --- silence after noise: no runaway (output RMS < -100 dBFS after the tail) ---
    {
        auto m = make(pkg, d, host, err);
        m->prepare(48000.0, 512, false, err);
        Driver drv(*m, 512, 9);
        bool ok = true;
        for (int i = 0; i < 40 && ok; ++i)
            ok = drv.block(Signal::Noise, 256);
        const uint32_t tail = m->tailFrames() + m->latencyFrames() + 48000;
        for (uint32_t done = 0; done + 256 <= tail + 2048 && ok; done += 256)
            ok = drv.block(Signal::Silence, 256);
        double worst = -300.0;
        for (int i = 0; i < 8 && ok; ++i) {
            ok = drv.block(Signal::Silence, 256);
            worst = std::max({worst, rmsDb(drv.l.data(), 256), rmsDb(drv.r.data(), 256)});
        }
        chk.add("silence in -> silence out after the tail (< -100 dBFS)", ok && worst < -100.0,
                ok ? "output RMS " + std::to_string(worst) + " dBFS" : "crash");
    }

    // --- denormal speed: no more than 3x slower than normal input ---
    {
        const auto timeIt = [&](Signal sig) {
            auto m = make(pkg, d, host, err);
            m->prepare(48000.0, 512, false, err);
            Driver drv(*m, 512, 13);
            double best = 1e30;
            for (int run = 0; run < 3; ++run) {
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < 400; ++i)
                    drv.block(sig, 512);
                best = std::min(best,
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            return best;
        };
        const double normal = timeIt(Signal::Noise);
        const double denorm = timeIt(Signal::Denormal);
        // Very cheap modules are dominated by the signal generator; only judge when there is something to
        // measure.
        const bool ok = normal < 0.002 || denorm < 3.0 * normal;
        chk.add("denormal input is not more than 3x slower", ok,
                "noise " + std::to_string(normal * 1000) + " ms, denormals " + std::to_string(denorm * 1000) +
                    " ms");
    }

    // --- parameter fuzz ---
    {
        auto m = make(pkg, d, host, err);
        m->prepare(48000.0, 512, false, err);
        Driver drv(*m, 512, 17);
        std::mt19937 rng(23);
        bool ok = true;
        std::string detail;
        for (int b = 0; b < 300 && ok; ++b) {
            for (uint32_t p = 0; p < m->params().size(); ++p) {
                const auto& pd = m->params().desc(p);
                std::uniform_real_distribution<float> u(0.0f, 1.0f);
                float v;
                if ((pd.flags & DSPTB_PARAM_LOG) && pd.min_value > 0.0f)
                    v = pd.min_value * std::pow(pd.max_value / pd.min_value, u(rng));
                else
                    v = pd.min_value + (pd.max_value - pd.min_value) * u(rng);
                m->params().set(p, v);
            }
            ok = drv.block(Signal::Noise, 256) && drv.outputFinite(256);
            if (!ok)
                detail = "block " + std::to_string(b);
        }
        // every value of every bool and enum
        for (uint32_t p = 0; p < m->params().size() && ok; ++p) {
            const auto& pd = m->params().desc(p);
            if (pd.type != DSPTB_PARAM_BOOL && pd.type != DSPTB_PARAM_ENUM)
                continue;
            for (int v = static_cast<int>(pd.min_value); v <= static_cast<int>(pd.max_value) && ok; ++v) {
                m->params().set(p, static_cast<float>(v));
                for (int i = 0; i < 10 && ok; ++i)
                    ok = drv.block(Signal::Noise, 256) && drv.outputFinite(256);
                if (!ok)
                    detail = std::string("parameter ") + pd.id + " = " + std::to_string(v);
            }
        }
        chk.add("parameter fuzz keeps the output finite", ok, detail);
    }
}

// ---- analyzers: the same calls the analysis thread makes ----

enum class AnalyzerInput { Silence, Noise, Impulse, Square, Denormal, NonFinite };

const char* analyzerInputName(AnalyzerInput s) {
    switch (s) {
    case AnalyzerInput::Silence:
        return "silence";
    case AnalyzerInput::Noise:
        return "white noise -6 dBFS";
    case AnalyzerInput::Impulse:
        return "impulse";
    case AnalyzerInput::Square:
        return "full-scale square";
    case AnalyzerInput::Denormal:
        return "denormal noise (1e-30)";
    case AnalyzerInput::NonFinite:
        return "NaN / Inf samples";
    }
    return "?";
}

// Feeds an analyzer instance chunk by chunk and keeps the stream clock.
struct AnalyzerDriver {
    AnalyzerDriver(ModuleInstance& m, uint32_t seed) : inst(m), rng(seed), l(4096), r(4096) {}

    bool chunk(AnalyzerInput sig, uint32_t n, uint32_t dropped = 0) {
        const bool special = sig == AnalyzerInput::NonFinite;
        fillSignal(special ? Signal::Noise : static_cast<Signal>(static_cast<int>(sig)), rng, l.data(),
                   r.data(), n, clock);
        if (special && n >= 4) {
            l[1] = std::numeric_limits<float>::quiet_NaN();
            r[2] = std::numeric_limits<float>::infinity();
            l[3] = -std::numeric_limits<float>::infinity();
        }
        const bool ok = inst.callAnalyze(l.data(), r.data(), n, clock, dropped, DSPTB_TAP_POST);
        clock += n + dropped;
        return ok;
    }

    ModuleInstance& inst;
    std::mt19937 rng;
    std::vector<float> l, r;
    uint64_t clock = 0;
};

void checkAnalyzer(ModulePackage& pkg, const dsptb_module_desc& d, std::vector<ValidationCheck>& out) {
    Checker chk{out};
    SimpleModuleHost host;
    std::string err;
    constexpr uint32_t kMaxChunk = 4096;

    // --- lifecycle matrix: random chunk sizes, an occasional dropped-frames report, draw in between ---
    {
        std::string failure;
        for (double sr : {44100.0, 48000.0, 96000.0})
            for (uint32_t chunk : {32u, 1024u, kMaxChunk}) {
                if (!failure.empty())
                    break;
                auto inst = make(pkg, d, host, err);
                if (!inst || !inst->prepare(sr, kMaxChunk, false, err)) {
                    failure = "create/prepare failed: " + err;
                    break;
                }
                AnalyzerDriver drv(*inst, 7);
                RecordingCanvas canvas;
                std::mt19937 sizes(11);
                std::uniform_int_distribution<uint32_t> pick(1, chunk);
                for (int i = 0; i < 200 && failure.empty(); ++i) {
                    const uint32_t dropped = (i % 50 == 25) ? 777u : 0u;
                    if (!drv.chunk(AnalyzerInput::Noise, pick(sizes), dropped))
                        failure = std::to_string(static_cast<int>(sr)) + " Hz, chunk " +
                                  std::to_string(chunk) + ": analyze() crashed";
                    if (i % 20 == 0) {
                        std::string problem;
                        inst->draw(canvas.api());
                        if (inst->faulted())
                            failure = "draw() crashed while analysing";
                        else if (!canvas.finishFrame(problem))
                            failure = "draw(): " + problem;
                    }
                }
            }
        chk.add("lifecycle (3 rates x 3 chunk sizes, 200 random chunks, dropped frames, draw in between)",
                failure.empty(), failure);
    }

    // --- create/destroy 50x, memory growth under 1 MB ---
    {
        for (int i = 0; i < 5; ++i) {
            auto m = make(pkg, d, host, err);
            if (m)
                m->prepare(48000.0, kMaxChunk, false, err);
        }
        const size_t before = privateBytes();
        bool created = true;
        for (int i = 0; i < 50 && created; ++i) {
            auto m = make(pkg, d, host, err);
            created = m && m->prepare(48000.0, kMaxChunk, false, err);
        }
        const size_t after = privateBytes();
        const double grewMb = (static_cast<double>(after) - static_cast<double>(before)) / (1024.0 * 1024.0);
        chk.add("create/destroy x50 does not leak (< 1 MB growth)", created && grewMb < 1.0,
                created ? "private bytes grew by " + std::to_string(grewMb) + " MB"
                        : "create/prepare failed: " + err);
    }

    // --- inputs, including NaN/Inf: the module may not crash or fault ---
    for (AnalyzerInput sig : {AnalyzerInput::Silence, AnalyzerInput::Noise, AnalyzerInput::Impulse,
                              AnalyzerInput::Square, AnalyzerInput::Denormal, AnalyzerInput::NonFinite}) {
        auto m = make(pkg, d, host, err);
        bool ok = m && m->prepare(48000.0, kMaxChunk, false, err);
        if (ok) {
            AnalyzerDriver drv(*m, 5);
            for (int i = 0; i < 100 && ok; ++i)
                ok = drv.chunk(sig, 480);
            // and a normal signal afterwards: the module must have recovered
            for (int i = 0; i < 20 && ok; ++i)
                ok = drv.chunk(AnalyzerInput::Noise, 480);
        }
        chk.add(std::string("input: ") + analyzerInputName(sig) + " -> no crash", ok && !m->faulted(),
                ok ? "" : "crash");
    }

    // --- re-prepare at another sample rate while it has state ---
    {
        auto m = make(pkg, d, host, err);
        bool ok = m && m->prepare(44100.0, kMaxChunk, false, err);
        if (ok) {
            AnalyzerDriver drv(*m, 19);
            for (int i = 0; i < 30 && ok; ++i)
                ok = drv.chunk(AnalyzerInput::Noise, 1024);
            ok = ok && m->prepare(96000.0, kMaxChunk, false, err);
            AnalyzerDriver again(*m, 21);
            for (int i = 0; i < 30 && ok; ++i)
                ok = again.chunk(AnalyzerInput::Noise, 1024);
        }
        chk.add("prepare() again at a new sample rate keeps working", ok && !m->faulted(),
                ok ? "" : "crash after re-prepare: " + err);
    }

    // --- parameter fuzz while analysing ---
    {
        auto m = make(pkg, d, host, err);
        bool ok = m && m->prepare(48000.0, kMaxChunk, false, err);
        std::string detail;
        if (ok) {
            AnalyzerDriver drv(*m, 17);
            std::mt19937 rng(23);
            for (int b = 0; b < 300 && ok; ++b) {
                for (uint32_t p = 0; p < m->params().size(); ++p) {
                    const auto& pd = m->params().desc(p);
                    std::uniform_real_distribution<float> u(0.0f, 1.0f);
                    float v;
                    if ((pd.flags & DSPTB_PARAM_LOG) && pd.min_value > 0.0f)
                        v = pd.min_value * std::pow(pd.max_value / pd.min_value, u(rng));
                    else
                        v = pd.min_value + (pd.max_value - pd.min_value) * u(rng);
                    m->params().set(p, v);
                }
                ok = drv.chunk(AnalyzerInput::Noise, 1024);
                if (!ok)
                    detail = "chunk " + std::to_string(b);
            }
            for (uint32_t p = 0; p < m->params().size() && ok; ++p) {
                const auto& pd = m->params().desc(p);
                if (pd.type != DSPTB_PARAM_BOOL && pd.type != DSPTB_PARAM_ENUM)
                    continue;
                for (int v = static_cast<int>(pd.min_value); v <= static_cast<int>(pd.max_value) && ok; ++v) {
                    m->params().set(p, static_cast<float>(v));
                    for (int i = 0; i < 6 && ok; ++i)
                        ok = drv.chunk(AnalyzerInput::Noise, 1024);
                    if (!ok)
                        detail = std::string("parameter ") + pd.id + " = " + std::to_string(v);
                }
            }
        }
        chk.add("parameter fuzz while analysing", ok && !m->faulted(), detail.empty() ? err : detail);
    }

    // --- cost: 10 s of stereo noise at 48 kHz must analyse in well under real time ---
    {
        auto m = make(pkg, d, host, err);
        bool ok = m && m->prepare(48000.0, kMaxChunk, false, err);
        double seconds = 0.0;
        if (ok) {
            AnalyzerDriver drv(*m, 29);
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 480000 / 1024 && ok; ++i)
                ok = drv.chunk(AnalyzerInput::Noise, 1024);
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        // The signal generator is part of the time, and Debug builds are slow: 25 % of real time is generous.
        chk.add("analyses 10 s of audio in under 2.5 s", ok && seconds < 2.5,
                "took " + std::to_string(seconds) + " s");
    }
}

// An offline module gets the whole file on a worker thread, and must honour cancel and report progress.
void checkOfflineJob(ModulePackage& pkg, const dsptb_module_desc& d, std::vector<ValidationCheck>& out) {
    Checker chk{out};
    SimpleModuleHost host;
    std::string err;

    const auto signal = [](double sr, double seconds, double hz, float amp) {
        std::vector<float> s(static_cast<size_t>(sr * seconds));
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = amp * std::sin(static_cast<float>(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / sr));
        return s;
    };
    const auto runOn = [&](ModuleInstance& inst, const std::vector<float>& mono, double sr,
                           std::atomic<float>* progress, const std::atomic<bool>* cancel, int& result) {
        const float* ch[2] = {mono.data(), mono.data()};
        return inst.callRunOffline(ch, 2, mono.size(), sr, progress, cancel, result);
    };

    // --- every rate, and a silent and an empty file ---
    {
        std::string failure;
        for (double sr : {44100.0, 48000.0, 96000.0}) {
            auto inst = make(pkg, d, host, err);
            if (!inst || !inst->prepare(sr, kOfflineMaxBlock, true, err)) {
                failure = "could not create/prepare: " + err;
                break;
            }
            for (const auto& mono : {signal(sr, 2.0, 440.0, 0.5f), signal(sr, 2.0, 440.0, 0.0f), std::vector<float>{}}) {
                int result = 1;
                if (!runOn(*inst, mono, sr, nullptr, nullptr, result))
                    failure = "crashed at " + std::to_string(sr) + " Hz";
                else if (result != 0)
                    failure = "reported a failure at " + std::to_string(sr) + " Hz";
            }
        }
        chk.add("runs at 44.1, 48 and 96 kHz on a tone, silence and an empty file", failure.empty(), failure);
    }

    // --- progress stays in range and reaches the end, and two runs agree ---
    {
        auto inst = make(pkg, d, host, err);
        const auto mono = signal(48000.0, 5.0, 1000.0, 0.5f);
        std::atomic<float> progress{0.0f};
        int result = 1;
        const bool ok = inst && inst->prepare(48000.0, kOfflineMaxBlock, true, err) &&
                        runOn(*inst, mono, 48000.0, &progress, nullptr, result) && result == 0;
        chk.add("reports progress up to the end", ok && progress.load() > 0.5f,
                "progress reached " + std::to_string(progress.load()));
        std::string first, second;
        if (ok && inst->dumpFrames(first)) {
            runOn(*inst, mono, 48000.0, nullptr, nullptr, result);
            inst->dumpFrames(second);
            chk.add("two runs give the same result", first == second, "the frame dumps differ");
        }
    }

    // --- cancel stops a long file quickly ---
    {
        auto inst = make(pkg, d, host, err);
        const auto mono = signal(48000.0, 60.0, 440.0, 0.5f);
        std::atomic<bool> cancel{true};
        int result = 0;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = inst && inst->prepare(48000.0, kOfflineMaxBlock, true, err) &&
                        runOn(*inst, mono, 48000.0, nullptr, &cancel, result);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        chk.add("cancel stops a 60 s file within 100 ms", ok && ms <= 100.0,
                "took " + std::to_string(ms) + " ms");
    }

    // --- parameter fuzz ---
    {
        std::string failure;
        std::mt19937 rng(0x0ff11e);
        auto inst = make(pkg, d, host, err);
        const auto mono = signal(48000.0, 2.0, 220.0, 0.5f);
        if (!inst || !inst->prepare(48000.0, kOfflineMaxBlock, true, err)) {
            failure = "could not create/prepare: " + err;
        } else {
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            for (int round = 0; round < 8 && failure.empty(); ++round) {
                for (uint32_t i = 0; i < inst->params().size(); ++i) {
                    const auto& p = inst->params().desc(i);
                    inst->params().set(i, p.min_value + unit(rng) * (p.max_value - p.min_value));
                }
                int result = 1;
                if (!runOn(*inst, mono, 48000.0, nullptr, nullptr, result))
                    failure = "crashed on round " + std::to_string(round);
            }
        }
        chk.add("random parameter values don't crash it", failure.empty(), failure);
    }
}

void checkCommon(ModulePackage& pkg, const dsptb_module_desc& d, std::vector<ValidationCheck>& out) {
    Checker chk{out};
    SimpleModuleHost host;
    std::string err;
    auto inst = make(pkg, d, host, err);
    if (!inst || !inst->prepare(48000.0, 512, false, err)) {
        chk.add("state and draw checks", false, "could not create/prepare: " + err);
        return;
    }
    // --- state round trip ---
    {
        std::string a, b;
        const bool saved = inst->saveState(a);
        if (!saved && !d.save_state) {
            chk.add("save_state -> load_state round-trips", true);
        } else {
            const bool loaded = saved && inst->loadState(a);
            const bool again = loaded && inst->saveState(b);
            chk.add("save_state -> load_state round-trips", saved && loaded && again && a == b,
                    saved ? "state changed: " + a + " -> " + b : "save_state failed");
        }
    }
    // --- draw against a recording canvas, 10 frames ---
    if (d.draw) {
        RecordingCanvas canvas;
        bool ok = true;
        std::string problem;
        for (int f = 0; f < 10 && ok; ++f) {
            inst->draw(canvas.api());
            ok = !inst->faulted() && canvas.finishFrame(problem);
        }
        chk.add("draw() for 10 frames makes well-formed canvas calls", ok,
                inst->faulted() ? "draw() crashed" : problem);
    } else {
        chk.add("draw() for 10 frames makes well-formed canvas calls", true);
    }
}

} // namespace

ValidationReport validateModulePackage(ModuleRegistry& registry, const fs::path& target) {
    ValidationReport rep;
    rep.path = [&] {
        const auto u = target.u8string();
        return std::string(u.begin(), u.end());
    }();
    Checker pkgChk{rep.packageChecks};

    std::error_code ec;
    const fs::path folder = fs::is_directory(target, ec) ? target : target.parent_path();
    std::string err;
    ModulePackage* pkg = registry.addPackage(folder, err);
    if (!pkg) {
        pkgChk.add("manifest (module.json)", false, err);
        return rep;
    }
    rep.packageId = pkg->manifest.id;
    pkgChk.add("manifest (module.json)", true);
    if (pkg->status == PackageStatus::NeedsGpuRuntime) {
        rep.skipped = true;
        rep.skipReason = pkg->statusText;
        return rep;
    }
    if (!fs::is_directory(target, ec) && fs::path(target).filename() != fs::path(pkg->manifest.dll)) {
        pkgChk.add("DLL named in module.json", false,
                   "module.json names " + pkg->manifest.dll + ", but " + target.filename().string() +
                       " was given");
        return rep;
    }

    if (pkg->guardReset)
        pkg->guardReset();
    const bool ok = registry.ensureLoaded(*pkg);
    pkgChk.add(std::string("ABI ") + std::to_string(DSPTB_ABI_VERSION) +
                   " and descriptors match the manifest",
               ok, std::string(statusLabel(pkg->status)) + ": " + pkg->statusText);
    if (!ok)
        return rep;
    if (pkg->guardReset)
        pkg->guardReset();
#if defined(DSPTB_RT_GUARD)
    rep.rtGuardActive = pkg->guardViolations != nullptr;
    pkgChk.add("module exports the allocation-guard counter (Debug builds)", pkg->guardViolations != nullptr,
               "add DSPTB_REGISTER_MODULES(...) from <dsptb/sdk.hpp>");
#endif

    for (const dsptb_module_desc* d : pkg->descs) {
        ModuleValidation mv;
        mv.moduleId = d->id;
        mv.kind = kindName(d->kind);
        Checker chk{mv.checks};
        chk.add("descriptor, parameters and required callbacks", validateDescriptor(*d).empty(),
                validateDescriptor(*d));
        if (d->kind == DSPTB_KIND_PROCESSOR)
            checkProcessor(*pkg, *d, mv.checks);
        else if (d->kind == DSPTB_KIND_ANALYZER)
            checkAnalyzer(*pkg, *d, mv.checks);
        else
            checkOfflineJob(*pkg, *d, mv.checks);
        checkCommon(*pkg, *d, mv.checks);
        rep.modules.push_back(std::move(mv));
    }
    if (pkg->guardViolations)
        rep.rtGuardHits = pkg->guardViolations();
    return rep;
}

std::string ValidationReport::toText() const {
    std::ostringstream os;
    os << "Validating " << path << "\n";
    const auto line = [&](const ValidationCheck& c, const char* indent) {
        os << indent << (c.passed ? "[ ok ] " : "[FAIL] ") << c.name;
        if (!c.passed && !c.detail.empty())
            os << " -- " << c.detail;
        os << "\n";
    };
    for (const auto& c : packageChecks)
        line(c, "  ");
    for (const auto& m : modules) {
        os << "  module " << m.moduleId << " (" << m.kind << ")\n";
        for (const auto& c : m.checks)
            line(c, "    ");
    }
    if (rtGuardActive || rtGuardHits)
        os << "  RT guard hits: " << rtGuardHits << (rtGuardHits ? "  [FAIL]\n" : "\n");
    if (skipped)
        os << "SKIPPED: needs the GPU runtime (" << skipReason << ")\n";
    else
        os << (passed() ? "PASS" : "FAIL") << "\n";
    return os.str();
}

std::string ValidationReport::toJson() const {
    nlohmann::json j;
    j["path"] = path;
    j["package"] = packageId;
    j["passed"] = passed();
    j["skipped"] = skipped;
    if (skipped)
        j["skipReason"] = skipReason;
    j["rtGuardActive"] = rtGuardActive;
    j["rtGuardHits"] = rtGuardHits;
    const auto checks = [](const std::vector<ValidationCheck>& v) {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& c : v)
            a.push_back({{"name", c.name}, {"passed", c.passed}, {"detail", c.detail}});
        return a;
    };
    j["packageChecks"] = checks(packageChecks);
    j["modules"] = nlohmann::json::array();
    for (const auto& m : modules)
        j["modules"].push_back(
            {{"id", m.moduleId}, {"kind", m.kind}, {"passed", m.passed()}, {"checks", checks(m.checks)}});
    return j.dump(2);
}

} // namespace dsptb
