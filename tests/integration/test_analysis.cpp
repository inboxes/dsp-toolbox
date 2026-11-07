// The Audio Analysis package: acceptance tables, run through the real DLL, and offline runs in the app's model.

#include <doctest/doctest.h>

#include <dsptb/core/engine.hpp>
#include <dsptb/core/module_validator.hpp>
#include <dsptb/core/offline_analyze.hpp>
#include <dsptb/core/offline_run.hpp>
#include <dsptb/core/session_model.hpp>
#include <nlohmann/json.hpp>

#include "../../modules/common/tests/support/signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

using namespace dsptb;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr uint32_t kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

ModuleRegistry& registry() {
    static ModuleRegistry reg = [] {
        ModuleRegistry r;
        r.scan({{fs::path(DSPTB_MODULES_DIR), false}});
        return r;
    }();
    return reg;
}

template <class F> AudioBuffer makeSignal(double seconds, uint32_t rate, F&& sample) {
    AudioBuffer b;
    b.sampleRate = rate;
    const auto n = static_cast<size_t>(seconds * rate);
    b.ch[0].resize(n);
    b.ch[1].resize(n);
    for (size_t i = 0; i < n; ++i)
        b.ch[0][i] = b.ch[1][i] = static_cast<float>(sample(i));
    return b;
}

AudioBuffer sine(double hz, double dbfs, double seconds, uint32_t rate = kRate) {
    const double amp = std::pow(10.0, dbfs / 20.0);
    return makeSignal(seconds, rate, [&](size_t i) { return amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate); });
}

AudioBuffer saw(double hz, double seconds) {
    return makeSignal(seconds, kRate, [&](size_t i) {
        const double phase = hz * static_cast<double>(i) / kRate;
        return 0.5 * (2.0 * (phase - std::floor(phase)) - 1.0);
    });
}

AudioBuffer noise(double seconds) {
    uint32_t state = 1;
    return makeSignal(seconds, kRate, [&](size_t) {
        state = 1664525u * state + 1013904223u;
        return 0.25 * (2.0 * (static_cast<double>(state) / 4294967296.0) - 1.0);
    });
}

AudioBuffer clicks(double bpm, double seconds, uint32_t rate = kRate) {
    const double period = 60.0 / bpm * rate;
    return makeSignal(seconds, rate, [&](size_t i) {
        const double pos = std::fmod(static_cast<double>(i), period);
        return pos < 1.0 ? 0.9 : 0.0;
    });
}

json dump(const char* id, const AudioBuffer& in, std::vector<std::pair<std::string, float>> params = {}) {
    AnalyzeModuleOptions opt;
    opt.moduleId = id;
    opt.params = std::move(params);
    std::string out, err;
    const bool ok = analyzeWithModule(registry(), opt, in, out, err);
    INFO(err);
    REQUIRE(ok);
    return json::parse(out);
}

AudioBuffer silence(double seconds) {
    return makeSignal(seconds, kRate, [](size_t) { return 0.0; });
}

// The signal the librosa goldens were made from.
AudioBuffer goldenSignal() {
    const std::vector<float> mono = dsptb::test::signals::goldenSignal(kRate);
    AudioBuffer b;
    b.sampleRate = kRate;
    b.ch[0] = mono;
    b.ch[1] = mono;
    return b;
}

json golden(const char* name) {
    std::ifstream in(fs::path(__FILE__).parent_path().parent_path().parent_path() / "modules/common/tests/golden" / name);
    REQUIRE(in.good());
    return json::parse(in);
}

double median(std::vector<double> v) {
    REQUIRE_FALSE(v.empty());
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

std::vector<double> voicedHz(const json& j) {
    std::vector<double> out;
    for (const auto& f : j["frames"])
        if (f["voiced"].get<bool>())
            out.push_back(f["hz"].get<double>());
    return out;
}

} // namespace

TEST_CASE("analysis package: all five modules validate from one package") {
    ModuleRegistry fresh;
    const ValidationReport rep = validateModulePackage(fresh, fs::path(DSPTB_MODULES_DIR) / "org.dsptb.analysis");
    INFO(rep.toText());
    CHECK(rep.passed());
    CHECK(rep.modules.size() == 5);
}

TEST_CASE("pitch: a 220 Hz sine") {
    const json j = dump("org.dsptb.pitch", sine(220.0, -6.0, 5.0));
    const auto hz = voicedHz(j);
    CHECK(static_cast<double>(hz.size()) >= 0.95 * static_cast<double>(j["frames"].size()));
    CHECK(std::abs(median(hz) - 220.0) <= 0.5);
    CHECK(j["hop"].get<int>() == 256);
    // A frame's time is its centre.
    CHECK(j["frames"][0]["timeS"].get<double>() == doctest::Approx(1024.0 / kRate));
}

TEST_CASE("pitch: a 110 Hz sawtooth") {
    CHECK(std::abs(median(voicedHz(dump("org.dsptb.pitch", saw(110.0, 5.0)))) - 110.0) <= 1.0);
}

TEST_CASE("pitch: white noise has low confidence") {
    const json j = dump("org.dsptb.pitch", noise(5.0));
    size_t low = 0;
    for (const auto& f : j["frames"])
        low += f["confidence"].get<double>() < 0.3 ? 1 : 0;
    CHECK(static_cast<double>(low) >= 0.95 * static_cast<double>(j["frames"].size()));
}

TEST_CASE("pitch: the same answer at every rate") {
    for (const uint32_t rate : {44100u, 48000u, 96000u}) {
        CAPTURE(rate);
        CHECK(std::abs(median(voicedHz(dump("org.dsptb.pitch", sine(220.0, -6.0, 5.0, rate)))) - 220.0) <= 0.5);
    }
}

TEST_CASE("pitch: a sweep rises") {
    const double f0 = 100.0, f1 = 800.0, T = 10.0;
    const AudioBuffer sweep = makeSignal(T, kRate, [&](size_t i) {
        const double t = static_cast<double>(i) / kRate;
        const double k = std::log(f1 / f0);
        return 0.5 * std::sin(2.0 * kPi * f0 * T / k * (std::exp(t / T * k) - 1.0));
    });
    const auto hz = voicedHz(dump("org.dsptb.pitch", sweep));
    REQUIRE(hz.size() > 1000);
    double high = hz[0];
    for (const double v : hz) {
        CHECK(v >= high * 0.98);
        high = std::max(high, v);
    }
    CHECK(high > 700.0);
}

TEST_CASE("pitch: silence gives no voiced frames and valid JSON") {
    CHECK(voicedHz(dump("org.dsptb.pitch", silence(2.0))).empty());
}

TEST_CASE("onset: a click train at 120 BPM") {
    for (const uint32_t rate : {44100u, 48000u, 96000u}) {
        CAPTURE(rate);
        const json j = dump("org.dsptb.onset", clicks(120.0, 30.0, rate));
        std::vector<double> times;
        for (const auto& o : j["onsets"])
            times.push_back(o["timeS"].get<double>());
        size_t found = 0;
        const size_t expected = 59; // the first click at time 0 has no frame before it
        for (size_t k = 1; k <= expected; ++k) {
            const double t = 0.5 * static_cast<double>(k);
            found += std::any_of(times.begin(), times.end(), [&](double v) { return std::abs(v - t) <= 0.02; }) ? 1 : 0;
        }
        CHECK(static_cast<double>(found) >= 0.95 * static_cast<double>(expected));
    }
}

TEST_CASE("onset: a steady sine and silence") {
    const json j = dump("org.dsptb.onset", sine(1000.0, -6.0, 10.0));
    for (const auto& o : j["onsets"])
        CHECK(o["timeS"].get<double>() <= 0.2);
    CHECK(dump("org.dsptb.onset", silence(2.0))["onsets"].empty());
}

TEST_CASE("spectral features: the acceptance table") {
    const json tone = dump("org.dsptb.spectralfeatures", sine(1000.0, -6.0, 3.0));
    const auto& mid = tone["frames"][tone["frames"].size() / 2];
    CHECK(std::abs(mid["centroid"].get<double>() - 1000.0) <= 23.4);
    CHECK(mid["rmsDb"].get<double>() == doctest::Approx(-9.03).epsilon(0.006));

    const json white = dump("org.dsptb.spectralfeatures", noise(3.0));
    CHECK(white["frames"][white["frames"].size() / 2]["flatness"].get<double>() > 0.5);
}

TEST_CASE("spectral features: the golden signal matches librosa") {
    const json g = golden("spectral_features.json");
    const json j = dump("org.dsptb.spectralfeatures", goldenSignal());
    REQUIRE(j["frames"].size() >= g["frames"].get<size_t>());
    for (const char* key : {"centroid", "spread", "rolloff", "flatness", "zcr"})
        for (size_t i = 0; i < g["frames"].get<size_t>(); ++i) {
            const double want = g[key][i].get<double>();
            CAPTURE(key);
            CAPTURE(i);
            CHECK(std::abs(j["frames"][i][key].get<double>() - want) <= 1e-3 * std::max(1.0, std::abs(want)));
        }
}

TEST_CASE("mfcc: the golden signal matches librosa") {
    const json g = golden("mfcc_golden.json");
    const json j = dump("org.dsptb.mfcc", goldenSignal());
    REQUIRE(j["frames"].size() >= g["frames"].get<size_t>());
    for (size_t i = 0; i < g["frames"].get<size_t>(); ++i)
        for (size_t k = 0; k < g["n_mfcc"].get<size_t>(); ++k) {
            const double want = g["mfcc"][i][k].get<double>();
            CAPTURE(i);
            CAPTURE(k);
            CHECK(std::abs(j["frames"][i]["mfcc"][k].get<double>() - want) <= 1e-3 * std::max(1.0, std::abs(want)));
        }
}

TEST_CASE("mfcc: output shape, and it follows the parameters") {
    const json a = dump("org.dsptb.mfcc", noise(2.0));
    CHECK(a["nMfcc"].get<int>() == 13);
    REQUIRE_FALSE(a["frames"].empty());
    CHECK(a["frames"][0]["mfcc"].size() == 13);
    const json b = dump("org.dsptb.mfcc", noise(2.0), {{"n_mfcc", 20.0f}});
    CHECK(b["frames"][0]["mfcc"].size() == 20);
}

TEST_CASE("file analysis: agrees with the live Level Meter") {
    const AudioBuffer in = sine(1000.0, -23.0, 20.0);
    const json report = dump("org.dsptb.fileanalysis", in);
    CHECK(report["seconds"].get<double>() == doctest::Approx(20.0));
    CHECK(report["frames"].get<uint64_t>() == in.frames());
    CHECK(std::abs(report["integratedLufs"].get<double>() - (-23.0)) <= 0.5);
    CHECK(report["samplePeakDbfs"].get<double>() == doctest::Approx(-23.0).epsilon(0.001));
    CHECK(std::abs(report["dcOffset"][0].get<double>()) < 1e-4);

    const json meter = dump("org.dsptb.levelmeter", in);
    CHECK(std::abs(report["integratedLufs"].get<double>() - meter["integratedLufs"].get<double>()) <= 0.1);
}

TEST_CASE("file analysis: a known DC offset, and silence") {
    const AudioBuffer offset = makeSignal(2.0, kRate, [](size_t) { return 0.125; });
    CHECK(std::abs(dump("org.dsptb.fileanalysis", offset)["dcOffset"][0].get<double>() - 0.125) <= 1e-6);

    const json silent = dump("org.dsptb.fileanalysis", silence(2.0));
    CHECK(silent["integratedLufs"].is_null());
}

TEST_CASE("offline run: finishes, can be cancelled, and survives its instance being dropped") {
    SimpleModuleHost host;
    ModulePackage* pkg = nullptr;
    std::string err;
    const dsptb_module_desc* desc = registry().descriptor("org.dsptb.fileanalysis", &pkg, err);
    REQUIRE(desc != nullptr);
    auto audio = std::make_shared<AudioBuffer>(sine(440.0, -6.0, 10.0));
    WorkerPool pool(1);

    SUBCASE("finishes") {
        auto inst = ModuleInstance::create(1, *pkg, *desc, &host, err);
        REQUIRE(inst);
        auto run = OfflineRun::start(pool, inst, *audio, audio);
        while (!run->done())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(run->succeeded());
        CHECK(run->progress() == doctest::Approx(1.0f));
        std::string out;
        REQUIRE(inst->dumpFrames(out));
        CHECK(json::parse(out)["seconds"].get<double>() == doctest::Approx(10.0));
    }
    SUBCASE("cancel leaves no result") {
        auto inst = ModuleInstance::create(1, *pkg, *desc, &host, err);
        REQUIRE(inst);
        auto long_ = std::make_shared<AudioBuffer>(sine(440.0, -6.0, 120.0));
        auto run = OfflineRun::start(pool, inst, *long_, long_);
        run->cancel();
        const auto start = std::chrono::steady_clock::now();
        while (!run->done())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(ms <= 100.0);
        CHECK(run->cancelled());
        CHECK_FALSE(run->succeeded());
        std::string out;
        REQUIRE(inst->dumpFrames(out));
        CHECK_FALSE(json::parse(out).contains("seconds"));
    }
    SUBCASE("destroying a run waits for the worker") {
        auto inst = ModuleInstance::create(1, *pkg, *desc, &host, err);
        REQUIRE(inst);
        auto run = OfflineRun::start(pool, std::move(inst), *audio, audio);
        run.reset();
    }
}

TEST_CASE("offline modules in a session: not part of the chain, and saved like any other") {
    Engine engine;
    SessionModel model(engine, registry(), /*analysisThread=*/false);
    std::string err;
    const InstanceId gain = model.add("org.dsptb.gain", err);
    REQUIRE(gain != 0);
    const InstanceId file = model.add("org.dsptb.fileanalysis", err);
    REQUIRE(file != 0);
    const InstanceId gain2 = model.add("org.dsptb.gain", err);
    REQUIRE(gain2 != 0);
    CHECK(model.find(file)->isOffline());
    CHECK(model.totalLatencyFrames() == 0);
    // Reordering counts processors only.
    CHECK(model.move(gain2, 0));

    const auto docs = model.capture();
    const auto offline = std::find_if(docs.begin(), docs.end(), [](const SessionInstanceDoc& d) {
        return d.module == "org.dsptb.fileanalysis";
    });
    REQUIRE(offline != docs.end());
    CHECK(offline->role == "offline");

    std::vector<SessionMessage> messages;
    model.restore(docs, messages);
    CHECK(model.capture().size() == docs.size());
    CHECK(model.remove(model.instances()[1].id));
}
