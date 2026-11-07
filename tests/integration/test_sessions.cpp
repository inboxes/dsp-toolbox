// Session files (save, load, placeholders, relocation), export, the CLI reading full sessions, and the
// recorder. Real DLLs, real engine on the NullBackend.
#include <doctest/doctest.h>

#include <dsptb/core/audio_io.hpp>
#include <dsptb/core/audio_session.hpp>
#include <dsptb/core/export_job.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/paths.hpp>
#include <dsptb/core/recorder.hpp>
#include <dsptb/core/session_io.hpp>
#include <dsptb/core/signal_gen.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace dsptb;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

ModuleRegistry& registry() {
    static ModuleRegistry reg = [] {
        ModuleRegistry r;
        r.scan({fs::path(DSPTB_MODULES_DIR), fs::path(DSPTB_TEST_MODULES_DIR)});
        return r;
    }();
    return reg;
}

fs::path tempDir() {
    const fs::path d = fs::temp_directory_path() / "dsptb_test_m6";
    fs::create_directories(d);
    return d;
}

fs::path makeWav(const fs::path& file, double seconds, uint32_t rate = 48000) {
    GenParams p;
    p.seconds = seconds;
    p.sampleRate = rate;
    p.dbfs = -12.0;
    p.type = SignalType::Pink;
    std::string err;
    REQUIRE(writeWav(file, generateSignal(p), WavFormat::F32, err));
    return file;
}

AudioConfig nullConfig() {
    AudioConfig c;
    c.backend = BackendKind::Null;
    c.sampleRate = 48000;
    c.bufferFrames = 256;
    return c;
}

// An AudioSession on the NullBackend (manual pumping) plus a SessionModel on its engine.
struct Rig {
    Rig() : model(audio.engine(), registry()) {
        std::string err;
        REQUIRE(audio.applyConfig(nullConfig(), err));
        // applyConfig starts the stream on the timer thread; switch to manual pumping so tests are exact.
        Engine& e = audio.engine();
        e.stop();
        e.nullBackend()->setRealtime(false);
        REQUIRE(e.start(err));
    }
    void tick(double seconds = 0.0) {
        std::vector<AudioMessage> am;
        audio.tick(now, am);
        model.tick(now, messages);
        now += seconds;
    }
    void waitForLoad() {
        for (int i = 0; i < 2000 && (audio.loading() || i == 0); ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            now += 0.005;
        }
        tick();
        REQUIRE_FALSE(audio.loading());
    }
    InstanceId add(const char* module) {
        std::string err;
        const InstanceId id = model.add(module, err);
        INFO(err);
        REQUIRE(id != 0);
        return id;
    }
    void setParam(InstanceId id, const char* paramId, float v) {
        InstanceRecord* r = model.find(id);
        REQUIRE(r != nullptr);
        const int idx = r->instance->params().indexOf(paramId);
        REQUIRE(idx >= 0);
        r->instance->params().set(static_cast<uint32_t>(idx), v);
    }
    bool messageContains(const std::string& s) const {
        for (const auto& m : messages)
            if (m.text.find(s) != std::string::npos)
                return true;
        return false;
    }
    AudioSession audio;
    SessionModel model;
    double now = 0.0;
    std::vector<SessionMessage> messages;
};

std::string readText(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

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
// Session file
// ----------------------------------------------------------------------------------------------------------

TEST_CASE("session: save -> load -> save is byte-identical, with every first-party module") {
    const fs::path dir = tempDir();
    // A CJK file name: the path must survive the round trip as UTF-8.
    const fs::path audioFile = makeWav(dir / fromUtf8("\xE6\xB5\x8B\xE8\xAF\x95_in.wav"), 2.0);
    const fs::path sessionFile = dir / "roundtrip.dsptb.json";

    std::string first;
    {
        Rig a;
        a.audio.openFile(audioFile);
        a.waitForLoad();
        a.audio.setLoopSeconds(true, 0.25, 1.5);
        a.audio.engine().setMasterGainDb(-3.5f);

        const InstanceId gain = a.add("org.dsptb.gain");
        a.setParam(gain, "gain_db", -4.5f);
        a.setParam(gain, "invert", 1.0f);
        a.model.setLabel(gain, "My gain");
        const InstanceId spat = a.add("org.dsptb.spatializer");
        REQUIRE(a.model.find(spat)->instance->loadState(
            R"({"range":"near","snap":true,"hrtf":"builtin","sources":[{"azimuth":33.0,"distance":0.4}]})"));
        a.setParam(spat, "output_gain_db", -2.0f);
        const InstanceId meter = a.add("org.dsptb.levelmeter");
        a.model.setEnabled(meter, false);
        const InstanceId spec = a.add("org.dsptb.spectrogram");
        a.model.setTap(spec, DSPTB_TAP_PRE);

        SessionDoc doc = captureSession(a.audio, a.model, sessionFile);
        doc.layout = "[Window][Test]\nPos=1,2\nSize=3,4\n";
        REQUIRE(doc.instances.size() == 4);
        std::string err;
        REQUIRE(writeSessionFile(sessionFile, doc, err));
        first = readText(sessionFile);
    }

    // The file keeps the values.
    SessionDoc loaded;
    std::string err;
    REQUIRE(readSessionFile(sessionFile, loaded, err));
    CHECK(loaded.masterGainDb == doctest::Approx(-3.5));
    CHECK(loaded.source.loopEnabled);
    CHECK(loaded.source.loopEnd == doctest::Approx(1.5));
    CHECK(loaded.source.relativePath == toUtf8(audioFile.filename()));
    CHECK(loaded.instances[0].params.at("gain_db") == doctest::Approx(-4.5));
    CHECK(loaded.instances[0].label == "My gain");
    CHECK_FALSE(loaded.instances[2].enabled);
    CHECK(loaded.instances[3].tap == "pre");

    // Load into a fresh engine, save again: the same bytes.
    Rig b;
    const ApplyResult res = applySession(loaded, sessionFile, b.audio, b.model, b.messages);
    CHECK(res.wantsFile);
    CHECK_FALSE(res.fileMissing);
    b.waitForLoad();
    REQUIRE(b.audio.file() != nullptr);
    SessionDoc again = captureSession(b.audio, b.model, sessionFile);
    again.layout = loaded.layout;
    CHECK(b.audio.loopEnabled());
    CHECK(b.audio.engine().masterGainDb() == doctest::Approx(-3.5f));
    const fs::path second = dir / "roundtrip2.dsptb.json";
    REQUIRE(writeSessionFile(second, again, err));
    CHECK(readText(second) == first);
}

TEST_CASE("session: inputs round-trip, and version 1 files migrate") {
    SessionDoc doc;
    std::string err;

    SUBCASE("version 1, file") {
        REQUIRE(sessionFromJson(R"({"format":"dsptb-session","version":1,"source":{"type":"file"}})", doc, err));
        CHECK(doc.inputs.file.route == "chain");
        CHECK(doc.inputs.mic.route == "off");
    }
    SUBCASE("version 1, mic") {
        REQUIRE(sessionFromJson(R"({"format":"dsptb-session","version":1,"source":{"type":"mic"}})", doc, err));
        CHECK(doc.inputs.file.route == "off");
        CHECK(doc.inputs.mic.route == "chain");
    }
    SUBCASE("routes that break the rule fall back to the file") {
        REQUIRE(sessionFromJson(
            R"({"format":"dsptb-session","version":2,"inputs":{"file":{"route":"chain"},"mic":{"route":"chain"}}})", doc,
            err));
        CHECK(doc.inputs.file.route == "chain");
        CHECK(doc.inputs.mic.route == "off");
        REQUIRE(sessionFromJson(R"({"format":"dsptb-session","version":2,"inputs":{"file":{"route":"sideways"}}})", doc,
                                err));
        CHECK(doc.inputs.file.route == "chain");
    }
    SUBCASE("Mic + track survives save and load") {
        doc.inputs.file = {"output", -6.0};
        doc.inputs.mic = {"chain", 0.0};
        SessionDoc back;
        const std::string text = sessionToJson(doc);
        CHECK(json::parse(text)["version"] == 2);
        REQUIRE(sessionFromJson(text, back, err));
        CHECK(back.inputs.file.route == "output");
        CHECK(back.inputs.file.gainDb == doctest::Approx(-6.0));
        CHECK(back.inputs.mic.route == "chain");
        CHECK(sessionToJson(back) == text);

        Rig r;
        applySession(back, "none.dsptb.json", r.audio, r.model, r.messages);
        CHECK(r.audio.engine().route(SourceKind::File) == InputRoute::Output);
        CHECK(r.audio.engine().route(SourceKind::Mic) == InputRoute::Chain);
        CHECK(r.audio.engine().outputRouteGainDb(SourceKind::File) == doctest::Approx(-6.0f));
        const SessionDoc saved = captureSession(r.audio, r.model, {});
        CHECK(saved.inputs.file.route == "output");
        CHECK(saved.inputs.mic.route == "chain");
    }
}

TEST_CASE("session: a missing module becomes a placeholder that keeps its data") {
    json j = {{"format", "dsptb-session"},
              {"version", 1},
              {"audio", {{"sampleRate", 48000}}},
              {"instances", json::array({{{"module", "org.dsptb.gain"}, {"params", {{"gain_db", -3.0}}}},
                                         {{"module", "org.example.missing"},
                                          {"moduleVersion", "9.9.9"},
                                          {"role", "processor"},
                                          {"enabled", false},
                                          {"params", {{"depth", 0.75}, {"mix", 0.5}}},
                                          {"state", {{"table", json::array({1, 2, 3})}, {"name", "x"}}}},
                                         {{"module", "org.dsptb.levelmeter"}, {"role", "analyzer"}}})}};
    SessionDoc doc;
    std::string err;
    REQUIRE(sessionFromJson(j.dump(), doc, err));

    Rig r;
    applySession(doc, "none.dsptb.json", r.audio, r.model, r.messages);
    CHECK(r.messageContains("org.example.missing"));
    REQUIRE(r.model.placeholders().size() == 1);
    CHECK(r.model.instances().size() == 2); // gain and level meter are live

    // Saving again puts it back in the middle with everything it had.
    const SessionDoc out = captureSession(r.audio, r.model, {});
    REQUIRE(out.instances.size() == 3);
    CHECK(out.instances[0].module == "org.dsptb.gain");
    const SessionInstanceDoc& ph = out.instances[1];
    CHECK(ph.module == "org.example.missing");
    CHECK(ph.moduleVersion == "9.9.9");
    CHECK_FALSE(ph.enabled);
    CHECK(ph.params.at("depth") == doctest::Approx(0.75));
    CHECK(ph.params.at("mix") == doctest::Approx(0.5));
    CHECK(json::parse(ph.stateJson) == json::parse(R"({"table":[1,2,3],"name":"x"})"));
    CHECK(out.instances[2].module == "org.dsptb.levelmeter");
}

TEST_CASE("session: unknown parameters are ignored, missing ones use defaults, bad files are refused") {
    json j = {{"format", "dsptb-session"},
              {"instances", json::array({{{"module", "org.dsptb.gain"},
                                          {"params", {{"no_such_param", 3.0}, {"gain_db", -6.0}}}}})}};
    SessionDoc doc;
    std::string err;
    REQUIRE(sessionFromJson(j.dump(), doc, err));
    Rig r;
    applySession(doc, "none", r.audio, r.model, r.messages);
    REQUIRE(r.model.instances().size() == 1);
    const auto& p = r.model.instances()[0].instance->params();
    CHECK(p.get(static_cast<uint32_t>(p.indexOf("gain_db"))) == doctest::Approx(-6.0f));
    CHECK(p.get(static_cast<uint32_t>(p.indexOf("invert"))) == 0.0f); // default

    CHECK_FALSE(sessionFromJson("not json", doc, err));
    CHECK_FALSE(sessionFromJson(R"({"format":"dsptb-session","version":99})", doc, err));
    CHECK(err.find("newer") != std::string::npos);
    CHECK_FALSE(sessionFromJson(R"({"format":"dsptb-session","instances":[{}]})", doc, err));

    // Wrong-typed values are refused with a message and never throw.
    for (const char* bad : {R"({"format":"dsptb-session","version":"x"})",
                            R"({"format":"dsptb-session","masterGainDb":"loud"})",
                            R"({"format":"dsptb-session","audio":{"sampleRate":"fast"}})",
                            R"({"format":"dsptb-session","instances":[{"module":"a","enabled":"yes"}]})",
                            R"({"format":"dsptb-session","source":{"loop":{"start":"a"}}})"}) {
        err.clear();
        bool ok = true;
        CHECK_NOTHROW(ok = sessionFromJson(bad, doc, err));
        CHECK_FALSE(ok);
        CHECK(err.find("wrong type") != std::string::npos);
    }
}

TEST_CASE("session: the audio file is found by relative path when the absolute one is gone, else reported") {
    const fs::path dir = tempDir() / "reloc";
    fs::create_directories(dir);
    makeWav(dir / "clip.wav", 1.0);
    const fs::path sessionFile = dir / "s.dsptb.json";

    SessionSourceDoc src;
    src.path = "Z:/does/not/exist/clip.wav";
    src.relativePath = "clip.wav";
    CHECK(resolveSessionAudio(sessionFile, src) == (dir / "clip.wav").lexically_normal());
    src.relativePath = "gone.wav";
    CHECK(resolveSessionAudio(sessionFile, src).empty());

    SessionDoc doc;
    doc.source = src;
    Rig r;
    const ApplyResult res = applySession(doc, sessionFile, r.audio, r.model, r.messages);
    CHECK(res.wantsFile);
    CHECK(res.fileMissing);
    CHECK(res.missingPath == "Z:/does/not/exist/clip.wav");
}

TEST_CASE("session: the file is written atomically and replaces an existing one") {
    const fs::path f = tempDir() / "atomic.dsptb.json";
    SessionDoc doc;
    std::string err;
    REQUIRE(writeSessionFile(f, doc, err));
    doc.masterGainDb = -1.0;
    REQUIRE(writeSessionFile(f, doc, err));
    CHECK_FALSE(fs::exists(fs::path(f) += ".tmp"));
    SessionDoc back;
    REQUIRE(readSessionFile(f, back, err));
    CHECK(back.masterGainDb == -1.0);
}

// ----------------------------------------------------------------------------------------------------------
// Export and the CLI
// ----------------------------------------------------------------------------------------------------------

namespace {
bool sameSamples(const fs::path& a, const fs::path& b) {
    PcmAudio x, y;
    std::string err;
    REQUIRE(readPcm(a, x, err));
    REQUIRE(readPcm(b, y, err));
    return x.channels == y.channels && x.sampleRate == y.sampleRate && x.samples == y.samples;
}

std::shared_ptr<ExportJob> startExport(const SessionDoc& doc, const fs::path& audioFile,
                                       const ExportOptions& opt, WorkerPool& pool, std::string& err) {
    AudioBuffer input;
    REQUIRE(decodeToStereo(audioFile, doc.audio.sampleRate, input, err));
    return ExportJob::start(pool, registry(), descriptionFromSession(doc), input, opt, err);
}

void waitDone(ExportJob& job) {
    for (int i = 0; i < 60000 && !job.done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(job.done());
}
} // namespace

TEST_CASE("export: equals dsptb-cli render of the same session, and is repeatable") {
    const fs::path dir = tempDir() / "export";
    fs::create_directories(dir);
    const fs::path audio = makeWav(dir / "in.wav", 3.0);

    json j = {
        {"format", "dsptb-session"},
        {"version", 1},
        {"audio", {{"sampleRate", 48000}}},
        {"source", {{"type", "file"}, {"path", toUtf8(audio)}, {"relativePath", "in.wav"}}},
        {"instances",
         json::array({{{"module", "org.dsptb.spatializer"},
                       {"role", "processor"},
                       {"params", {{"output_gain_db", -2.0}}},
                       {"state", {{"hrtf", "builtin"}, {"sources", json::array({{{"motion", "orbit"}}})}}}},
                      {{"module", "org.dsptb.gain"}, {"params", {{"gain_db", -5.0}}}},
                      {{"module", "org.dsptb.levelmeter"}, {"role", "analyzer"}}})}};
    const fs::path sessionFile = dir / "s.dsptb.json";
    {
        std::ofstream(sessionFile, std::ios::binary) << j.dump(2);
    }
    SessionDoc doc;
    std::string err;
    REQUIRE(readSessionFile(sessionFile, doc, err));

    // CLI: no --in, so the session's own audio file is used.
    const fs::path cliOut = dir / "cli.wav";
    const RunResult cli =
        runCli("render --session " + q(sessionFile) + " --out " + q(cliOut) + " --format f32 --tail 2");
    INFO(cli.out);
    REQUIRE(cli.exitCode == 0);

    WorkerPool pool(2);
    ExportOptions opt;
    opt.format = WavFormat::F32;
    opt.tailSeconds = 2.0;
    opt.out = dir / "export1.wav";
    auto job = startExport(doc, audio, opt, pool, err);
    INFO(err);
    REQUIRE(job != nullptr);
    waitDone(*job);
    CHECK(job->error().empty());
    REQUIRE(job->succeeded());
    CHECK(sameSamples(cliOut, opt.out));

    opt.out = dir / "export2.wav";
    auto job2 = startExport(doc, audio, opt, pool, err);
    REQUIRE(job2 != nullptr);
    waitDone(*job2);
    REQUIRE(job2->succeeded());
    CHECK(readText(dir / "export1.wav") == readText(dir / "export2.wav")); // byte for byte
}

TEST_CASE("export: options (loop region, tail, 16-bit) are honoured") {
    const fs::path dir = tempDir() / "export_opts";
    fs::create_directories(dir);
    const fs::path audio = makeWav(dir / "in.wav", 4.0);
    SessionDoc doc;
    doc.audio.sampleRate = 48000;
    WorkerPool pool(1);
    std::string err;

    ExportOptions opt;
    opt.out = dir / "region.wav";
    opt.loopRegionOnly = true;
    opt.regionStartSeconds = 1.0;
    opt.regionEndSeconds = 2.5;
    opt.tailSeconds = 0.5;
    opt.format = WavFormat::S16;
    auto job = startExport(doc, audio, opt, pool, err);
    REQUIRE(job != nullptr);
    waitDone(*job);
    REQUIRE(job->succeeded());
    PcmAudio out;
    REQUIRE(readPcm(opt.out, out, err));
    CHECK(out.frames() == static_cast<size_t>((1.5 + 0.5) * 48000));
    CHECK_FALSE(fs::exists(fs::path(opt.out) += ".part"));
}

TEST_CASE("export: cancelling at 50 percent leaves no output file") {
    const fs::path dir = tempDir() / "export_cancel";
    fs::create_directories(dir);
    const fs::path audio = makeWav(dir / "in.wav", 240.0); // long enough that 50 % takes a moment
    json j = {{"format", "dsptb-session"},
              {"audio", {{"sampleRate", 48000}}},
              {"instances",
               json::array({{{"module", "org.dsptb.spatializer"}, {"state", {{"hrtf", "builtin"}}}},
                            {{"module", "org.dsptb.spatializer"}, {"state", {{"hrtf", "builtin"}}}}})}};
    SessionDoc doc;
    std::string err;
    REQUIRE(sessionFromJson(j.dump(), doc, err));

    WorkerPool pool(1);
    ExportOptions opt;
    opt.out = dir / "never.wav";
    auto job = startExport(doc, audio, opt, pool, err);
    REQUIRE(job != nullptr);
    while (!job->done() && job->progress() < 0.5f)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_FALSE(job->done()); // still running at 50 %
    job->cancel();
    waitDone(*job);
    CHECK(job->cancelled());
    CHECK_FALSE(job->succeeded());
    CHECK_FALSE(fs::exists(opt.out));
    CHECK_FALSE(fs::exists(fs::path(opt.out) += ".part"));
}

TEST_CASE("export: a session with a missing module is refused up front") {
    SessionDoc doc;
    SessionInstanceDoc d;
    d.module = "org.example.missing";
    doc.instances.push_back(d);
    AudioBuffer in;
    in.sampleRate = 48000;
    in.resize(1000);
    WorkerPool pool(1);
    ExportOptions opt;
    opt.out = tempDir() / "x.wav";
    std::string err;
    CHECK(ExportJob::start(pool, registry(), descriptionFromSession(doc), in, opt, err) == nullptr);
    CHECK_FALSE(err.empty());
}

// ----------------------------------------------------------------------------------------------------------
// Recorder
// ----------------------------------------------------------------------------------------------------------

namespace {
// Pumps `seconds` of audio in half-second steps, letting the writer thread drain between them (the ring holds
// four seconds; pumping a manual backend flat out would outrun any real-time writer).
void pumpWithWriter(Rig& r, Recorder& rec, double seconds) {
    constexpr double kStep = 0.5;
    uint64_t pumped = 0;
    for (double t = 0.0; t < seconds - 1e-9; t += kStep) {
        const size_t frames = static_cast<size_t>(kStep * 48000);
        r.audio.engine().nullBackend()->pump(frames, 256);
        pumped += frames;
        for (int i = 0; i < 1000 && rec.framesWritten() + 4096 < pumped; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        r.tick(kStep);
    }
}

double energyOf(const PcmAudio& pcm) {
    double e = 0.0;
    for (float v : pcm.samples)
        e += static_cast<double>(v) * v;
    return e;
}
} // namespace

TEST_CASE("recorder: POST for 10 s gives a 10 s WAV, no overruns") {
    Rig r;
    r.add("org.dsptb.gain");
    r.audio.openFile(makeWav(tempDir() / "rec_src.wav", 12.0));
    r.waitForLoad();
    r.audio.engine().play();

    Recorder rec;
    std::string err;
    const fs::path folder = tempDir() / "rec";
    REQUIRE(rec.start(r.model, 48000, DSPTB_TAP_POST, folder, err));
    CHECK(rec.active());
    pumpWithWriter(r, rec, 10.0);
    const fs::path file = rec.stop(r.model);
    CHECK_FALSE(rec.active());
    CHECK(rec.overrunFrames() == 0);

    CHECK(file.filename().string().rfind("rec_", 0) == 0);
    CHECK(file.extension() == ".wav");
    PcmAudio pcm;
    REQUIRE(readPcm(file, pcm, err));
    CHECK(pcm.channels == 2);
    CHECK(pcm.sampleRate == 48000);
    CHECK(std::llabs(static_cast<long long>(pcm.frames()) - 480000) <= 256);
    CHECK(energyOf(pcm) > 1.0); // it recorded the playing file, not silence
}

TEST_CASE("recorder: PRE records the input, before the processors") {
    Rig r;
    const InstanceId gain = r.add("org.dsptb.gain");
    r.setParam(gain, "mute", 1.0f); // POST would be silent
    r.audio.openFile(makeWav(tempDir() / "rec_src2.wav", 3.0));
    r.waitForLoad();
    r.audio.engine().play();

    Recorder rec;
    std::string err;
    REQUIRE(rec.start(r.model, 48000, DSPTB_TAP_PRE, tempDir() / "rec_pre", err));
    pumpWithWriter(r, rec, 1.0);
    const fs::path file = rec.stop(r.model);
    PcmAudio pcm;
    REQUIRE(readPcm(file, pcm, err));
    CHECK(energyOf(pcm) > 1.0);
}

TEST_CASE("recorder: a second start while recording is refused; stop is idempotent") {
    Rig r;
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(r.model, 48000, DSPTB_TAP_POST, tempDir() / "rec_twice", err));
    CHECK_FALSE(rec.start(r.model, 48000, DSPTB_TAP_POST, tempDir() / "rec_twice", err));
    rec.stop(r.model);
    rec.stop(r.model);
    CHECK_FALSE(rec.active());
}
