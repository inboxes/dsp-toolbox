// The analysis host, analyzer instances in the SessionModel, and the Spectrogram and Level Meter
// acceptance tables run through the real module DLLs.
#include <doctest/doctest.h>

#include <dsptb/core/analysis_host.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/engine.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/offline_analyze.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/signal_gen.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>

using namespace dsptb;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 256;

ModuleRegistry& registry() {
    static ModuleRegistry reg = [] {
        ModuleRegistry r;
        r.scan({fs::path(DSPTB_MODULES_DIR), fs::path(DSPTB_TEST_MODULES_DIR)});
        return r;
    }();
    return reg;
}

AudioBuffer signal(SignalType type, double freq, double dbfs, double seconds, uint32_t rate = kRate,
                   double phaseDeg = 0.0, double freqEnd = 20000.0) {
    GenParams p;
    p.type = type;
    p.freq = freq;
    p.freqEnd = freqEnd;
    p.dbfs = dbfs;
    p.seconds = seconds;
    p.sampleRate = rate;
    p.phaseDeg = phaseDeg;
    return pcmToStereo(generateSignal(p));
}

AudioBuffer sine(double freq, double dbfs, double seconds, uint32_t rate = kRate, double phaseDeg = 0.0) {
    return signal(SignalType::Sine, freq, dbfs, seconds, rate, phaseDeg);
}

AudioBuffer concat(const std::vector<AudioBuffer>& parts) {
    AudioBuffer out;
    out.sampleRate = parts.front().sampleRate;
    for (const auto& p : parts)
        for (int c = 0; c < 2; ++c)
            out.ch[c].insert(out.ch[c].end(), p.ch[c].begin(), p.ch[c].end());
    return out;
}

using Params = std::vector<std::pair<std::string, float>>;

// Runs the module over `in` the way `dsptb-cli analyze --module ... --dump-frames` does, and parses the dump.
json dump(const char* module, const AudioBuffer& in, const Params& params = {}) {
    AnalyzeModuleOptions opt;
    opt.moduleId = module;
    opt.params = params;
    std::string out, err;
    INFO(err);
    REQUIRE(analyzeWithModule(registry(), opt, in, out, err));
    return json::parse(out);
}

// ----------------------------------------------------------------------------------------------------------
// Rig: Engine + SessionModel on the NullBackend, driven by hand. The analysis thread is off; the test calls
// pumpOnce() so every result is deterministic.
// ----------------------------------------------------------------------------------------------------------
struct AnalyzerRig {
    explicit AnalyzerRig(bool micCapture = false, double seconds = 20.0)
        : engine(options()), model(engine, registry(), /*analysisThread=*/false) {
        AudioConfig cfg;
        cfg.backend = BackendKind::Null;
        cfg.sampleRate = kRate;
        cfg.bufferFrames = kBlock;
        cfg.capture = micCapture;
        std::string err;
        REQUIRE(engine.open(cfg, err));
        nb = engine.nullBackend();
        REQUIRE(nb != nullptr);
        nb->setRealtime(false);
        nb->setCaptureOutput(true, static_cast<size_t>(kRate * seconds));
        REQUIRE(engine.start(err));
    }
    static EngineOptions options() {
        EngineOptions o;
        o.declickMs = 0.0;
        return o;
    }
    void useFile(AudioBuffer audio) {
        auto f = std::make_shared<LoadedFile>();
        f->path = "<generated>";
        f->overview = computeOverview(audio);
        f->seconds = static_cast<double>(audio.frames()) / kRate;
        f->audio = std::move(audio);
        engine.setFile(f);
        engine.play();
    }
    void run(size_t blocks, bool pumpAnalysis = true) {
        for (size_t b = 0; b < blocks; ++b) {
            nb->pump(kBlock, kBlock);
            now += static_cast<double>(kBlock) / kRate;
            model.tick(now, messages);
            if (pumpAnalysis)
                model.analysis().pumpOnce();
        }
    }
    InstanceId add(const char* module) {
        std::string err;
        const InstanceId id = model.add(module, err);
        INFO(err);
        REQUIRE(id != 0);
        return id;
    }
    json probe(InstanceId id) {
        InstanceRecord* rec = model.find(id);
        REQUIRE(rec != nullptr);
        std::string text;
        REQUIRE(rec->instance->dumpFrames(text));
        return json::parse(text);
    }
    bool messageContains(const std::string& s) const {
        return std::any_of(messages.begin(), messages.end(),
                           [&](const SessionMessage& m) { return m.text.find(s) != std::string::npos; });
    }
    static double rmsDb(double sumSq, double frames) {
        return 10.0 * std::log10(std::max(1e-30, sumSq / std::max(1.0, frames)));
    }

    Engine engine;
    SessionModel model;
    NullBackend* nb = nullptr;
    double now = 0.0;
    std::vector<SessionMessage> messages;
};

} // namespace

// ----------------------------------------------------------------------------------------------------------
// Spectrogram acceptance tests (spectrogram.md)
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("spectrogram: 1 kHz sine at -6 dBFS peaks at 1 kHz and -6 dB (fft 4096, 48 kHz)") {
    const json j = dump("org.dsptb.spectrogram", sine(1000.0, -6.0, 5.0));
    const double binHz = j["binHz"].get<double>();
    CHECK(binHz == doctest::Approx(11.71875));
    REQUIRE(j["frames"].size() > 100);
    double worstHz = 0.0, worstDb = 0.0;
    for (const auto& f : j["frames"]) {
        worstHz = std::max(worstHz, std::fabs(f["peakHz"].get<double>() - 1000.0));
        worstDb = std::max(worstDb, std::fabs(f["peakDb"].get<double>() - (-6.0)));
    }
    MESSAGE("worst peak-frequency error " << worstHz << " Hz (bin " << binHz << " Hz), worst level error "
                                          << worstDb << " dB");
    CHECK(worstHz <= binHz);
    CHECK(worstDb <= 0.5);
}

TEST_CASE("spectrogram: other FFT sizes keep the peak within one bin") {
    for (float sizeIndex : {0.0f, 2.0f, 4.0f}) { // 512, 2048, 8192
        const json j = dump("org.dsptb.spectrogram", sine(3000.0, -12.0, 3.0), {{"fft_size", sizeIndex}});
        const double binHz = j["binHz"].get<double>();
        REQUIRE(j["frames"].size() > 10);
        for (const auto& f : j["frames"]) {
            CHECK(std::fabs(f["peakHz"].get<double>() - 3000.0) <= binHz);
            CHECK(std::fabs(f["peakDb"].get<double>() - (-12.0)) <= 0.5);
        }
    }
}

TEST_CASE("spectrogram: a 20 Hz to 20 kHz sweep over 10 s rises monotonically") {
    const json j =
        dump("org.dsptb.spectrogram", signal(SignalType::Sweep, 20.0, -6.0, 10.0, kRate, 0.0, 20000.0));
    const auto& frames = j["frames"];
    REQUIRE(frames.size() > 300);
    size_t violations = 0;
    double prev = 0.0;
    for (const auto& f : frames) {
        const double hz = f["peakHz"].get<double>();
        if (prev > 0.0 && hz < prev * 0.98) // allow 2 % jitter
            ++violations;
        prev = std::max(prev, hz);
    }
    CHECK(violations == 0);
    CHECK(frames.front()["peakHz"].get<double>() < 200.0);
    CHECK(frames.back()["peakHz"].get<double>() > 15000.0);
}

TEST_CASE("spectrogram: analysis time per hop at fft 4096") {
    const AudioBuffer noise = signal(SignalType::Noise, 0.0, -20.0, 20.0);
    AnalyzeModuleOptions opt;
    opt.moduleId = "org.dsptb.spectrogram";
    std::string out, err;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(analyzeWithModule(registry(), opt, noise, out, err));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double hops = static_cast<double>(json::parse(out)["frames"].size());
    const double msPerHop = 1000.0 * seconds / hops;
    MESSAGE("spectrogram: " << msPerHop << " ms per hop (" << hops << " hops, 20 s of noise)");
#ifdef NDEBUG
    CHECK(msPerHop < 0.2); // spectrogram.md: under 0.2 ms per hop at 4096 (bench, Release)
#else
    CHECK(msPerHop < 2.0); // Debug builds are not representative; Release asserts the real budget
#endif
}

// ----------------------------------------------------------------------------------------------------------
// Level Meter acceptance tests (level-meter.md, EBU Tech 3341 minimal set)
// ----------------------------------------------------------------------------------------------------------

namespace {
bool within(double value, double target, double tolerance) {
    return std::fabs(value - target) <= tolerance;
}
} // namespace

TEST_CASE("levelmeter: stereo 1 kHz sine at -23 dBFS reads -23 LUFS (M, S and I)") {
    const json j = dump("org.dsptb.levelmeter", sine(1000.0, -23.0, 20.0));
    CHECK(within(j["integratedLufs"].get<double>(), -23.0, 0.1));
    CHECK(within(j["momentaryLufs"].get<double>(), -23.0, 0.1));
    CHECK(within(j["shortTermLufs"].get<double>(), -23.0, 0.1));
}

TEST_CASE("levelmeter: the same at -33 dBFS reads -33 LUFS") {
    const json j = dump("org.dsptb.levelmeter", sine(1000.0, -33.0, 20.0));
    CHECK(within(j["integratedLufs"].get<double>(), -33.0, 0.1));
}

TEST_CASE("levelmeter: gating (10 s at -36, 60 s at -23, 10 s at -36) integrates to -23 LUFS") {
    const AudioBuffer in =
        concat({sine(1000.0, -36.0, 10.0), sine(1000.0, -23.0, 60.0), sine(1000.0, -36.0, 10.0)});
    const json j = dump("org.dsptb.levelmeter", in);
    CHECK(within(j["integratedLufs"].get<double>(), -23.0, 0.1));
}

TEST_CASE("levelmeter: 44.1, 48 and 96 kHz all read -23 LUFS") {
    for (uint32_t rate : {44100u, 48000u, 96000u}) {
        const json j = dump("org.dsptb.levelmeter", sine(1000.0, -23.0, 20.0, rate));
        INFO("sample rate " << rate);
        CHECK(within(j["integratedLufs"].get<double>(), -23.0, 0.1));
    }
}

TEST_CASE("levelmeter: true peak of fs/4 at 45 degrees (sample peak -3.01 dBFS, true peak 0 dBTP)") {
    // Amplitude 1.0 at fs/4 with a 45 degree phase: every sample is +-0.707, but the waveform between them
    // peaks at 1.0.
    const json j = dump("org.dsptb.levelmeter", sine(12000.0, 0.0, 5.0, kRate, 45.0));
    CHECK(within(j["samplePeakDbfs"][0].get<double>(), -3.01, 0.05));
    CHECK(within(j["truePeakDbtp"][0].get<double>(), 0.0, 0.3));
    CHECK(within(j["truePeakDbtp"][1].get<double>(), 0.0, 0.3));
}

TEST_CASE("levelmeter: peak, RMS and the clip lamp read sensibly") {
    const json j = dump("org.dsptb.levelmeter", sine(1000.0, -6.0, 5.0));
    CHECK(within(j["rmsDbfs"][0].get<double>(), -9.0, 0.3)); // sine RMS is 3 dB under its peak
    CHECK(j["peakDbfs"][0].get<double>() > -6.5);
    CHECK(j["clip"][0].get<int>() == 0);
    const json hot = dump("org.dsptb.levelmeter", sine(1000.0, 0.0, 1.0)); // peaks reach 1.0
    CHECK(hot["clip"][0].get<int>() == 1);
}

TEST_CASE("analyze --module: unknown module, wrong kind and unknown parameter are errors") {
    AnalyzeModuleOptions opt;
    std::string out, err;
    opt.moduleId = "no.such.module";
    CHECK_FALSE(analyzeWithModule(registry(), opt, sine(1000.0, -20.0, 1.0), out, err));
    opt.moduleId = "org.dsptb.gain";
    CHECK_FALSE(analyzeWithModule(registry(), opt, sine(1000.0, -20.0, 1.0), out, err));
    CHECK(err.find("not an analyzer") != std::string::npos);
    opt.moduleId = "org.dsptb.spectrogram";
    opt.params = {{"no_such_param", 1.0f}};
    CHECK_FALSE(analyzeWithModule(registry(), opt, sine(1000.0, -20.0, 1.0), out, err));
    CHECK(err.find("no_such_param") != std::string::npos);
}

// ----------------------------------------------------------------------------------------------------------
// Analysis host and analyzer instances in the session
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("routes: a seek resets the chain only when the file feeds it") {
    for (const bool micInChain : {false, true}) {
        CAPTURE(micInChain);
        AnalyzerRig rig(/*micCapture=*/true, 4.0);
        rig.engine.setSource(micInChain ? SourceKind::Mic : SourceKind::File);
        rig.engine.setOutputRoute(SourceKind::File, micInChain);
        const InstanceId id = rig.add("test.reset-counter");
        rig.useFile(sine(1000.0, -12.0, 2.0));
        rig.run(8);
        const uint64_t before = rig.probe(id)["resets"].get<uint64_t>();
        rig.engine.seekFrames(24000);
        rig.run(8);
        CHECK(rig.probe(id)["resets"].get<uint64_t>() - before == (micInChain ? 0u : 1u));
        CHECK(rig.engine.positionFrames() > 24000);
    }
}

TEST_CASE("analyzers: an analyzer on PRE receives the microphone while monitoring is off") {
    AnalyzerRig rig(/*micCapture=*/true, 4.0);
    rig.engine.setSource(SourceKind::Mic);
    rig.engine.setMonitorMic(false);
    const AudioBuffer tone = sine(1000.0, -12.0, 1.0);
    std::vector<float> mic(tone.frames() * 2);
    for (size_t i = 0; i < tone.frames(); ++i) {
        mic[2 * i] = tone.ch[0][i];
        mic[2 * i + 1] = tone.ch[1][i];
    }
    rig.nb->setInput(mic);

    const InstanceId id = rig.add("test.probe");
    REQUIRE(rig.model.setTap(id, DSPTB_TAP_PRE));
    rig.run(400); // 100 k frames, a little over 2 s

    const json p = rig.probe(id);
    const double frames = p["frames"].get<double>();
    CHECK(frames > 90000.0);
    CHECK(p["tap"].get<int>() == DSPTB_TAP_PRE);
    CHECK(rig.model.analysis().droppedFrames(id) == 0);
    CHECK(p["dropped"].get<int>() == 1); // only the gap marker that setTap(PRE) leaves behind
    CHECK(p["clockJumps"].get<int>() == 0);
    const double dbfs = AnalyzerRig::rmsDb(p["sumSqL"].get<double>(), frames);
    MESSAGE("probe on PRE heard the mic at " << dbfs << " dB RMS (power mean) with monitoring off");
    CHECK(within(dbfs, -15.0, 0.2)); // -12 dBFS sine: mean power -15 dB

    // Monitoring is off, so the output stays silent once the monitor gate has closed.
    const auto& out = rig.nb->captured();
    double outEnergy = 0.0;
    for (size_t i = out.size() / 2; i < out.size(); ++i) // skip the gate's initial fade
        outEnergy += static_cast<double>(out[i]) * out[i];
    CHECK(outEnergy == 0.0);
}

TEST_CASE("analyzers: PRE hears the source, POST hears it after the chain") {
    AnalyzerRig rig;
    rig.useFile(sine(1000.0, -6.0, 10.0));
    const InstanceId gain = rig.add("org.dsptb.gain");
    rig.model.setParamUndoable(gain, 0, -20.0f); // gain_db
    const InstanceId pre = rig.add("test.probe");
    const InstanceId post = rig.add("test.probe");
    REQUIRE(rig.model.setTap(pre, DSPTB_TAP_PRE));
    REQUIRE(rig.model.setTap(post, DSPTB_TAP_POST));
    rig.run(50); // a new slot fades in over 20 ms; measure after that
    const json a0 = rig.probe(pre), b0 = rig.probe(post);
    rig.run(250);

    const json a = rig.probe(pre), b = rig.probe(post);
    const auto powerDb = [](const json& now, const json& before) {
        return AnalyzerRig::rmsDb(now["sumSqL"].get<double>() - before["sumSqL"].get<double>(),
                                  now["frames"].get<double>() - before["frames"].get<double>());
    };
    const double preDb = powerDb(a, a0), postDb = powerDb(b, b0);
    MESSAGE("PRE " << preDb << " dB, POST " << postDb << " dB");
    CHECK(within(preDb - postDb, 20.0,
                 0.1)); // the Gain in between takes 20 dB off    CHECK(a["tap"].get<int>() == DSPTB_TAP_PRE);
    CHECK(b["tap"].get<int>() == DSPTB_TAP_POST);
}

TEST_CASE("analyzers: a full ring drops whole blocks and the module is told") {
    AnalyzerRig rig;
    rig.useFile(sine(440.0, -6.0, 12.0));
    const InstanceId id = rig.add("test.probe");
    rig.run(500,
            /*pumpAnalysis=*/false); // 128 000 frames written, nobody reading: the ring (65 536) overflows

    const auto ring = rig.model.analysis().ring(id);
    REQUIRE(ring != nullptr);
    const uint64_t dropped = ring->droppedFrames.load();
    const uint64_t written = ring->framesWritten.load();
    CHECK(dropped > 0);
    CHECK(dropped + written == 500u * kBlock); // every frame is either in the ring or counted as dropped
    CHECK(rig.model.analysis().droppedFrames(id) == dropped);

    rig.model.analysis().pumpOnce();
    const json p = rig.probe(id);
    CHECK(p["dropped"].get<uint64_t>() == dropped); // reported through dropped_frames
    CHECK(p["frames"].get<uint64_t>() == written);
    CHECK(p["maxChunk"].get<uint32_t>() <= AnalysisHost::kChunk);
    CHECK(p["clockJumps"].get<int>() == 0);
}

TEST_CASE("analyzers: a disabled analyzer is paused and reports a gap when it resumes") {
    AnalyzerRig rig;
    rig.useFile(sine(440.0, -6.0, 12.0));
    const InstanceId id = rig.add("test.probe");
    rig.run(100);
    const uint64_t before = rig.probe(id)["frames"].get<uint64_t>();
    CHECK(before == 100u * kBlock);

    rig.model.setEnabled(id, false);
    rig.run(4); // let the audio thread pick up the snapshot
    const uint64_t paused = rig.probe(id)["frames"].get<uint64_t>();
    rig.run(100);
    CHECK(rig.probe(id)["frames"].get<uint64_t>() == paused); // nothing arrives while it is off
    CHECK(paused <= before + 4u * kBlock);

    rig.model.setEnabled(id, true);
    rig.run(50);
    const json p = rig.probe(id);
    CHECK(p["frames"].get<uint64_t>() >= paused + 40u * kBlock);
    CHECK(p["dropped"].get<uint64_t>() >= 1); // the gap marker
}

TEST_CASE("analyzers: a paused file gives them nothing, and they carry on with no gap when it resumes") {
    AnalyzerRig rig;
    rig.useFile(sine(440.0, -6.0, 12.0));
    const InstanceId id = rig.add("test.probe");
    rig.run(100);
    const uint64_t playing = rig.probe(id)["frames"].get<uint64_t>();
    CHECK(playing == 100u * kBlock);

    rig.engine.pause();
    rig.run(100);
    CHECK(rig.probe(id)["frames"].get<uint64_t>() <= playing + kBlock);

    rig.engine.play();
    rig.run(50);
    const json p = rig.probe(id);
    CHECK(p["frames"].get<uint64_t>() >= playing + 49u * kBlock);
    CHECK(p["dropped"].get<uint64_t>() == 0);
}

TEST_CASE("analyzers: the end of the file stops them, and no file gives them nothing") {
    AnalyzerRig rig;
    const InstanceId id = rig.add("test.probe");
    rig.run(20);
    CHECK(rig.probe(id)["frames"].get<uint64_t>() == 0);

    rig.useFile(sine(440.0, -6.0, 0.5));
    rig.run(200);
    const uint64_t atEnd = rig.probe(id)["frames"].get<uint64_t>();
    CHECK(atEnd >= kRate / 2);
    CHECK(atEnd <= kRate / 2 + kBlock);
    rig.run(50);
    CHECK(rig.probe(id)["frames"].get<uint64_t>() == atEnd);
}

TEST_CASE("analyzers: the microphone keeps feeding them with no file playing") {
    AnalyzerRig rig(/*micCapture=*/true);
    rig.engine.setSource(SourceKind::Mic);
    const InstanceId id = rig.add("test.probe");
    rig.run(50);
    CHECK(rig.probe(id)["frames"].get<uint64_t>() == 50u * kBlock);
}

TEST_CASE("analyzers: they are not part of the chain order, so reordering processors ignores them") {
    AnalyzerRig rig;
    rig.useFile(sine(440.0, -6.0, 12.0));
    const InstanceId g1 = rig.add("org.dsptb.gain");
    const InstanceId probe = rig.add("test.probe");
    const InstanceId g2 = rig.add("org.dsptb.gain");
    REQUIRE(rig.model.instances().size() == 3);

    REQUIRE(rig.model.move(g2, 0)); // index 0 means "first processor"
    rig.run(20);
    std::vector<InstanceId> processors;
    for (const auto& r : rig.model.instances())
        if (!r.isAnalyzer())
            processors.push_back(r.id);
    REQUIRE(processors.size() == 2);
    CHECK(processors[0] == g2);
    CHECK(processors[1] == g1);
    CHECK_FALSE(rig.model.move(probe, 0)); // an analyzer cannot be moved
    rig.run(10);
    CHECK(rig.probe(probe)["frames"].get<uint64_t>() > 0); // and it kept listening throughout
}

TEST_CASE("analyzers: adding and removing them while audio runs is safe") {
    AnalyzerRig rig;
    rig.useFile(sine(440.0, -6.0, 12.0));
    for (int i = 0; i < 40; ++i) {
        const InstanceId id = rig.add(i % 2 ? "org.dsptb.spectrogram" : "org.dsptb.levelmeter");
        rig.run(3);
        REQUIRE(rig.model.remove(id));
        rig.run(2);
    }
    CHECK(rig.model.instances().empty());
    rig.run(10);
    CHECK_FALSE(rig.messageContains("faulted"));
}

TEST_CASE("analyzers: an analyzer with CAP_OWN_THREAD runs on a thread of its own") {
    AnalysisHost host(/*startThread=*/true);
    SimpleModuleHost simple;
    std::string err;
    ModulePackage* pkg = nullptr;
    const auto make = [&](const char* moduleId, InstanceId id) {
        const dsptb_module_desc* d = registry().descriptor(moduleId, &pkg, err);
        REQUIRE(d != nullptr);
        auto inst = ModuleInstance::create(id, *pkg, *d, &simple, err);
        REQUIRE(inst != nullptr);
        REQUIRE(inst->prepare(kRate, AnalysisHost::kChunk, false, err));
        return inst;
    };
    auto shared = make("test.probe", 1);
    auto own = make("test.probe-thread", 2);
    const auto ringA = host.attach(shared, DSPTB_TAP_POST, kRate);
    const auto ringB = host.attach(own, DSPTB_TAP_POST, kRate);

    std::vector<float> block(1024 * 2, 0.25f);
    for (int i = 0; i < 20; ++i) {
        ringA->ring.write(block.data(), block.size());
        ringA->framesWritten.fetch_add(1024);
        ringB->ring.write(block.data(), block.size());
        ringB->framesWritten.fetch_add(1024);
    }
    for (int i = 0; i < 400 && (host.analyzedFrames(1) < 20480 || host.analyzedFrames(2) < 20480); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(host.analyzedFrames(1) == 20480u);
    CHECK(host.analyzedFrames(2) == 20480u);

    host.detach(1); // stops the threads (the own one is joined) before the probes are read
    host.detach(2);
    std::string a, b;
    REQUIRE(shared->dumpFrames(a));
    REQUIRE(own->dumpFrames(b));
    const json ja = json::parse(a), jb = json::parse(b);
    CHECK(ja["threadId"].get<uint64_t>() != 0);
    CHECK(jb["threadId"].get<uint64_t>() != 0);
    CHECK(ja["threadId"].get<uint64_t>() != jb["threadId"].get<uint64_t>());
    CHECK(ja["threadId"].get<uint64_t>() != std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

TEST_CASE("analyzers: the real analysis thread analyses a running session") {
    // The rest of this file pumps the analysis by hand; here the shared thread does it, as in the app.
    Engine engine(AnalyzerRig::options());
    SessionModel model(engine, registry());
    AudioConfig cfg;
    cfg.backend = BackendKind::Null;
    cfg.sampleRate = kRate;
    cfg.bufferFrames = kBlock;
    std::string err;
    REQUIRE(engine.open(cfg, err));
    NullBackend* nb = engine.nullBackend();
    nb->setRealtime(false);
    REQUIRE(engine.start(err));
    auto f = std::make_shared<LoadedFile>();
    f->audio = sine(440.0, -6.0, 10.0);
    f->overview = computeOverview(f->audio);
    f->seconds = 10.0;
    f->path = "<generated>";
    engine.setFile(f);
    engine.play();

    const InstanceId id = model.add("org.dsptb.spectrogram", err);
    INFO(err);
    REQUIRE(id != 0);
    std::vector<SessionMessage> messages;
    for (int b = 0; b < 200; ++b) {
        nb->pump(kBlock, kBlock);
        model.tick(b * 0.005, messages);
    }
    for (int i = 0; i < 400 && model.analysis().analyzedFrames(id) < 40000; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(model.analysis().analyzedFrames(id) >= 40000u);
    CHECK(messages.empty());
}