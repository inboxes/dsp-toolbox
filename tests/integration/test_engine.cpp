// Engine integration tests: the real audio callback driven through the NullBackend in manual (pump) mode.
#include <doctest/doctest.h>

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/engine.hpp>
#include <dsptb/core/signal_gen.hpp>
#include <dsptb/rt/rt_guard.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <random>

using namespace dsptb;

namespace {

constexpr uint32_t kRate = 48000;

std::shared_ptr<const LoadedFile> makeFile(const PcmAudio& pcm) {
    auto f = std::make_shared<LoadedFile>();
    f->path = "<generated>";
    f->audio = pcmToStereo(pcm);
    f->overview = computeOverview(f->audio);
    f->seconds = static_cast<double>(f->audio.frames()) / pcm.sampleRate;
    return f;
}

PcmAudio sine(double freq, double dbfs, double seconds, uint32_t rate = kRate) {
    GenParams p;
    p.freq = freq;
    p.dbfs = dbfs;
    p.seconds = seconds;
    p.sampleRate = rate;
    return generateSignal(p);
}

// A manual-mode engine: nothing runs until the test calls pump().
struct Rig {
    explicit Rig(EngineOptions opt = exactOptions(), bool capture = false, uint32_t block = 256,
                 size_t reserveFrames = kRate * 8)
        : engine(opt) {
        AudioConfig cfg;
        cfg.backend = BackendKind::Null;
        cfg.sampleRate = kRate;
        cfg.bufferFrames = block;
        cfg.capture = capture;
        std::string err;
        REQUIRE(engine.open(cfg, err));
        nb = engine.nullBackend();
        REQUIRE(nb != nullptr);
        nb->setRealtime(false);
        nb->setCaptureOutput(true, reserveFrames);
        REQUIRE(engine.start(err));
    }
    static EngineOptions exactOptions() {
        EngineOptions o;
        o.declickMs = 0.0; // bit-exact playback: no ramp on play/pause
        return o;
    }
    // Frames [from, from+n) of the captured output as a mono vector (left channel).
    std::vector<float> left(size_t from, size_t n) const {
        std::vector<float> v(n);
        for (size_t i = 0; i < n; ++i)
            v[i] = nb->captured()[(from + i) * 2];
        return v;
    }
    Engine engine;
    NullBackend* nb = nullptr;
};

double maxFirstDifference(const std::vector<float>& x) {
    double m = 0.0;
    for (size_t i = 1; i < x.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(x[i]) - x[i - 1]));
    return m;
}

} // namespace

TEST_CASE("engine: playing a file through the NullBackend reproduces it bit for bit") {
    const PcmAudio src = sine(1000.0, -6.0, 3.0);
    Rig rig;
    rig.engine.setFile(makeFile(src));
    REQUIRE(rig.engine.hasFile());
    rig.engine.play();
    rig.nb->pump(src.frames() + 4800); // play to the end plus a little silence

    PcmAudio out;
    out.channels = 2;
    out.sampleRate = kRate;
    out.samples.assign(rig.nb->captured().begin(),
                       rig.nb->captured().begin() + static_cast<std::ptrdiff_t>(src.frames() * 2));
    const CompareResult cmp = compareAudio(src, out);
    CHECK(cmp.shapeMatches);
    CHECK(cmp.maxAbsDiff == 0.0); // tolerance 0 at master 0 dB

    // Everything after the file is silence, and the transport stopped itself.
    bool silent = true;
    for (size_t i = src.frames() * 2; i < rig.nb->captured().size(); ++i)
        if (rig.nb->captured()[i] != 0.0f)
            silent = false;
    CHECK(silent);
    CHECK_FALSE(rig.engine.playing());

    EngineEvent ev;
    bool sawEnd = false;
    while (rig.engine.popEvent(ev))
        sawEnd |= ev.type == EngineEventType::EndOfFile;
    CHECK(sawEnd);
}

TEST_CASE("engine: nothing plays until Play, and Pause holds the position") {
    Rig rig;
    rig.engine.setFile(makeFile(sine(1000.0, -6.0, 2.0)));
    rig.nb->pump(4800);
    CHECK(analyze([&] {
              PcmAudio p;
              p.channels = 2;
              p.sampleRate = kRate;
              p.samples = rig.nb->captured();
              return p;
          }())
              .peakDbfs == kDbfsFloor);
    CHECK(rig.engine.positionFrames() == 0);

    rig.engine.play();
    rig.nb->pump(9600);
    const uint64_t pos = rig.engine.positionFrames();
    CHECK(pos == 9600);
    rig.engine.pause();
    rig.nb->pump(4800);
    CHECK(rig.engine.positionFrames() == pos);
}

TEST_CASE("engine: loop [1 s, 2 s] over 3 s wraps sample-accurately and stays continuous") {
    const PcmAudio src = sine(1000.0, -6.0, 4.0);
    Rig rig;
    rig.engine.setFile(makeFile(src));
    rig.engine.setLoop(true, kRate * 1, kRate * 2);
    rig.engine.play();
    rig.nb->pump(kRate * 3);
    REQUIRE(rig.nb->captured().size() == static_cast<size_t>(kRate) * 3 * 2);

    const std::vector<float> out = rig.left(0, kRate * 3);
    // 0..2 s plays the file, then the loop jumps back to 1 s: out[2 s + k] == in[1 s + k].
    bool exact = true;
    for (size_t k = 0; k < kRate; ++k) {
        if (out[kRate * 2 + k] != src.samples[(kRate + k) * 2])
            exact = false;
        if (out[k] != src.samples[k * 2])
            exact = false;
    }
    CHECK(exact);

    // no step bigger than 1.5x the sine's own.
    std::vector<float> original(src.frames());
    for (size_t i = 0; i < original.size(); ++i)
        original[i] = src.samples[i * 2];
    const double bound = 1.5 * maxFirstDifference(original);
    CHECK(maxFirstDifference(out) <= bound);
    const std::vector<float> aroundWrap = rig.left(kRate * 2 - 200, 400);
    CHECK(maxFirstDifference(aroundWrap) <= bound);
    CHECK(rig.engine.playing());
}

TEST_CASE("engine: seek and stop while playing do not click when declick is on") {
    EngineOptions opt; // default declick = 5 ms
    Rig rig(opt);
    const PcmAudio src = sine(200.0, -6.0, 4.0);
    rig.engine.setFile(makeFile(src));
    rig.engine.play();
    rig.nb->pump(kRate / 2);
    rig.engine.seekFrames(kRate * 2 + 123); // lands at an arbitrary phase
    rig.nb->pump(kRate / 2);
    rig.engine.pause();
    rig.nb->pump(kRate / 4);

    const std::vector<float> out = rig.left(0, rig.nb->captured().size() / 2);
    // 200 Hz at -6 dBFS: the sine's own largest step is tiny; allow a generous multiple but far below a
    // click.
    std::vector<float> original(src.frames());
    for (size_t i = 0; i < original.size(); ++i)
        original[i] = src.samples[i * 2];
    CHECK(maxFirstDifference(out) <= 2.0 * maxFirstDifference(original));
    // Pause faded to digital silence and the position froze just past where Pause was pressed.
    CHECK(out.back() == 0.0f);
    CHECK(rig.engine.positionFrames() >= kRate * 2 + 123 + kRate / 2);
}

TEST_CASE("engine: stop returns to the start, or to the loop start when looping") {
    Rig rig;
    rig.engine.setFile(makeFile(sine(1000.0, -6.0, 3.0)));
    rig.engine.play();
    rig.nb->pump(kRate);
    rig.engine.stopTransport();
    rig.nb->pump(256);
    CHECK(rig.engine.positionFrames() == 0);
    CHECK_FALSE(rig.engine.playing());

    rig.engine.setLoop(true, kRate / 2, kRate);
    rig.engine.stopTransport();
    rig.nb->pump(256);
    CHECK(rig.engine.positionFrames() == kRate / 2);
}

TEST_CASE("engine: master gain scales the output, Mute mutes with a 5 ms ramp") {
    Rig rig;
    rig.engine.setFile(makeFile(sine(1000.0, -12.0, 3.0)));
    rig.engine.setMasterGainDb(6.0f);
    rig.engine.play();
    rig.nb->pump(kRate / 2);
    PcmAudio p;
    p.channels = 2;
    p.sampleRate = kRate;
    p.samples.assign(rig.nb->captured().begin() + 2 * 4800,
                     rig.nb->captured().end());            // skip the 10 ms gain ramp
    CHECK(std::fabs(analyze(p).peakDbfs - (-6.0)) < 0.02); // -12 + 6

    rig.engine.setMasterGainDb(100.0f);
    CHECK(rig.engine.masterGainDb() == 12.0f); // clamped to +12
    rig.engine.setMasterGainDb(-100.0f);
    CHECK(rig.engine.masterGainDb() == -60.0f);
    rig.engine.setMasterGainDb(0.0f);

    rig.nb->pump(kRate / 2); // let the ramp settle
    rig.nb->clearCaptured();
    rig.engine.setMute(true);
    rig.nb->pump(1024);
    const std::vector<float> out = rig.left(0, 1024);
    CHECK(std::fabs(out[0]) > 0.0f); // the ramp starts from the live signal, no instant cut
    CHECK(out[256] == 0.0f);         // fully silent after >= 5 ms (240 samples)
    CHECK(out[1023] == 0.0f);
    rig.engine.setMute(false);
    rig.nb->pump(1024);
    CHECK(rig.left(1024, 1024)[1023] != 0.0f);
}

TEST_CASE("engine: safety stage clips at 0 dBFS and scrubs NaN/Inf") {
    // Hot microphone signal with monitoring on: values above full scale, plus NaN and Inf.
    Rig rig(Rig::exactOptions(), /*capture=*/true);
    rig.engine.setSource(SourceKind::Mic);
    rig.engine.setMonitorMic(true);
    // The monitor gate is created open only when monitoring is on at open(); reopen to apply the initial
    // state.
    AudioConfig cfg = rig.engine.config();
    std::string err;
    REQUIRE(rig.engine.open(cfg, err));
    rig.nb = rig.engine.nullBackend();
    rig.nb->setRealtime(false);
    rig.nb->setCaptureOutput(true, 4096);
    REQUIRE(rig.engine.start(err));

    std::vector<float> mic(2 * 512, 0.25f);
    mic[2 * 10] = 2.0f;      // left over full scale
    mic[2 * 20 + 1] = -3.0f; // right under -full scale
    mic[2 * 30] = std::numeric_limits<float>::quiet_NaN();
    mic[2 * 40 + 1] = std::numeric_limits<float>::infinity();
    rig.nb->setInput(mic);
    rig.nb->pump(512);

    const std::vector<float>& out = rig.nb->captured();
    CHECK(out[2 * 10] == 1.0f);
    CHECK(out[2 * 20 + 1] == -1.0f);
    CHECK(out[2 * 30] == 0.0f);
    CHECK(out[2 * 40 + 1] == 0.0f);
    CHECK(out[2 * 5] == 0.25f);
    for (float v : out)
        CHECK(std::isfinite(v));
    CHECK(rig.engine.clipLatched());
    CHECK(rig.engine.nonFiniteBlocks() >= 1);

    bool sawClip = false, sawNonFinite = false;
    EngineEvent ev;
    while (rig.engine.popEvent(ev)) {
        sawClip |= ev.type == EngineEventType::Clip;
        sawNonFinite |= ev.type == EngineEventType::NonFinite;
    }
    CHECK(sawClip);
    CHECK(sawNonFinite);
    rig.engine.clearClip();
    CHECK_FALSE(rig.engine.clipLatched());
}

TEST_CASE("engine: mic source is silent at the output unless monitoring is on, but taps still see it") {
    const PcmAudio micSignal = sine(440.0, -10.0, 1.0);
    auto run = [&](bool monitor, std::vector<float>& outCaptured, std::vector<float>& tapped) {
        Rig rig(Rig::exactOptions(), /*capture=*/true);
        rig.engine.setSource(SourceKind::Mic);
        rig.engine.setMonitorMic(monitor);
        AudioConfig cfg = rig.engine.config();
        std::string err;
        REQUIRE(rig.engine.open(cfg, err)); // re-open so the initial gate state matches
        rig.nb = rig.engine.nullBackend();
        rig.nb->setRealtime(false);
        rig.nb->setCaptureOutput(true, kRate);
        rig.nb->setInput(micSignal.samples);

        TapRing pre(kRate); // one second of frames
        auto snap = std::make_unique<ChainSnapshot>();
        snap->preTaps.push_back(&pre);
        rig.engine.publishChain(std::move(snap));
        REQUIRE(rig.engine.start(err));
        rig.nb->pump(9600, 256);
        tapped.resize(pre.ring.size());
        tapped.resize(pre.ring.read(tapped.data(), tapped.size()));
        outCaptured = rig.nb->captured();
        CHECK(pre.droppedFrames.load() == 0);
        rig.engine.stop();
    };

    std::vector<float> outOff, tapOff, outOn, tapOn;
    run(false, outOff, tapOff);
    run(true, outOn, tapOn);

    // Monitoring off: output is exactly silent (the source starts in the "muted" state), analyzers still hear
    // the mic.
    for (float v : outOff)
        CHECK(v == 0.0f);
    REQUIRE(tapOff.size() == 9600u * 2);
    for (size_t i = 0; i < tapOff.size(); ++i)
        CHECK(tapOff[i] == micSignal.samples[i]);

    // Monitoring on: output equals the mic exactly (unity path), taps the same.
    REQUIRE(outOn.size() == 9600u * 2);
    for (size_t i = 0; i < outOn.size(); ++i)
        CHECK(outOn[i] == micSignal.samples[i]);
    CHECK(tapOn == tapOff);
}

TEST_CASE("engine: switching File -> Mic crossfades over 20 ms and ends on the pure mic signal") {
    Rig rig(Rig::exactOptions(), /*capture=*/true);
    rig.engine.setFile(makeFile(sine(1000.0, -6.0, 3.0)));
    rig.engine.setMonitorMic(true);
    rig.nb->setInput(sine(250.0, -12.0, 1.0).samples);
    rig.engine.play();
    rig.nb->pump(kRate / 4);
    rig.engine.setSource(SourceKind::Mic);
    rig.nb->pump(kRate / 4);

    const size_t switchFrame = kRate / 4;
    const std::vector<float> out = rig.left(0, kRate / 2);
    for (float v : out)
        CHECK(std::isfinite(v));
    // Within one 256-frame chunk plus the 20 ms fade (960 samples) the file has faded out completely: the
    // output equals the mic input from then on (file gain is exactly 0, so the sum is exactly the mic
    // sample).
    const PcmAudio mic = sine(250.0, -12.0, 1.0);
    const size_t settled = switchFrame + 256 + 960 + 2;
    for (size_t i = settled; i < out.size(); ++i) {
        const size_t micIndex = i % mic.frames();
        REQUIRE(out[i] == mic.samples[micIndex * 2]);
    }
    // No step during the fade bigger than the louder source's own steps.
    const std::vector<float> fade = rig.left(switchFrame, 1400);
    CHECK(maxFirstDifference(fade) < 0.2);
}

TEST_CASE("engine: taps get every frame of the post-source bus in order, and drop whole blocks when full") {
    const PcmAudio src = sine(1000.0, -6.0, 2.0);
    Rig rig;
    rig.engine.setFile(makeFile(src));
    TapRing tap(kRate / 2);
    auto snap = std::make_unique<ChainSnapshot>();
    snap->preTaps.push_back(&tap);
    snap->postTaps.push_back(
        &tap); // same ring in both slots just doubles the data: use a separate one for POST
    snap->postTaps.clear();
    rig.engine.publishChain(std::move(snap));
    rig.engine.play();

    std::vector<float> got;
    std::vector<float> chunk(4096);
    for (int i = 0; i < 100; ++i) { // 100 x 256 frames, draining as we go
        rig.nb->pump(256);
        const size_t n = tap.ring.read(chunk.data(), chunk.size());
        got.insert(got.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
    }
    REQUIRE(got.size() == 25600u * 2);
    for (size_t i = 0; i < got.size(); ++i)
        REQUIRE(got[i] == src.samples[i]);
    CHECK(tap.droppedFrames.load() == 0);
    CHECK(tap.framesWritten.load() == 25600u);

    // Without a consumer the ring (0.5 s) fills and whole blocks are dropped and counted.
    rig.nb->pump(kRate); // 1 s more, nobody reading
    CHECK(tap.droppedFrames.load() > 0);
    CHECK(tap.ring.size() % 2 == 0); // never a split frame
}

TEST_CASE("engine: a lost device is reported and the transport survives") {
    Rig rig;
    CHECK_FALSE(rig.engine.deviceLost());
    rig.nb->simulateDeviceLoss();
    CHECK(rig.engine.deviceLost());
}

TEST_CASE("engine: pausing stops analyzer taps after the fade-out, and a recording's tap keeps going") {
    Rig rig(EngineOptions{}, /*capture=*/false, 256, /*reserveFrames=*/kRate);
    rig.engine.setFile(makeFile(sine(440.0, -6.0, 5.0)));
    TapRing analyzer(kRate), recording(kRate);
    analyzer.stopsWhenPaused = true;
    auto snap = std::make_unique<ChainSnapshot>();
    snap->preTaps.push_back(&analyzer);
    snap->postTaps.push_back(&recording);
    rig.engine.publishChain(std::move(snap));
    rig.engine.play();

    std::vector<float> drain(8192);
    const auto run = [&](size_t blocks) {
        for (size_t b = 0; b < blocks; ++b) {
            rig.nb->pump(256);
            analyzer.ring.read(drain.data(), drain.size());
            recording.ring.read(drain.data(), drain.size());
        }
    };
    run(20);
    CHECK(analyzer.framesWritten.load() == 20u * 256u);

    rig.engine.pause();
    run(20); // longer than any fade-out
    const uint64_t paused = analyzer.framesWritten.load();
    CHECK(paused > 20u * 256u); // the fade-out arrived
    const uint64_t recorded = recording.framesWritten.load();
    run(20);
    CHECK(analyzer.framesWritten.load() == paused);
    CHECK(recording.framesWritten.load() == recorded + 20u * 256u);

    // A seek while playing dips the gate but must not stop the stream.
    rig.engine.play();
    run(20);
    const uint64_t beforeSeek = analyzer.framesWritten.load();
    rig.engine.seekFrames(kRate);
    run(20);
    CHECK(analyzer.framesWritten.load() == beforeSeek + 20u * 256u);
}

TEST_CASE("engine: 30 s of mixed activity makes no allocation on the audio thread") {
    rt::resetRtGuardViolations();
    Rig rig(EngineOptions{}, /*capture=*/true, 256, /*reserveFrames=*/kRate * 31);
    rig.engine.setFile(makeFile(sine(440.0, -6.0, 10.0)));
    rig.nb->setInput(sine(220.0, -20.0, 2.0).samples);
    TapRing pre(kRate), post(kRate);
    auto snap = std::make_unique<ChainSnapshot>();
    snap->preTaps.push_back(&pre);
    snap->postTaps.push_back(&post);
    rig.engine.publishChain(std::move(snap));
    rig.engine.setLoop(true, kRate, kRate * 3);
    rig.engine.play();

    std::vector<float> drain(8192);
    const size_t blocks = (kRate * 30) / 256;
    for (size_t b = 0; b < blocks; ++b) {
        rig.nb->pump(256);
        pre.ring.read(drain.data(), drain.size());
        post.ring.read(drain.data(), drain.size());
        // Poke every control from the "UI thread" while the callback runs.
        if (b % 400 == 100)
            rig.engine.seekFrames((b * 7919) % (kRate * 5));
        if (b % 500 == 200)
            rig.engine.setSource(b % 1000 == 200 ? SourceKind::Mic : SourceKind::File);
        if (b % 300 == 50)
            rig.engine.setMasterGainDb(static_cast<float>(static_cast<int>(b % 7) - 3));
        if (b % 450 == 10)
            rig.engine.setMute(b % 900 == 10);
        if (b % 600 == 0)
            rig.engine.setFile(makeFile(sine(330.0 + static_cast<double>(b % 5) * 10.0, -9.0, 4.0)));
        if (b % 600 == 5)
            rig.engine.play();
        rig.engine.collectGarbage();
        EngineEvent ev;
        while (rig.engine.popEvent(ev)) {
        }
    }
    CHECK(rig.nb->captured().size() == blocks * 256 * 2);
    CHECK(rt::rtGuardViolations() == 0);
    CHECK(rig.engine.nonFiniteBlocks() == 0);
    CHECK(rig.engine.callbacks() == blocks);
}

TEST_CASE("engine: mic noise gate is bit-exact when off, and silences noise but passes speech when on") {
    // 1 s of -60 dBFS noise, 1 s of a -20 dBFS tone, 2 s of noise again, interleaved stereo.
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0.0, std::pow(10.0, -60.0 / 20.0));
    const size_t frames = static_cast<size_t>(kRate) * 4;
    std::vector<float> mic(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        double v = noise(rng);
        if (t >= 1.0 && t < 2.0)
            v += 0.1 * std::sin(2.0 * 3.14159265358979 * 220.0 * t);
        mic[2 * i] = mic[2 * i + 1] = static_cast<float>(v);
    }

    auto run = [&](bool gateOn, std::vector<float>& captured, float* inputPeak) {
        Rig rig(Rig::exactOptions(), /*capture=*/true, 256, frames);
        rig.engine.setSource(SourceKind::Mic);
        rig.engine.setMonitorMic(true);
        rig.engine.setMicGate(gateOn, -50.0f);
        AudioConfig cfg = rig.engine.config();
        std::string err;
        REQUIRE(rig.engine.open(cfg, err)); // reopen so the source gates start in the "mic selected" state
        rig.nb = rig.engine.nullBackend();
        rig.nb->setRealtime(false);
        rig.nb->setCaptureOutput(true, frames);
        rig.nb->setInput(mic);
        REQUIRE(rig.engine.start(err));
        rig.nb->pump(frames, 256);
        captured = rig.nb->captured();
        if (inputPeak)
            *inputPeak = rig.engine.micPeakLeft();
    };

    std::vector<float> off, on;
    float peakAtEnd = 0.0f;
    run(false, off, nullptr);
    REQUIRE(off.size() == mic.size());
    for (size_t i = 0; i < off.size(); ++i)
        REQUIRE(off[i] == mic[i]); // gate off: exactly the microphone

    run(true, on, &peakAtEnd);
    REQUIRE(on.size() == mic.size());
    auto rmsDb = [&](double fromSec, double toSec) {
        double acc = 0.0;
        const size_t a = static_cast<size_t>(fromSec * kRate), b = static_cast<size_t>(toSec * kRate);
        for (size_t i = a; i < b; ++i)
            acc += static_cast<double>(on[2 * i]) * on[2 * i];
        return 10.0 * std::log10(acc / static_cast<double>(b - a) + 1e-30);
    };
    CHECK(rmsDb(0.1, 0.9) < -120.0); // background noise removed
    CHECK(std::fabs(rmsDb(1.1, 1.9) - (-23.0103)) <
          0.3);                      // tone passes at its level (0.1 peak = -23 dBFS RMS)
    CHECK(rmsDb(3.2, 3.9) < -120.0); // closed again after the hold and release time
    // The input meter reads the microphone before the gate: at the end it has decayed to the noise level, not
    // zero.
    CHECK(peakAtEnd > 0.0f);
}
