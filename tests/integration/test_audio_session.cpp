// AudioSession on the NullBackend (timer thread): loading, transport, rate change, source switching,
// device-lost recovery and configuration failure.
#include <doctest/doctest.h>

#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/audio_session.hpp>
#include <dsptb/core/signal_gen.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace dsptb;

namespace {

AudioConfig nullConfig(uint32_t rate = 48000) {
    AudioConfig c;
    c.backend = BackendKind::Null;
    c.sampleRate = rate;
    c.bufferFrames = 256;
    return c;
}

fs::path makeWav(const char* name, double seconds, uint32_t rate = 48000) {
    const fs::path dir = fs::temp_directory_path() / "dsptb_test_session";
    fs::create_directories(dir);
    GenParams p;
    p.seconds = seconds;
    p.sampleRate = rate;
    p.dbfs = -6.0;
    std::string err;
    REQUIRE(writeWav(dir / name, generateSignal(p), WavFormat::F32, err));
    return dir / name;
}

struct Clock {
    double now = 0.0;
    void sleep(double seconds) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        now += seconds;
    }
};

// Ticks until the load finishes (or fails the test after 10 s). Returns the banners produced.
std::vector<AudioMessage> waitForLoad(AudioSession& s, Clock& clock) {
    std::vector<AudioMessage> msgs;
    for (int i = 0; i < 2000 && (s.loading() || i == 0); ++i) {
        s.tick(clock.now, msgs);
        clock.sleep(0.005);
    }
    // loading() turns false when the worker finishes; the next tick() is what publishes the result.
    s.tick(clock.now, msgs);
    CHECK_FALSE(s.loading());
    return msgs;
}

} // namespace

TEST_CASE("session: opens a file on the worker pool and publishes it to the engine") {
    AudioSession s;
    Clock clock;
    std::string err;
    REQUIRE(s.applyConfig(nullConfig(), err));
    CHECK(s.engine().isRunning());

    s.openFile(makeWav("one.wav", 1.0));
    CHECK(s.loading());
    waitForLoad(s, clock);
    REQUIRE(s.file() != nullptr);
    CHECK(s.durationSeconds() == doctest::Approx(1.0));
    CHECK(s.engine().hasFile());
    CHECK(s.file()->audio.sampleRate == 48000);
    CHECK(s.file()->overview.minimum.size() > 0);

    // Transport: play for a moment, pause, seek.
    s.engine().play();
    clock.sleep(0.25);
    CHECK(s.positionSeconds() > 0.05);
    s.engine().pause();
    clock.sleep(0.05);
    s.seekSeconds(0.5);
    std::vector<AudioMessage> msgs;
    clock.sleep(0.1);
    CHECK(s.positionSeconds() == doctest::Approx(0.5).epsilon(0.02));

    s.setLoopSeconds(true, 0.25, 0.75);
    CHECK(s.engine().loopEnabled());
    CHECK(s.engine().loopStart() == 12000);
    CHECK(s.engine().loopEnd() == 36000);
    s.setLoopSeconds(true, 0.0, 0.0); // empty region = the whole file
    CHECK(s.engine().loopEnd() == 48000);
}

TEST_CASE("session: a missing or unreadable file produces an error banner, not a crash") {
    AudioSession s;
    Clock clock;
    std::string err;
    REQUIRE(s.applyConfig(nullConfig(), err));
    s.openFile(fs::temp_directory_path() / "dsptb_no_such_file.wav");
    const auto msgs = waitForLoad(s, clock);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].level == NoticeLevel::Error);
    CHECK(s.file() == nullptr);
}

TEST_CASE("session: changing the sample rate reloads the file at the new rate and keeps the position") {
    AudioSession s;
    Clock clock;
    std::string err;
    REQUIRE(s.applyConfig(nullConfig(48000), err));
    s.openFile(makeWav("rate.wav", 2.0));
    waitForLoad(s, clock);
    s.seekSeconds(1.0);
    clock.sleep(0.1);

    REQUIRE(s.applyConfig(nullConfig(44100), err));
    CHECK(s.engine().sampleRate() == 44100);
    waitForLoad(s, clock);
    REQUIRE(s.file() != nullptr);
    CHECK(s.file()->audio.sampleRate == 44100);
    CHECK(s.durationSeconds() == doctest::Approx(2.0).epsilon(0.01));
    clock.sleep(0.1);
    CHECK(s.positionSeconds() == doctest::Approx(1.0).epsilon(0.05));
}

TEST_CASE("session: Mic source opens the stream with capture, File source releases it") {
    AudioSession s;
    std::string err;
    REQUIRE(s.applyConfig(nullConfig(), err));
    CHECK_FALSE(s.engine().captureActive());
    REQUIRE(s.setSource(SourceKind::Mic, err));
    CHECK(s.engine().captureActive());
    CHECK(s.source() == SourceKind::Mic);
    CHECK(s.engine().isRunning());
    REQUIRE(s.setSource(SourceKind::File, err));
    CHECK_FALSE(s.engine().captureActive());
    CHECK(s.source() == SourceKind::File);
}

TEST_CASE("session: an unplugged device is reported, retried every 2 s and recovered") {
    AudioSession s;
    std::string err;
    REQUIRE(s.applyConfig(nullConfig(), err));
    std::vector<AudioMessage> msgs;

    s.tick(10.0, msgs);
    CHECK_FALSE(s.deviceLost());
    s.engine().nullBackend()->simulateDeviceLoss();
    s.tick(10.1, msgs);
    CHECK(s.deviceLost());
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].level == NoticeLevel::Error);

    s.tick(11.0, msgs); // not yet 2 s since the loss was noticed
    CHECK(s.deviceLost());
    s.tick(12.2, msgs); // retry succeeds (the Null device is always available)
    CHECK_FALSE(s.deviceLost());
    CHECK(s.engine().isRunning());
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[1].level == NoticeLevel::Info);
}

TEST_CASE("session: a configuration that cannot be opened restores the previous one") {
    AudioSession s;
    std::string err;
    REQUIRE(s.applyConfig(nullConfig(48000), err));

    AudioConfig bad;
    bad.backend = BackendKind::WasapiShared;
    bad.outputDeviceId = "not-a-device-id"; // rejected before any hardware is touched
    CHECK_FALSE(s.applyConfig(bad, err));
    CHECK_FALSE(err.empty());

    CHECK(s.config().backend == BackendKind::Null); // last working config is back
    CHECK(s.engine().isRunning());
    CHECK_FALSE(s.deviceLost());
}
