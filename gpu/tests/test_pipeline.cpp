// Unit tests for the backend-free latency pipeline (gpu/sdk/include/dsptb/gpu/pipeline.hpp) with fake stages.
// They need no GPU and run in CI.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dsptb/gpu/pipeline.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

DSPTB_DEFINE_RT_GUARD();

using namespace dsptb::gpu;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kRate = 48000.0;

uint32_t ms(double v) { return static_cast<uint32_t>(std::lround(v * kRate / 1000.0)); }

// y = 0.5 x for the hop, optionally slow, failing or late to load.
class FakeStage : public Stage {
public:
    int sleepMs = 0;            // per run
    int prepareMs = 0;
    int throwOnRun = -1;        // the 0-based run that throws
    int stallOnRun = -1;        // this run sleeps stallMs
    int stallMs = 0;
    bool throwOnPrepare = false;
    std::atomic<int> runs{0};

    void prepare() override {
        if (prepareMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(prepareMs));
        if (throwOnPrepare) throw std::runtime_error("no model file");
    }
    void run(const float* window, uint32_t windowFrames, float* out, uint32_t hop) override {
        const int n = runs.fetch_add(1);
        if (n == throwOnRun) throw std::runtime_error("boom");
        if (n == stallOnRun) std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
        if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        const float* hopStart = window + (windowFrames - hop - lookahead);
        for (uint32_t k = 0; k < hop; ++k) out[k] = 0.5f * hopStart[k];
    }
    uint32_t lookahead = 0; // the pipeline's lookahead, so the stage finds the hop inside the window
};

PipelineConfig config(double hopMs, double lookaheadMs = 0, double contextMs = 4, double budgetMs = 15, uint32_t block = 128) {
    PipelineConfig c;
    c.sampleRate = kRate;
    c.hopFrames = ms(hopMs);
    c.lookaheadFrames = ms(lookaheadMs);
    c.contextFrames = ms(contextMs);
    c.budgetFrames = ms(budgetMs);
    c.maxBlockFrames = block;
    return c;
}

FakeStage* configure(Pipeline& p, const PipelineConfig& c, bool offline = false) {
    auto stage = std::make_unique<FakeStage>();
    FakeStage* raw = stage.get();
    raw->lookahead = c.lookaheadFrames;
    p.configure(c, std::move(stage), offline);
    return raw;
}

void waitForState(const Pipeline& p, State s, int timeoutMs = 3000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (p.state() != s && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// The test thread plays the audio thread. `lockstep` waits after every block until the worker has finished every hop
// that is complete, so the run is deterministic (no misses). `paced` instead spends real time per block.
struct Drive {
    bool lockstep = false;
    bool paced = false;
    uint32_t block = 128;
    float dry = 0.0f;
};

std::vector<float> drive(Pipeline& p, const std::vector<float>& in, const Drive& d, const PipelineConfig& c, uint32_t startRunsOffset = 0) {
    std::vector<float> out(in.size());
    std::vector<float> dryBuf(d.block, d.dry);
    const auto t0 = Clock::now();
    for (size_t pos = 0; pos < in.size(); pos += d.block) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(d.block, in.size() - pos));
        {
            dsptb::rt::RtScope scope;
            p.push(in.data() + pos, n);
            p.pull(out.data() + pos, n, dryBuf.data());
        }
        if (d.lockstep) {
            const int64_t pushed = static_cast<int64_t>(pos + n);
            const int64_t ready = std::max<int64_t>(0, (pushed - static_cast<int64_t>(c.lookaheadFrames)) / c.hopFrames);
            const auto end = Clock::now() + std::chrono::seconds(3);
            while (static_cast<int64_t>(p.stats().runs) - startRunsOffset < ready && p.state() == State::Running && Clock::now() < end)
                std::this_thread::yield();
        }
        if (d.paced) {
            const auto due = t0 + std::chrono::duration<double>(static_cast<double>(pos + n) / kRate);
            while (Clock::now() < due) std::this_thread::yield();
        }
    }
    return out;
}

std::vector<float> impulseAt(size_t length, size_t at) {
    std::vector<float> v(length, 0.0f);
    v[at] = 1.0f;
    return v;
}

// The index of the loudest sample, and how much is outside it.
size_t peakIndex(const std::vector<float>& v) {
    return static_cast<size_t>(std::max_element(v.begin(), v.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - v.begin());
}

} // namespace

TEST_CASE("offline: the impulse comes out exactly latencyFrames later, for hops of 10, 20 and 40 ms") {
    for (double hopMs : {10.0, 20.0, 40.0}) {
        for (double lookMs : {0.0, 10.0}) {
            Pipeline p;
            const PipelineConfig c = config(hopMs, lookMs);
            configure(p, c, /*offline=*/true);
            p.start();
            REQUIRE(p.state() == State::Running);
            const uint32_t latency = ms(hopMs) + ms(lookMs) + ms(15);
            CHECK(p.latencyFrames() == latency);
            for (size_t at : {size_t(0), size_t(1234)}) {
                p.reset();
                const auto in = impulseAt(30000, at);
                const auto out = drive(p, in, Drive{}, c);
                INFO("hop ", hopMs, " ms, lookahead ", lookMs, " ms, impulse at ", at);
                CHECK(peakIndex(out) == at + latency);
                CHECK(std::fabs(out[at + latency] - 0.5f) < 1e-6f);
                float rest = 0;
                for (size_t i = 0; i < out.size(); ++i)
                    if (i != at + latency) rest = std::max(rest, std::fabs(out[i]));
                CHECK(rest == 0.0f);
                CHECK(p.stats().misses == 0);
            }
        }
    }
}

TEST_CASE("live: the same exact latency, with the worker thread") {
    for (double hopMs : {10.0, 20.0, 40.0}) {
        Pipeline p;
        const PipelineConfig c = config(hopMs, 5);
        configure(p, c);
        p.start();
        waitForState(p, State::Running);
        REQUIRE(p.state() == State::Running);
        Drive d;
        d.lockstep = true;
        const auto out = drive(p, impulseAt(30000, 0), d, c);
        const uint32_t latency = p.latencyFrames();
        INFO("hop ", hopMs, " ms");
        CHECK(peakIndex(out) == latency);
        CHECK(std::fabs(out[latency] - 0.5f) < 1e-6f);
        CHECK(p.stats().misses == 0);
        CHECK(p.stats().runs > 10);
        CHECK(p.stats().computeMaxUs < 1000000);
        p.stop();
    }
}

TEST_CASE("live and offline render the same noise sample for sample") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> noise(40000);
    for (float& v : noise) v = u(rng);
    const PipelineConfig c = config(20, 10);

    Pipeline offline;
    configure(offline, c, true);
    offline.start();
    const auto a = drive(offline, noise, Drive{}, c);

    Pipeline live;
    configure(live, c);
    live.start();
    waitForState(live, State::Running);
    Drive d;
    d.lockstep = true;
    const auto b = drive(live, noise, d, c);
    live.stop();
    REQUIRE(a.size() == b.size());
    size_t differing = 0;
    for (size_t i = 0; i < a.size(); ++i) differing += a[i] != b[i];
    CHECK(differing == 0);
    CHECK(live.stats().misses == 0);
}

TEST_CASE("live, paced at real time: a fast stage never misses") {
    // Scheduling on a busy machine can delay one wake-up, so a clean run is accepted on any of 3 tries.
    bool clean = false;
    for (int attempt = 0; attempt < 3 && !clean; ++attempt) {
        Pipeline p;
        const PipelineConfig c = config(20, 0, 4, 20, 128);
        configure(p, c);
        p.start();
        waitForState(p, State::Running);
        Drive d;
        d.paced = true;
        const auto out = drive(p, impulseAt(static_cast<size_t>(kRate * 0.5), 0), d, c);
        const Stats s = p.stats();
        p.stop();
        if (s.misses == 0) {
            clean = true;
            CHECK(peakIndex(out) == p.latencyFrames());
            CHECK(s.runs > 15);
        }
    }
    CHECK(clean);
}

TEST_CASE("a stage slower than the budget gives counted misses and dry output") {
    Pipeline p;
    const PipelineConfig c = config(10, 0, 4, 5, 128);
    FakeStage* stage = configure(p, c);
    stage->sleepMs = 40; // four hops per run
    p.start();
    waitForState(p, State::Running);
    Drive d;
    d.paced = true;
    d.dry = 0.25f;
    std::vector<float> in(static_cast<size_t>(kRate * 0.8), 0.7f);
    const auto out = drive(p, in, d, c);
    const Stats s = p.stats();
    p.stop();
    CHECK(s.misses > 20);
    CHECK(s.consecutiveMisses >= Pipeline::kTooSlowMisses);
    CHECK(s.tooSlow);
    // After the fade the output is the dry signal, not silence and not the model.
    size_t dryCount = 0;
    for (size_t i = out.size() / 2; i < out.size(); ++i) dryCount += std::fabs(out[i] - 0.25f) < 1e-6f;
    CHECK(dryCount > (out.size() / 2) * 8 / 10);
}

TEST_CASE("a stage that throws puts the pipeline in Failed, and the output turns dry") {
    Pipeline p;
    const PipelineConfig c = config(10, 0, 4, 15, 128);
    FakeStage* stage = configure(p, c);
    stage->throwOnRun = 4;
    p.start();
    waitForState(p, State::Running);
    Drive d;
    d.paced = true;
    d.dry = 0.3f;
    const auto out = drive(p, std::vector<float>(static_cast<size_t>(kRate * 0.5), 0.8f), d, c);
    waitForState(p, State::Failed);
    CHECK(p.state() == State::Failed);
    CHECK(p.lastError().find("boom") != std::string::npos);
    // After Failed (and the 5 ms fade) the pipeline outputs the dry signal.
    CHECK(std::fabs(out.back() - 0.3f) < 1e-6f);
    // It keeps answering from the audio thread.
    std::vector<float> more(256, 0.0f), dry(256, 0.3f);
    p.push(more.data(), 256);
    p.pull(more.data(), 256, dry.data());
    CHECK(std::fabs(more.back() - 0.3f) < 1e-6f);
    p.stop();
    CHECK(p.state() == State::Failed);
}

TEST_CASE("a stage that cannot load: Failed with the reason, audio stays dry") {
    Pipeline p;
    const PipelineConfig c = config(10);
    FakeStage* stage = configure(p, c);
    stage->throwOnPrepare = true;
    p.start();
    waitForState(p, State::Failed);
    CHECK(p.state() == State::Failed);
    CHECK(p.lastError().find("no model file") != std::string::npos);
    std::vector<float> out(512), dry(512, 0.6f), in(512, 0.0f);
    p.push(in.data(), 512);
    p.pull(out.data(), 512, dry.data());
    CHECK(std::fabs(out.back() - 0.6f) < 1e-6f);
    p.stop();
}

TEST_CASE("reset() mid-stream restarts the timeline") {
    for (bool offline : {true, false}) {
        Pipeline p;
        const PipelineConfig c = config(20, 5);
        configure(p, c, offline);
        p.start();
        if (!offline) waitForState(p, State::Running);
        Drive d;
        d.lockstep = !offline;
        // Some audio first.
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        std::vector<float> noise(7000);
        for (float& v : noise) v = u(rng);
        drive(p, noise, d, c);
        p.reset();
        const uint32_t latency = p.latencyFrames();
        // The live worker has to find the epoch change before the lockstep counter makes sense.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint32_t before = static_cast<uint32_t>(p.stats().runs);
        const auto out = drive(p, impulseAt(20000, 0), d, c, before);
        INFO("offline = ", offline);
        CHECK(peakIndex(out) == latency);
        CHECK(std::fabs(out[latency] - 0.5f) < 1e-6f);
        p.stop();
    }
}

TEST_CASE("loading is asynchronous: the audio runs dry meanwhile and the latency does not change") {
    Pipeline p;
    const PipelineConfig c = config(20, 0, 4, 15, 128);
    FakeStage* stage = configure(p, c);
    stage->prepareMs = 300;
    const uint32_t latency = p.latencyFrames();
    p.start();
    CHECK(p.state() == State::Loading);
    // While loading: dry.
    std::vector<float> in(512, 0.0f), out(512), dry(512, 0.4f);
    p.push(in.data(), 512);
    p.pull(out.data(), 512, dry.data());
    CHECK(std::fabs(out[500] - 0.4f) < 1e-6f); // after the 5 ms fade
    CHECK(p.latencyFrames() == latency);
    waitForState(p, State::Running);
    REQUIRE(p.state() == State::Running);
    CHECK(p.latencyFrames() == latency);
    // After loading, an impulse still arrives exactly one latency later.
    p.reset();
    Drive d;
    d.lockstep = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto result = drive(p, impulseAt(20000, 0), d, c);
    CHECK(peakIndex(result) == latency);
    CHECK(std::fabs(result[latency] - 0.5f) < 1e-6f);
    p.stop();
}

TEST_CASE("a late hop is a short crossfade to dry and back, never a step") {
    Pipeline p;
    const PipelineConfig c = config(10, 0, 4, 10, 128);
    FakeStage* stage = configure(p, c);
    stage->stallOnRun = 12;
    stage->stallMs = 60;
    p.start();
    waitForState(p, State::Running);
    Drive d;
    d.paced = true;
    d.dry = 0.0f;
    std::vector<float> in(static_cast<size_t>(kRate * 0.6));
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 500.0 * static_cast<double>(i) / kRate));
    const auto out = drive(p, in, d, c);
    const Stats s = p.stats();
    p.stop();
    CHECK(s.misses >= 1);
    // A 500 Hz sine of amplitude 0.5 moves at most 0.5 * 2 pi 500 / 48000 = 0.033 per sample; a fade adds little.
    float worst = 0;
    for (size_t i = 1; i < out.size(); ++i) worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
    CHECK(worst < 0.06f);
    CHECK(s.consecutiveMisses < Pipeline::kTooSlowMisses); // it recovered
}

TEST_CASE("the audio-thread calls do not allocate") {
    Pipeline p;
    const PipelineConfig c = config(20, 5);
    configure(p, c, true);
    p.start();
    dsptb::rt::resetRtGuardViolations();
    std::vector<float> in(256, 0.1f), out(256), dry(256, 0.0f);
    for (int i = 0; i < 200; ++i) {
        dsptb::rt::RtScope scope;
        p.push(in.data(), 256);
        p.pull(out.data(), 256, dry.data());
        if (i == 100) p.reset();
    }
    CHECK(dsptb::rt::rtGuardViolations() == 0);
}
