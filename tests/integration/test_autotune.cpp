// Autotune's acceptance table, run through the real module DLL and the Pitch analyzer. Renders use the same code as
// `dsptb-cli render`.
#include <doctest/doctest.h>

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/engine.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/module_validator.hpp>
#include <dsptb/core/offline_analyze.hpp>
#include <dsptb/core/offline_render.hpp>
#include <dsptb/core/session_model.hpp>
#include <dsptb/core/signal_gen.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace dsptb;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRate = 48000;

ModuleRegistry& registry() {
    static ModuleRegistry reg = [] {
        ModuleRegistry r;
        r.scan({fs::path(DSPTB_MODULES_DIR)});
        return r;
    }();
    return reg;
}

AudioBuffer toBuffer(const PcmAudio& pcm) {
    return pcmToStereo(pcm);
}

AudioBuffer vowel(double hz, double seconds = 3.0, double vibratoHz = 0.0, double vibratoCents = 0.0) {
    GenParams p;
    p.type = SignalType::Vowel;
    p.freq = hz;
    p.seconds = seconds;
    p.dbfs = -12.0;
    p.vibratoHz = vibratoHz;
    p.vibratoCents = vibratoCents;
    return toBuffer(generateSignal(p));
}

struct Setup {
    double strength = 100.0;
    int key = 9;   // A
    int scale = 1; // minor
    int shifter = 1; // Live
};

AudioBuffer render(const AudioBuffer& in, const Setup& s) {
    const json session = {
        {"format", "dsptb-session"},
        {"version", 2},
        {"instances", json::array({{{"module", "org.dsptb.autotune"},
                                    {"params",
                                     {{"strength", s.strength},
                                      {"key_source", 1},
                                      {"key", s.key},
                                      {"scale", s.scale},
                                      {"shifter_mode", s.shifter}}}}})}};
    SessionDescription desc;
    std::string err;
    REQUIRE(parseSessionDescription(session.dump(), desc, err));
    RenderOptions opt;
    opt.tailSeconds = 0.0;
    AudioBuffer out;
    const bool ok = renderOffline(registry(), desc, in, opt, out, err);
    INFO(err);
    REQUIRE(ok);
    return out;
}

// The sung pitch over voiced frames, skipping the first 100 ms. The end is trimmed too: the input stops there.
std::vector<double> measure(const AudioBuffer& audio) {
    AnalyzeModuleOptions opt;
    opt.moduleId = "org.dsptb.pitch";
    std::string out, err;
    const bool ok = analyzeWithModule(registry(), opt, audio, out, err);
    INFO(err);
    REQUIRE(ok);
    std::vector<double> hz;
    const json j = json::parse(out);
    const double lastTime = static_cast<double>(audio.frames()) / audio.sampleRate - 0.1;
    for (const json& f : j["frames"])
        if (f["voiced"].get<bool>() && f["timeS"].get<double>() > 0.1 && f["timeS"].get<double>() < lastTime)
            hz.push_back(f["hz"].get<double>());
    return hz;
}

double centsFrom(double hz, double reference) {
    return 1200.0 * std::log2(hz / reference);
}

double shareWithin(const std::vector<double>& hz, double reference, double cents) {
    if (hz.empty())
        return 0.0;
    const auto n = std::count_if(hz.begin(), hz.end(), [&](double f) { return std::fabs(centsFrom(f, reference)) <= cents; });
    return static_cast<double>(n) / static_cast<double>(hz.size());
}

// p95 - p5 of the pitch in cents, ignoring a steady offset.
double depthCents(std::vector<double> hz, double reference) {
    REQUIRE(hz.size() > 20);
    for (double& f : hz)
        f = centsFrom(f, reference);
    std::sort(hz.begin(), hz.end());
    return hz[hz.size() * 95 / 100] - hz[hz.size() * 5 / 100];
}

} // namespace

TEST_CASE("autotune: T1, T2, T5 correct the pitch in both shifter modes") {
    for (const int shifter : {1, 2}) {
        CAPTURE(shifter);
        SUBCASE("T1: a sharp note lands on A4") {
            Setup s;
            s.shifter = shifter;
            CHECK(shareWithin(measure(render(vowel(450.3), s)), 440.0, 5.0) >= 0.95);
        }
        SUBCASE("T2: Strength 0 changes nothing") {
            Setup s;
            s.shifter = shifter;
            s.strength = 0.0;
            CHECK(shareWithin(measure(render(vowel(450.3), s)), 450.3, 2.0) >= 0.95);
        }
        SUBCASE("T5a: A4 + 58 c in C major goes down to A4") {
            Setup s;
            s.shifter = shifter;
            s.key = 0;
            s.scale = 0;
            CHECK(shareWithin(measure(render(vowel(455.0), s)), 440.0, 5.0) >= 0.95);
        }
        SUBCASE("T5b: A4 + 58 c in Chromatic goes up to A#4") {
            Setup s;
            s.shifter = shifter;
            s.scale = 2;
            CHECK(shareWithin(measure(render(vowel(455.0), s)), 466.16, 5.0) >= 0.95);
        }
    }
}

TEST_CASE("autotune: T3 low Strength keeps the vibrato in both shifter modes") {
    const AudioBuffer input = vowel(440.0, 3.0, 6.0, 50.0);
    const double inputDepth = depthCents(measure(input), 440.0);
    for (const int shifter : {1, 2}) {
        CAPTURE(shifter);
        Setup s;
        s.shifter = shifter;
        s.strength = 25.0;
        CHECK(depthCents(measure(render(input, s)), 440.0) >= 0.7 * inputDepth);
    }
}

// Full Strength flattens a slow drift almost completely, and most of a 6 Hz vibrato. It must never make a wobble
// deeper: a correction that arrives late does exactly that, so this also checks how late it is.
TEST_CASE("autotune: T4 full Strength flattens drift and vibrato in both shifter modes") {
    for (const int shifter : {1, 2}) {
        CAPTURE(shifter);
        Setup s;
        s.shifter = shifter;
        s.scale = 2;
        CHECK(depthCents(measure(render(vowel(440.0, 4.0, 1.0, 50.0), s)), 440.0) <= 10.0);
        CHECK(depthCents(measure(render(vowel(440.0, 3.0, 6.0, 50.0), s)), 440.0) <= 25.0);
        // G3 is too low for the shortest frame, so the correction comes a little later there.
        for (const double rate : {3.0, 6.0, 8.0}) {
            CAPTURE(rate);
            const AudioBuffer in = vowel(196.0, 3.0, rate, 50.0);
            CHECK(depthCents(measure(render(in, s)), 196.0) <= 0.5 * depthCents(measure(in), 196.0));
        }
    }
}

TEST_CASE("autotune: T6 noise passes through") {
    GenParams p;
    p.type = SignalType::Noise;
    p.dbfs = -20.0;
    p.seconds = 5.0;
    const AudioBuffer input = toBuffer(generateSignal(p));
    for (const int shifter : {1, 2}) {
        CAPTURE(shifter);
        Setup s;
        s.shifter = shifter;
        const AudioBuffer out = render(input, s);
        REQUIRE(out.frames() >= input.frames());
        const size_t window = kRate / 100;
        size_t clean = 0, windows = 0;
        for (size_t at = 0; at + window <= input.frames(); at += window, ++windows) {
            float worst = 0.0f;
            for (size_t i = 0; i < window; ++i)
                worst = std::max(worst, std::fabs(out.ch[0][at + i] - input.ch[0][at + i]));
            clean += worst <= 1e-4f ? 1 : 0;
        }
        CHECK(static_cast<double>(clean) >= 0.99 * static_cast<double>(windows));
    }
}

TEST_CASE("autotune: T7 export trims the latency") {
    GenParams p;
    p.type = SignalType::Impulse;
    p.dbfs = -6.0;
    p.seconds = 1.0;
    const AudioBuffer input = toBuffer(generateSignal(p));
    const size_t expected = static_cast<size_t>(0.1 * kRate);
    for (const int shifter : {1, 2}) {
        CAPTURE(shifter);
        Setup s;
        s.shifter = shifter;
        const AudioBuffer out = render(input, s);
        size_t onset = 0;
        for (size_t i = 0; i < out.frames(); ++i)
            if (std::fabs(out.ch[0][i]) > 0.1f) {
                onset = i;
                break;
            }
        CHECK(std::llabs(static_cast<long long>(onset) - static_cast<long long>(expected)) <= 1);
    }
}

TEST_CASE("autotune: T9 renders are byte-identical") {
    for (const int shifter : {1, 2}) {
        CAPTURE(shifter);
        Setup s;
        s.shifter = shifter;
        const AudioBuffer in = vowel(450.3, 1.0);
        const AudioBuffer a = render(in, s);
        const AudioBuffer b = render(in, s);
        CHECK(a.ch[0] == b.ch[0]);
        CHECK(a.ch[1] == b.ch[1]);
    }
}

TEST_CASE("autotune: T12 the package validates") {
    ModuleRegistry fresh;
    const ValidationReport report = validateModulePackage(fresh, fs::path(DSPTB_MODULES_DIR) / "org.dsptb.autotune");
    CHECK(report.passed());
}

TEST_CASE("autotune: T17 the live delay stays bounded while Strength moves") {
    // Live, as it runs in the app: detuned 150 cents up and then down, with Strength stepping every second.
    constexpr double kNote = 220.0;
    const AudioBuffer up = vowel(kNote * std::pow(2.0, 150.0 / 1200.0), 3.0);
    const AudioBuffer down = vowel(kNote * std::pow(2.0, -150.0 / 1200.0), 3.0);
    auto file = std::make_shared<LoadedFile>();
    file->path = "<generated>";
    file->audio = up;
    file->audio.ch[0].insert(file->audio.ch[0].end(), down.ch[0].begin(), down.ch[0].end());
    file->audio.ch[1].insert(file->audio.ch[1].end(), down.ch[1].begin(), down.ch[1].end());
    file->overview = computeOverview(file->audio);
    file->seconds = 6.0;

    EngineOptions options;
    options.declickMs = 0.0;
    Engine engine(options);
    AudioConfig config;
    config.backend = BackendKind::Null;
    config.sampleRate = kRate;
    config.bufferFrames = 128;
    std::string err;
    REQUIRE(engine.open(config, err));
    NullBackend* nb = engine.nullBackend();
    nb->setRealtime(false);
    nb->setCaptureOutput(true, kRate * 7);
    SessionModel model(engine, registry(), /*analysisThread=*/false);
    const InstanceId id = model.add("org.dsptb.autotune", err);
    INFO(err);
    REQUIRE(id != 0);
    ModuleInstance& inst = *model.find(id)->instance;
    model.setParam(id, static_cast<uint32_t>(inst.params().indexOf("key_source")), 1.0f);
    model.setParam(id, static_cast<uint32_t>(inst.params().indexOf("scale")), 2.0f);
    const auto strength = static_cast<uint32_t>(inst.params().indexOf("strength"));
    const uint32_t latency = inst.latencyFrames();
    REQUIRE(latency <= kRate / 1000 + 4);
    engine.setFile(file);
    REQUIRE(engine.start(err));
    engine.play();
    std::vector<SessionMessage> messages;
    double now = 0.0;
    for (int second = 0; second < 6; ++second) {
        model.setParam(id, strength, second % 2 == 0 ? 0.0f : 100.0f);
        for (uint32_t done = 0; done < kRate; done += 128) {
            nb->pump(128, 128);
            now += 128.0 / kRate;
            model.tick(now, messages);
        }
    }
    const std::vector<float>& out = nb->captured();
    const std::vector<float>& in = file->audio.ch[0];

    // A periodic signal correlates at every whole period, so what's checked is that the best match inside the allowed
    // band is as good as the best match anywhere.
    const auto period = static_cast<size_t>(kRate / kNote);
    const size_t bandEnd = latency + period + kRate / 500;
    const size_t searchEnd = bandEnd + 3 * period;
    const size_t window = kRate / 20;
    size_t checked = 0, inBand = 0;
    for (size_t at = searchEnd + kRate / 10; at + window < in.size() && 2 * (at + window) < out.size(); at += window) {
        // The input itself changes note here, so it isn't a fair reference.
        if (at + window + kRate / 10 > up.frames() && at < up.frames() + kRate / 10)
            continue;
        double bestBand = 0.0, bestAnywhere = 0.0;
        for (size_t lag = 0; lag <= searchEnd; ++lag) {
            double sum = 0.0;
            for (size_t i = 0; i < window; i += 2)
                sum += static_cast<double>(out[2 * (at + i)]) * in[at + i - lag];
            bestAnywhere = std::max(bestAnywhere, sum);
            if (lag >= latency && lag <= bandEnd)
                bestBand = std::max(bestBand, sum);
        }
        ++checked;
        inBand += bestBand >= 0.9 * bestAnywhere ? 1 : 0;
    }
    REQUIRE(checked > 50);
    CHECK(inBand == checked);
}

TEST_CASE("autotune: T18 live splices don't click") {
    const AudioBuffer in = vowel(220.0 * std::pow(2.0, 150.0 / 1200.0), 5.0);
    Setup s;
    s.scale = 2;
    const AudioBuffer out = render(in, s);
    const auto largestStep = [](const std::vector<float>& x) {
        float worst = 0.0f;
        for (size_t i = 1; i < x.size(); ++i)
            worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
        return worst;
    };
    CHECK(largestStep(out.ch[0]) <= 2.0f * largestStep(in.ch[0]));
}
