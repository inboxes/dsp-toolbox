// Module system integration tests: the real Gain DLL, the test-only fault/allocation/ABI fixtures, the engine
// and SessionModel running through the NullBackend in manual (pump) mode, and the CLI.
#include <doctest/doctest.h>

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/engine.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/offline_render.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/signal_gen.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace dsptb;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 256;

// One registry for the whole test run: DLLs stay loaded until exit anyway.
ModuleRegistry& registry() {
    static ModuleRegistry reg = [] {
        ModuleRegistry r;
        r.scan({fs::path(DSPTB_MODULES_DIR), fs::path(DSPTB_TEST_MODULES_DIR)});
        return r;
    }();
    return reg;
}

AudioBuffer sineBuffer(double freq, double dbfs, double seconds) {
    GenParams p;
    p.freq = freq;
    p.dbfs = dbfs;
    p.seconds = seconds;
    p.sampleRate = kRate;
    return pcmToStereo(generateSignal(p));
}

std::shared_ptr<const LoadedFile> makeFile(AudioBuffer audio) {
    auto f = std::make_shared<LoadedFile>();
    f->path = "<generated>";
    f->overview = computeOverview(audio);
    f->seconds = static_cast<double>(audio.frames()) / kRate;
    f->audio = std::move(audio);
    return f;
}

// Renders `in` through a Gain session given as the params object of one instance.
AudioBuffer renderGain(const AudioBuffer& in, const std::string& paramsJson, bool enabled = true) {
    const std::string text =
        std::string(
            R"({"format":"dsptb-session","version":1,"instances":[{"module":"org.dsptb.gain","enabled":)") +
        (enabled ? "true" : "false") + R"(,"params":)" + paramsJson + "}]}";
    SessionDescription desc;
    std::string err;
    REQUIRE(parseSessionDescription(text, desc, err));
    RenderOptions opt;
    opt.tailSeconds = 0.0;
    AudioBuffer out;
    REQUIRE(renderOffline(registry(), desc, in, opt, out, err));
    return out;
}

double maxAbsDiff(const AudioBuffer& a, const AudioBuffer& b, double scale) {
    REQUIRE(a.frames() == b.frames());
    double m = 0.0;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < a.frames(); ++i)
            m = std::max(
                m, std::fabs(static_cast<double>(a.ch[c][i]) - scale * static_cast<double>(b.ch[c][i])));
    return m;
}

// Engine + SessionModel driven by hand: nothing runs until run() is called, and `now` is the audio clock.
struct ModelRig {
    explicit ModelRig(AudioBuffer fileAudio = sineBuffer(440.0, -6.0, 12.0))
        : engine(options()), model(engine, registry()) {
        AudioConfig cfg;
        cfg.backend = BackendKind::Null;
        cfg.sampleRate = kRate;
        cfg.bufferFrames = kBlock;
        std::string err;
        REQUIRE(engine.open(cfg, err));
        nb = engine.nullBackend();
        REQUIRE(nb != nullptr);
        nb->setRealtime(false);
        nb->setCaptureOutput(true, kRate * 12);
        REQUIRE(engine.start(err));
        engine.setFile(makeFile(std::move(fileAudio)));
    }
    static EngineOptions options() {
        EngineOptions o;
        o.declickMs = 0.0; // exact playback so the only changes are the ones the chain makes
        return o;
    }
    void run(size_t blocks) {
        for (size_t b = 0; b < blocks; ++b) {
            nb->pump(kBlock, kBlock);
            now += static_cast<double>(kBlock) / kRate;
            model.tick(now, messages);
        }
    }
    InstanceId add(const char* module) {
        std::string err;
        const InstanceId id = model.add(module, err);
        INFO(err);
        REQUIRE(id != 0);
        return id;
    }
    bool messageContains(const std::string& s) const {
        return std::any_of(messages.begin(), messages.end(),
                           [&](const SessionMessage& m) { return m.text.find(s) != std::string::npos; });
    }
    float left(size_t frame) const { return nb->captured()[frame * 2]; }

    Engine engine;
    SessionModel model;
    NullBackend* nb = nullptr;
    double now = 0.0;
    std::vector<SessionMessage> messages;
};

// ---- CLI helper (same approach as test_cli.cpp) ----
struct RunResult {
    int exitCode = -1;
    std::string out;
};
RunResult runCli(const std::string& args) {
    const std::string cmd = "\"\"" + std::string(DSPTB_CLI_PATH) + "\" " + args + " 2>&1\"";
    RunResult r;
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        r.out += buf;
    r.exitCode = _pclose(pipe);
    return r;
}
std::string q(const fs::path& p) {
    return "\"" + p.string() + "\"";
}

} // namespace

// ----------------------------------------------------------------------------------------------------------
// Gain
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("gain: the DLL loads, validates and exposes its parameters") {
    ModulePackage* pkg = registry().findPackage("org.dsptb.gain");
    REQUIRE(pkg != nullptr);
    CHECK(pkg->status == PackageStatus::Ok);
    std::string err;
    const dsptb_module_desc* d = registry().descriptor("org.dsptb.gain", nullptr, err);
    REQUIRE(d != nullptr);
    CHECK(pkg->loaded);
    CHECK(d->kind == DSPTB_KIND_PROCESSOR);
    REQUIRE(d->num_params == 3);
    CHECK(std::string(d->params[0].id) == "gain_db");
    CHECK(d->params[0].min_value == -60.0f);
    CHECK(d->params[0].max_value == 24.0f);
    CHECK(d->params[0].smoothing_ms == 20.0f);
    CHECK(std::string(d->params[1].id) == "invert");
    CHECK(d->params[1].type == DSPTB_PARAM_BOOL);
    CHECK(std::string(d->params[2].id) == "mute");
}

TEST_CASE("gain: gain_db = 0 is bit-identical to the input (null test)") {
    const AudioBuffer in = sineBuffer(997.0, -3.0, 2.0);
    const AudioBuffer out = renderGain(in, "{}");
    CHECK(maxAbsDiff(out, in, 1.0) == 0.0);
}

TEST_CASE("gain: gain_db = -6.0206 gives input x 0.5 within 1e-6") {
    const AudioBuffer in = sineBuffer(997.0, -3.0, 2.0);
    const AudioBuffer out = renderGain(in, R"({"gain_db":-6.0206})");
    CHECK(maxAbsDiff(out, in, 0.5) <= 1e-6);
}

TEST_CASE("gain: invert gives exactly -input") {
    const AudioBuffer in = sineBuffer(997.0, -3.0, 2.0);
    const AudioBuffer out = renderGain(in, R"({"invert":true})");
    CHECK(maxAbsDiff(out, in, -1.0) == 0.0);
}

TEST_CASE("gain: mute silences the output; a disabled instance is skipped") {
    const AudioBuffer in = sineBuffer(997.0, -3.0, 1.0);
    const AudioBuffer muted = renderGain(in, R"({"mute":true})");
    CHECK(maxAbsDiff(muted, in, 0.0) == 0.0);
    const AudioBuffer skipped = renderGain(in, R"({"gain_db":-20})", /*enabled=*/false);
    CHECK(maxAbsDiff(skipped, in, 1.0) == 0.0);
}

TEST_CASE("gain: a live gain change is smoothed, with no step larger than the sine's own") {
    ModelRig rig;
    const InstanceId g = rig.add("org.dsptb.gain");
    rig.engine.play();
    rig.run(40);
    rig.model.setParam(g, 0, -20.0f); // -20 dB jump requested between two blocks
    rig.run(40);
    const size_t n = rig.nb->captured().size() / 2;
    double maxStep = 0.0;
    for (size_t i = 1; i < n; ++i)
        maxStep = std::max(maxStep, std::fabs(static_cast<double>(rig.left(i)) - rig.left(i - 1)));
    const double sineStep = 0.5 * 2.0 * 3.14159265358979 * 440.0 / kRate; // peak 0.5
    CHECK(maxStep <= 1.5 * sineStep);
    CHECK(std::fabs(rig.left(n - 1)) <= 0.0501); // settled at -20 dB: 0.05 peak
}

// ----------------------------------------------------------------------------------------------------------
// registry and loader
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("a module whose DLL reports another ABI is listed as Incompatible ABI and is never instantiated") {
    ModulePackage* pkg = registry().findPackage("test.abimismatch");
    REQUIRE(pkg != nullptr);
    CHECK_FALSE(registry().ensureLoaded(*pkg));
    CHECK(pkg->status == PackageStatus::IncompatibleAbi);
    CHECK_FALSE(pkg->loaded);
    CHECK(pkg->handle == nullptr);
    CHECK(std::string(statusLabel(pkg->status)) == "Incompatible ABI");

    ModelRig rig;
    std::string err;
    CHECK(rig.model.add("test.abimismatch", err) == 0);
    CHECK(err.find("Incompatible ABI") != std::string::npos);
    CHECK(rig.model.instances().empty());
}

// ----------------------------------------------------------------------------------------------------------
// fault handling
// ----------------------------------------------------------------------------------------------------------

namespace {
// Chain Gain(-6) -> Faulty -> Gain(-6): healthy output is input x 0.25.
struct FaultRig : ModelRig {
    FaultRig() {
        a = add("org.dsptb.gain");
        faulty = add("test.faulty");
        b = add("org.dsptb.gain");
        model.setParam(a, 0, -6.0206f);
        model.setParam(b, 0, -6.0206f);
        engine.play();
    }
    void checkStillProcessing(size_t fromFrame) {
        const auto in = sineBuffer(440.0, -6.0, 12.0);
        const size_t n = nb->captured().size() / 2;
        REQUIRE(n > fromFrame + kBlock);
        double worst = 0.0;
        for (size_t i = fromFrame; i < n; ++i) {
            REQUIRE(std::isfinite(left(i)));
            worst = std::max(worst, std::fabs(static_cast<double>(left(i)) - 0.25 * in.ch[0][i]));
        }
        CHECK(worst <= 2e-6); // the other modules still did their job, and the faulty one added nothing
    }
    InstanceId a = 0, faulty = 0, b = 0;
};
} // namespace

TEST_CASE("fault: a module that crashes (access violation) is marked Faulted, the app keeps running") {
    FaultRig rig;
    rig.run(20);
    CHECK_FALSE(rig.model.find(rig.faulty)->instance->faulted());
    rig.model.setParam(rig.faulty, 0, 3.0f); // mode = Crash
    rig.run(20);
    auto* rec = rig.model.find(rig.faulty);
    REQUIRE(rec != nullptr);
    CHECK(rec->instance->faulted());
    CHECK(rec->instance->faultReason() == FaultReason::Exception);
    CHECK(rig.messageContains("bypassed"));
    ModulePackage* pkg = registry().findPackage("test.faulty");
    REQUIRE(pkg != nullptr);
    CHECK(pkg->status == PackageStatus::Faulted);
    CHECK(rig.engine.isRunning());
    rig.checkStillProcessing(8 * kBlock); // after the 20 ms fade-ins of the Gains
    // The other two instances are healthy and untouched.
    CHECK_FALSE(rig.model.find(rig.a)->instance->faulted());
    CHECK_FALSE(rig.model.find(rig.b)->instance->faulted());
    // A faulted module cannot be added again in this run.
    std::string err;
    CHECK(rig.model.add("test.faulty", err) == 0);
    CHECK(err.find("faulted") != std::string::npos);
}

TEST_CASE("fault: a C++ exception thrown in process() faults the module too") {
    // The SDK trampoline catches the exception, silences the block and re-raises it as a structured fault.
    // Use a fresh registry: the shared one already has test.faulty marked Faulted by the previous test.
    ModuleRegistry reg;
    reg.scan({fs::path(DSPTB_MODULES_DIR), fs::path(DSPTB_TEST_MODULES_DIR)});
    Engine engine(ModelRig::options());
    AudioConfig cfg;
    cfg.backend = BackendKind::Null;
    cfg.sampleRate = kRate;
    cfg.bufferFrames = kBlock;
    std::string err;
    REQUIRE(engine.open(cfg, err));
    engine.nullBackend()->setRealtime(false);
    engine.nullBackend()->setCaptureOutput(true, kRate * 4);
    REQUIRE(engine.start(err));
    engine.setFile(makeFile(sineBuffer(440.0, -6.0, 4.0)));
    SessionModel model(engine, reg);
    const InstanceId f = model.add("test.faulty", err);
    REQUIRE(f != 0);
    model.setParam(f, 0, 1.0f); // mode = Throw
    engine.play();
    std::vector<SessionMessage> msgs;
    for (int i = 0; i < 10; ++i) {
        engine.nullBackend()->pump(kBlock, kBlock);
        model.tick(i * 0.005, msgs);
    }
    CHECK(model.find(f)->instance->faulted());
    // the output is still the (dry) sine, not silence and not garbage
    const auto& cap = engine.nullBackend()->captured();
    double peak = 0.0;
    for (size_t i = 5 * kBlock; i < 10 * kBlock; ++i)
        peak = std::max(peak, std::fabs(static_cast<double>(cap[i * 2])));
    CHECK(peak == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("fault: NaN output for 3 blocks in a row faults the module; the NaNs never reach the output") {
    ModuleRegistry reg;
    reg.scan({fs::path(DSPTB_MODULES_DIR), fs::path(DSPTB_TEST_MODULES_DIR)});
    Engine engine(ModelRig::options());
    AudioConfig cfg;
    cfg.backend = BackendKind::Null;
    cfg.sampleRate = kRate;
    cfg.bufferFrames = kBlock;
    std::string err;
    REQUIRE(engine.open(cfg, err));
    engine.nullBackend()->setRealtime(false);
    engine.nullBackend()->setCaptureOutput(true, kRate * 4);
    REQUIRE(engine.start(err));
    engine.setFile(makeFile(sineBuffer(440.0, -6.0, 4.0)));
    SessionModel model(engine, reg);
    const InstanceId f = model.add("test.faulty", err);
    REQUIRE(f != 0);
    model.setParam(f, 0, 2.0f); // mode = NaN
    engine.play();
    std::vector<SessionMessage> msgs;
    engine.nullBackend()->pump(kBlock, kBlock);
    model.tick(0.005, msgs);
    CHECK_FALSE(model.find(f)->instance->faulted()); // one bad block is repaired, not yet a fault
    engine.nullBackend()->pump(kBlock, kBlock);
    CHECK_FALSE(model.find(f)->instance->faulted());
    engine.nullBackend()->pump(kBlock, kBlock);
    model.tick(0.015, msgs);
    CHECK(model.find(f)->instance->faulted());
    CHECK(model.find(f)->instance->faultReason() == FaultReason::NonFinite);
    for (float v : engine.nullBackend()->captured())
        REQUIRE(std::isfinite(v));
    CHECK(engine.nonFiniteBlocks() == 0); // the chain repaired it before the safety stage had to
}

TEST_CASE("RT guard: a module that allocates in process() trips its guard (Debug / rt-check builds)") {
#if defined(DSPTB_RT_GUARD)
    ModelRig rig;
    const InstanceId id = rig.add("test.allocating");
    ModuleInstance& m = *rig.model.find(id)->instance;
    REQUIRE(m.package().guardViolations != nullptr);
    m.package().guardReset();
    rig.engine.play();
    rig.run(10); // allocate = off: clean
    CHECK(m.rtGuardHits() == 0);
    rig.model.setParam(id, 0, 1.0f);
    rig.run(10);
    CHECK(m.rtGuardHits() > 0);
#else
    MESSAGE("built without DSPTB_RT_GUARD: the allocation guard is not active in this configuration");
#endif
}

// ----------------------------------------------------------------------------------------------------------
// chain behaviour: the chain-swap glitch test, removal, undo
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("chain swap: add, reorder, bypass, re-enable and remove produce no steps and no dropouts") {
    ModelRig rig;
    rig.engine.play();
    InstanceId a = 0, b = 0;
    constexpr size_t kReorderBlock = 120;
    for (size_t blk = 0; blk < 450; ++blk) {
        switch (blk) {
        case 20:
            a = rig.add("org.dsptb.gain");
            break;
        case 60:
            b = rig.add("org.dsptb.gain");
            rig.model.setParam(b, 0, -3.0f);
            break;
        case kReorderBlock:
            rig.model.move(b, 0);
            break;
        case 200:
            rig.model.setEnabled(a, false);
            break;
        case 260:
            rig.model.setEnabled(a, true);
            break;
        case 320:
            rig.model.remove(b);
            break;
        default:
            break;
        }
        rig.run(1);
    }
    CHECK(rig.model.instances().size() == 1); // b was removed, and its ramp has finished
    CHECK_FALSE(rig.model.reorderPending());

    const auto& cap = rig.nb->captured();
    const size_t frames = cap.size() / 2;
    REQUIRE(frames == 450 * kBlock);

    // 1) No step: the largest first difference stays within 1.5x of the plain sine's.
    const double sineStep = 0.5 * 2.0 * 3.14159265358979 * 440.0 / kRate;
    double maxStep = 0.0;
    size_t where = 0;
    for (size_t i = 1; i < frames; ++i) {
        const double d = std::fabs(static_cast<double>(cap[i * 2]) - cap[(i - 1) * 2]);
        if (d > maxStep) {
            maxStep = d;
            where = i;
        }
    }
    INFO("largest step " << maxStep << " at frame " << where << ", sine step " << sineStep);
    CHECK(maxStep <= 1.5 * sineStep);

    // 2) No dropout: no whole block is silent while playing, except during the intentional reorder dip.
    size_t silentBlocks = 0, silentOutsideDip = 0;
    for (size_t blk = 0; blk < 450; ++blk) {
        bool allZero = true;
        for (size_t i = blk * kBlock; i < (blk + 1) * kBlock && allZero; ++i)
            allZero = std::fabs(cap[i * 2]) < 1e-9f;
        if (allZero) {
            ++silentBlocks;
            if (!(blk >= kReorderBlock && blk <= kReorderBlock + 8))
                ++silentOutsideDip;
        }
    }
    CHECK(silentOutsideDip == 0);

    // 3) The reorder dip really happened, and it is short: the quiet stretch around it is under 25 ms.
    size_t quiet = 0, longest = 0;
    for (size_t i = kReorderBlock * kBlock; i < (kReorderBlock + 12) * kBlock; ++i) {
        quiet = std::fabs(cap[i * 2]) < 0.002f ? quiet + 1 : 0;
        longest = std::max(longest, quiet);
    }
    MESSAGE("reorder dip: longest near-silent run " << longest << " frames ("
                                                    << 1000.0 * static_cast<double>(longest) / kRate
                                                    << " ms), whole silent blocks " << silentBlocks);
    CHECK(longest > 0);
    CHECK(longest < kRate / 40); // < 25 ms
}

TEST_CASE("chain: removing an instance ramps it out, then frees it once the audio thread let go") {
    ModelRig rig;
    const InstanceId a = rig.add("org.dsptb.gain");
    rig.engine.play();
    rig.run(10);
    std::weak_ptr<ModuleInstance> weak = rig.model.find(a)->instance;
    REQUIRE(rig.model.remove(a));
    CHECK(rig.model.pendingRemovals() == 1);
    rig.run(20);
    CHECK(rig.model.instances().empty());
    rig.run(5); // snapshots retired through the graveyard and collected
    rig.engine.collectGarbage();
    CHECK(weak.expired());
}

TEST_CASE("chain: structural changes with the engine stopped apply immediately") {
    ModelRig rig;
    const InstanceId a = rig.add("org.dsptb.gain");
    const InstanceId b = rig.add("org.dsptb.gain");
    rig.engine.stop();
    CHECK(rig.model.move(b, 0));
    CHECK(rig.model.instances()[0].id == b);
    CHECK_FALSE(rig.model.reorderPending());
    CHECK(rig.model.remove(a));
    CHECK(rig.model.instances().size() == 1);
}

TEST_CASE("labels: an instance can be named, the name is capped, empty restores the module name") {
    ModelRig rig;
    const InstanceId g = rig.add("org.dsptb.gain");
    CHECK(rig.model.find(g)->label.empty());
    CHECK(rig.model.setLabel(g, "Vocals"));
    CHECK(rig.model.find(g)->label == "Vocals");
    CHECK(rig.model.setLabel(g, std::string(100, 'x')));
    CHECK(rig.model.find(g)->label.size() == 48);
    CHECK(rig.model.setLabel(g, ""));
    CHECK(rig.model.find(g)->label.empty());
    CHECK_FALSE(rig.model.setLabel(9999, "nope"));
}

TEST_CASE("undo/redo: parameter edits, with a 100-step history") {
    ModelRig rig;
    const InstanceId g = rig.add("org.dsptb.gain");
    ParamBlock& p = rig.model.find(g)->instance->params();
    CHECK_FALSE(rig.model.canUndo());

    // a drag: live updates, then one undo step for the whole gesture
    const float before = p.get(0);
    for (float v : {-1.0f, -3.0f, -9.0f})
        rig.model.setParam(g, 0, v);
    rig.model.commitParam(g, 0, before, -9.0f);
    CHECK(p.get(0) == -9.0f);
    rig.model.setParamUndoable(g, 1, 1.0f); // invert on
    CHECK(p.get(1) == 1.0f);

    CHECK(rig.model.undo());
    CHECK(p.get(1) == 0.0f);
    CHECK(rig.model.undo());
    CHECK(p.get(0) == 0.0f);
    CHECK_FALSE(rig.model.undo());
    CHECK(rig.model.redo());
    CHECK(p.get(0) == -9.0f);
    CHECK(rig.model.redo());
    CHECK(p.get(1) == 1.0f);

    // a new edit clears redo
    rig.model.undo();
    rig.model.setParamUndoable(g, 0, 6.0f);
    CHECK_FALSE(rig.model.canRedo());

    // history is capped
    for (int i = 0; i < 150; ++i)
        rig.model.setParamUndoable(g, 0, static_cast<float>(i % 20));
    int steps = 0;
    while (rig.model.undo())
        ++steps;
    CHECK(steps <= static_cast<int>(SessionModel::kMaxUndo));
    CHECK(steps >= static_cast<int>(SessionModel::kMaxUndo) - 1);

    // undo skips steps of an instance that no longer exists
    rig.model.setParamUndoable(g, 0, 3.0f);
    rig.engine.stop();
    rig.model.remove(g);
    CHECK_FALSE(rig.model.undo());
}

TEST_CASE("param gestures: one undo step per gesture; the data folder exists before create()") {
    ModelRig rig;
    const InstanceId g = rig.add("org.dsptb.gain");
    ModuleInstance& m = *rig.model.find(g)->instance;
    ParamBlock& p = m.params();

    // The host made <dataDir>/module-data/<id> and handed it over.
    const char* dir = m.hostApi().module_data_dir_utf8;
    REQUIRE(dir != nullptr);
    CHECK(fs::is_directory(fs::path(reinterpret_cast<const char8_t*>(dir))));
    CHECK(fs::path(reinterpret_cast<const char8_t*>(dir)).filename() == "org.dsptb.gain");

    // A drag across two params is one step, labelled, undone and redone together.
    rig.model.moduleBeginGesture(m, "Move point");
    for (float v : {-1.0f, -4.0f, -8.0f}) {
        rig.model.moduleSetParam(m, 0, v);
        rig.model.moduleSetParam(m, 1, 1.0f);
        CHECK(p.get(0) == v); // values apply at once
    }
    rig.model.moduleEndGesture(m);
    CHECK(rig.model.undoLabel() == "Move point");
    CHECK(rig.model.undo());
    CHECK(p.get(0) == 0.0f);
    CHECK(p.get(1) == 0.0f);
    CHECK_FALSE(rig.model.canUndo());
    CHECK(rig.model.redo());
    CHECK(p.get(0) == -8.0f);
    CHECK(p.get(1) == 1.0f);

    // A gesture that ends where it started records nothing; a second begin ends the first.
    rig.model.moduleBeginGesture(m, "Wiggle");
    rig.model.moduleSetParam(m, 0, -2.0f);
    rig.model.moduleSetParam(m, 0, -8.0f);
    rig.model.moduleBeginGesture(m, "Second");
    rig.model.moduleSetParam(m, 0, -3.0f);
    rig.model.endGestureOf(g);
    CHECK(rig.model.undoLabel() == "Second");
    CHECK(rig.model.undo());
    CHECK(p.get(0) == -8.0f);
    CHECK(rig.model.undoLabel() == "Move point");

    // Removing the instance ends its gesture.
    rig.model.moduleBeginGesture(m, "Left open");
    rig.model.moduleSetParam(m, 0, -5.0f);
    rig.engine.stop();
    rig.model.remove(g);
    CHECK(rig.model.undoLabel() == "Left open");
}

// ----------------------------------------------------------------------------------------------------------
// CLI
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("cli validate: Gain passes; the fixtures fail for the right reasons") {
    const fs::path gain = fs::path(DSPTB_MODULES_DIR) / "org.dsptb.gain";
    const RunResult ok = runCli("validate " + q(gain) + " --json");
    REQUIRE(ok.exitCode == 0);
    const auto j = nlohmann::json::parse(ok.out);
    CHECK(j["passed"].get<bool>());
    CHECK(j["rtGuardHits"].get<int>() == 0);
    REQUIRE(j["modules"].size() == 1);
    CHECK(j["modules"][0]["checks"].size() >= 12);

    const RunResult abi = runCli("validate " + q(fs::path(DSPTB_TEST_MODULES_DIR) / "test.abimismatch"));
    CHECK(abi.exitCode == 1);
    CHECK(abi.out.find("Incompatible ABI") != std::string::npos);

    const RunResult faulty = runCli("validate " + q(fs::path(DSPTB_TEST_MODULES_DIR) / "test.faulty"));
    CHECK(faulty.exitCode == 1);

    CHECK(runCli("validate " + q(fs::path(DSPTB_MODULES_DIR) / "does-not-exist")).exitCode == 2);
#if defined(DSPTB_RT_GUARD)
    const RunResult alloc =
        runCli("validate " + q(fs::path(DSPTB_TEST_MODULES_DIR) / "test.allocating") + " --json");
    CHECK(alloc.exitCode == 1);
    CHECK(nlohmann::json::parse(alloc.out)["rtGuardHits"].get<int>() > 0);
#endif
}

TEST_CASE("cli render: Gain -6.0206 dB halves a generated sine, matching the in-process render") {
    const fs::path dir = fs::temp_directory_path() / "dsptb_test_render";
    fs::create_directories(dir);
    const fs::path session = dir / "s.dsptb.json";
    std::ofstream(session) << R"({"format":"dsptb-session","version":1,"audio":{"sampleRate":48000},
        "instances":[{"module":"org.dsptb.gain","params":{"gain_db":-6.0206}}]})";
    const fs::path in = dir / "in.wav", out = dir / "out.wav";
    REQUIRE(runCli("gen sine --freq 1000 --dbfs -6 --seconds 2 --out " + q(in)).exitCode == 0);
    REQUIRE(runCli("render --session " + q(session) + " --in " + q(in) + " --out " + q(out) + " --tail 0")
                .exitCode == 0);
    const RunResult cmp = runCli("analyze " + q(out) + " --json");
    REQUIRE(cmp.exitCode == 0);
    const auto j = nlohmann::json::parse(cmp.out);
    CHECK(std::fabs(j["peakDbfs"].get<double>() - (-12.0206)) < 0.01);
    CHECK(runCli("render --session " + q(dir / "missing.json") + " --in " + q(in) + " --out " + q(out))
              .exitCode == 2);
}
