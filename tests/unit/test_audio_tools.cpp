#include <doctest/doctest.h>

#include <dsptb/core/analysis.hpp>
#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/signal_gen.hpp>
#include <dsptb/dsp_common/voice_stats.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace dsptb;

namespace {
fs::path tempFile(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "dsptb_test_audio";
    fs::create_directories(dir);
    return dir / name;
}
} // namespace

// Median VTL of a mono signal: VoiceStats on 2048-frame windows every 1024, skipping quiet ones.
float medianVtl(const std::vector<float>& mono, double sr) {
    dsp::VoiceStats stats(sr);
    const size_t n = stats.frameSize();
    for (size_t at = 0; at + n <= mono.size(); at += n / 2) {
        double e = 0.0;
        for (size_t i = 0; i < n; ++i)
            e += static_cast<double>(mono[at + i]) * mono[at + i];
        if (std::sqrt(e / static_cast<double>(n)) > 0.01)
            stats.pushFrame(mono.data() + at, 0.95f);
    }
    return stats.summary().vtlCmMedian;
}

std::vector<float> speakerMono(double vtl, double f0, const char* vowels, uint32_t seed = 1) {
    GenParams p;
    p.type = SignalType::Speaker;
    p.vtlCm = vtl;
    p.freq = f0;
    p.vowels = vowels;
    p.seconds = 6.0;
    p.channels = 1;
    p.seed = seed;
    return generateSignal(p).samples;
}

TEST_CASE("generator: speaker has the requested tract length, its gaps, and is deterministic") {
    for (const auto& [vtl, f0, vowels, tol] :
         {std::tuple{17.0, 110.0, "schwa", 0.05}, std::tuple{14.0, 210.0, "schwa", 0.05},
          std::tuple{17.0, 110.0, "mixed", 0.08}, std::tuple{14.0, 210.0, "mixed", 0.08}}) {
        const std::vector<float> x = speakerMono(vtl, f0, vowels);
        const float measured = medianVtl(x, 48000.0);
        INFO(vtl, " cm, ", f0, " Hz, ", vowels, ": measured ", measured);
        CHECK(std::fabs(measured - vtl) / vtl <= tol);
    }
    const std::vector<float> a = speakerMono(17.0, 110.0, "mixed");
    CHECK(a == speakerMono(17.0, 110.0, "mixed"));
    CHECK(a != speakerMono(17.0, 110.0, "mixed", 2)); // the seed drives the breath noise
    // 100 ms of silence after every 4th vowel: 1.6 s to 1.7 s, minus the 10 ms fades.
    for (size_t i = static_cast<size_t>(1.615 * 48000); i < static_cast<size_t>(1.685 * 48000); ++i)
        REQUIRE(a[i] == 0.0f);
}

TEST_CASE("generator: vowel takes custom formants and a source tilt") {
    GenParams p;
    p.type = SignalType::Vowel;
    p.freq = 110.0;
    p.channels = 1;
    p.seconds = 3.0;
    p.formants = {514.7, 1544.1, 2573.5, 3602.9}; // the 17 cm tube
    const std::vector<float> tube = generateSignal(p).samples;
    CHECK(std::fabs(medianVtl(tube, 48000.0) - 17.0f) / 17.0f <= 0.05f);

    // A -6 dB/oct tilt takes high harmonics down against low ones (energy above 2 kHz vs below 1 kHz).
    const auto hiLo = [](const std::vector<float>& x) {
        double hi = 0.0, lo = 0.0, yl = 0.0, yh = 0.0, prev = 0.0;
        for (const float s : x) {
            yl += 0.12 * (s - yl);       // one-pole low-pass ~1 kHz
            yh = 0.75 * (yh + s - prev); // one-pole high-pass ~2 kHz
            prev = s;
            lo += yl * yl;
            hi += yh * yh;
        }
        return 10.0 * std::log10(hi / lo);
    };
    const std::vector<float> flat = generateSignal(p).samples;
    p.tiltDbOct = -6.0;
    const std::vector<float> tilted = generateSignal(p).samples;
    CHECK(hiLo(tilted) < hiLo(flat) - 6.0);
}

TEST_CASE("generator: 1 kHz sine at -6 dBFS measures -6.00 peak and -9.01 RMS") {
    GenParams p;
    p.type = SignalType::Sine;
    p.freq = 1000.0;
    p.dbfs = -6.0;
    const PcmAudio a = generateSignal(p);
    CHECK(a.channels == 2);
    CHECK(a.frames() == 5 * 48000);
    const AnalysisResult r = analyze(a);
    CHECK(std::fabs(r.peakDbfs - (-6.0)) < 0.01);
    CHECK(std::fabs(r.rmsDbfs - (-9.01)) < 0.02);
    CHECK(std::fabs(r.perChannel[0].dc) < 1e-6);
    CHECK(r.perChannel[0].onsetSample == 1); // sin(0) = 0, so the first non-zero frame is 1
}

TEST_CASE("generator: noise and pink hit the requested RMS and are deterministic") {
    GenParams p;
    p.dbfs = -20.0;
    p.seconds = 4.0;
    p.type = SignalType::Noise;
    const PcmAudio n1 = generateSignal(p);
    const PcmAudio n2 = generateSignal(p);
    CHECK(n1.samples == n2.samples);
    CHECK(std::fabs(analyze(n1).rmsDbfs - (-20.0)) < 0.1);
    p.seed = 2;
    CHECK(generateSignal(p).samples != n1.samples);

    p.type = SignalType::Pink;
    CHECK(std::fabs(analyze(generateSignal(p)).rmsDbfs - (-20.0)) < 0.001);
}

TEST_CASE("generator: impulse, clicks, silence, sweep and channel count") {
    GenParams p;
    p.type = SignalType::Impulse;
    p.dbfs = 0.0;
    p.seconds = 1.0;
    p.channels = 1;
    PcmAudio imp = generateSignal(p);
    CHECK(imp.channels == 1);
    const AnalysisResult ir = analyze(imp);
    CHECK(ir.perChannel[0].onsetSample == 4800);
    CHECK(ir.perChannel[0].peakDbfs == doctest::Approx(0.0));

    p.type = SignalType::Clicks;
    p.bpm = 120.0; // every 0.5 s
    p.seconds = 2.0;
    PcmAudio clicks = generateSignal(p);
    int count = 0;
    for (size_t i = 0; i < clicks.frames(); ++i)
        count += clicks.at(i, 0) != 0.0f;
    CHECK(count == 4);
    CHECK(clicks.at(0, 0) != 0.0f);
    CHECK(clicks.at(24000, 0) != 0.0f);

    p.type = SignalType::Silence;
    const AnalysisResult sr = analyze(generateSignal(p));
    CHECK(sr.peakDbfs == kDbfsFloor);
    CHECK(sr.perChannel[0].onsetSample == -1);

    p.type = SignalType::Sweep;
    p.freq = 100.0;
    p.freqEnd = 10000.0;
    p.dbfs = -3.0;
    p.seconds = 1.0;
    CHECK(std::fabs(analyze(generateSignal(p)).peakDbfs - (-3.0)) < 0.05);

    SignalType t;
    CHECK(parseSignalType("pink", t));
    CHECK(t == SignalType::Pink);
    CHECK_FALSE(parseSignalType("banana", t));
}

TEST_CASE("analysis: ranges and per-window RMS") {
    PcmAudio a;
    a.channels = 1;
    a.sampleRate = 1000;
    a.samples.assign(1000, 0.0f);
    for (size_t i = 500; i < 1000; ++i)
        a.samples[i] = 0.5f; // second half is -6 dBFS DC
    AnalysisOptions o;
    o.windowMs = 500.0;
    const AnalysisResult r = analyze(a, o);
    REQUIRE(r.windowRmsDbfs.size() == 2);
    CHECK(r.windowRmsDbfs[0][0] == kDbfsFloor);
    CHECK(std::fabs(r.windowRmsDbfs[1][0] - (-6.02)) < 0.01);
    CHECK(r.perChannel[0].onsetSample == 500);

    AnalysisOptions range;
    range.startSeconds = 0.5;
    const AnalysisResult tail = analyze(a, range);
    CHECK(tail.frames == 500);
    CHECK(tail.perChannel[0].onsetSample == 0);
    CHECK(std::fabs(tail.perChannel[0].dc - 0.5) < 1e-9);
}

TEST_CASE("wav round trip: f32 exact, s24 and s16 within one LSB, dither is deterministic") {
    GenParams p;
    p.seconds = 0.5;
    p.dbfs = -3.0;
    p.channels = 2;
    const PcmAudio src = generateSignal(p);
    std::string err;

    const fs::path f32 = tempFile("rt_f32.wav");
    REQUIRE(writeWav(f32, src, WavFormat::F32, err));
    PcmAudio back;
    REQUIRE(readPcm(f32, back, err));
    CHECK(back.channels == 2);
    CHECK(back.sampleRate == 48000);
    CHECK(back.samples == src.samples); // bit exact

    const fs::path s24 = tempFile("rt_s24.wav");
    REQUIRE(writeWav(s24, src, WavFormat::S24, err));
    REQUIRE(readPcm(s24, back, err));
    CHECK(compareAudio(src, back).maxAbsDiff < 2.0 / 8388608.0);

    const fs::path s16 = tempFile("rt_s16.wav");
    REQUIRE(writeWav(s16, src, WavFormat::S16, err));
    REQUIRE(readPcm(s16, back, err));
    CHECK(compareAudio(src, back).maxAbsDiff < 2.0 / 32768.0);

    const fs::path d1 = tempFile("rt_d1.wav"), d2 = tempFile("rt_d2.wav");
    REQUIRE(writeWav(d1, src, WavFormat::S16, err, true));
    REQUIRE(writeWav(d2, src, WavFormat::S16, err, true));
    PcmAudio da, db;
    REQUIRE(readPcm(d1, da, err));
    REQUIRE(readPcm(d2, db, err));
    CHECK(da.samples == db.samples);
    CHECK(da.samples != back.samples); // dither actually changed something
    CHECK(compareAudio(src, da).maxAbsDiff < 4.0 / 32768.0);

    WavFormat f;
    CHECK(parseWavFormat("s24", f));
    CHECK_FALSE(parseWavFormat("mp3", f));
}

TEST_CASE("decode: mono is duplicated, rate is converted, missing file reports an error") {
    GenParams p;
    p.channels = 1;
    p.sampleRate = 44100;
    p.seconds = 1.0;
    p.dbfs = -6.0;
    const PcmAudio mono = generateSignal(p);
    std::string err;
    const fs::path f = tempFile("mono44.wav");
    REQUIRE(writeWav(f, mono, WavFormat::F32, err));

    AudioBuffer same;
    REQUIRE(decodeToStereo(f, 0, same, err));
    CHECK(same.sampleRate == 44100);
    CHECK(same.frames() == 44100);
    bool identical = true;
    for (size_t i = 0; i < same.frames(); ++i)
        if (same.ch[0][i] != mono.samples[i] || same.ch[1][i] != mono.samples[i])
            identical = false;
    CHECK(identical); // duplicated at unity gain, no -3 dB pan law

    AudioBuffer up;
    REQUIRE(decodeToStereo(f, 48000, up, err));
    CHECK(up.sampleRate == 48000);
    CHECK(std::llabs(static_cast<long long>(up.frames()) - 48000) <= 2);
    float peak = 0.0f;
    for (float v : up.ch[0])
        peak = std::max(peak, std::fabs(v));
    CHECK(std::fabs(20.0 * std::log10(peak) - (-6.0)) < 0.2); // resampling keeps the level

    AudioBuffer none;
    CHECK_FALSE(decodeToStereo(tempFile("does_not_exist.wav"), 0, none, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("downmix: 5.1 puts the centre at -3 dB in both ears and drops the LFE") {
    PcmAudio six;
    six.channels = 6;
    six.sampleRate = 48000;
    six.samples.assign(6 * 4, 0.0f);
    // frame 0: centre only; frame 1: LFE only; frame 2: back-left only; frame 3: back-right only
    six.samples[0 * 6 + 2] = 1.0f;
    six.samples[1 * 6 + 3] = 1.0f;
    six.samples[2 * 6 + 4] = 1.0f;
    six.samples[3 * 6 + 5] = 1.0f;
    const AudioBuffer st = pcmToStereo(six);
    CHECK(st.ch[0][0] == doctest::Approx(0.70710678f));
    CHECK(st.ch[1][0] == doctest::Approx(0.70710678f));
    CHECK(st.ch[0][1] == 0.0f);
    CHECK(st.ch[1][1] == 0.0f);
    CHECK(st.ch[0][2] == doctest::Approx(0.70710678f));
    CHECK(st.ch[1][2] == 0.0f);
    CHECK(st.ch[0][3] == 0.0f);
    CHECK(st.ch[1][3] == doctest::Approx(0.70710678f));
}

TEST_CASE("compare: reports shape mismatches and the largest difference") {
    PcmAudio a;
    a.channels = 1;
    a.sampleRate = 48000;
    a.samples = {0.0f, 0.5f, -0.25f};
    PcmAudio b = a;
    CHECK(compareAudio(a, b).maxAbsDiff == 0.0);
    CHECK(compareAudio(a, b).shapeMatches);
    b.samples[2] = 0.25f;
    const CompareResult r = compareAudio(a, b);
    CHECK(r.maxAbsDiff == doctest::Approx(0.5));
    CHECK(r.firstDiffFrame == 2);
    b.samples.push_back(0.0f);
    CHECK_FALSE(compareAudio(a, b).shapeMatches);
}

TEST_CASE("overview: min/max per column of the mono mix") {
    AudioBuffer buf;
    buf.sampleRate = 48000;
    buf.resize(1000);
    for (size_t i = 0; i < 1000; ++i)
        buf.ch[0][i] = buf.ch[1][i] = (i < 500 ? 0.25f : -0.5f);
    const WaveformOverview o = computeOverview(buf, 10);
    REQUIRE(o.minimum.size() == 10);
    CHECK(o.framesPerColumn == 100);
    CHECK(o.maximum[0] == 0.25f);
    CHECK(o.minimum[9] == -0.5f);
}
