#include "dsptb/core/session_io.hpp"

#include "dsptb/core/paths.hpp"
#include "dsptb/core/version.hpp"

#include <dsptb/branding.h>

#include <spdlog/spdlog.h>

namespace dsptb {

namespace {

const char* routeName(InputRoute route) {
    return route == InputRoute::Chain ? "chain" : route == InputRoute::Output ? "output" : "off";
}

InputRoute parseRoute(const std::string& name) {
    return name == "chain" ? InputRoute::Chain : name == "output" ? InputRoute::Output : InputRoute::Off;
}

} // namespace

SessionDoc captureSession(const AudioSession& audio, const SessionModel& model,
                          const std::filesystem::path& sessionFile) {
    SessionDoc doc;
    doc.appName = DSPTB_APP_NAME;
    doc.appVersion = versionString();
    const AudioConfig& c = audio.config();
    doc.audio.backend = backendName(c.backend);
    doc.audio.input = c.inputDeviceId;
    doc.audio.output = c.outputDeviceId;
    doc.audio.sampleRate = c.sampleRate;
    doc.audio.bufferFrames = c.bufferFrames;

    const Engine& e = audio.engine();
    doc.masterGainDb = static_cast<double>(e.masterGainDb());
    SessionSourceDoc& s = doc.source;
    doc.inputs.file.route = routeName(e.route(SourceKind::File));
    doc.inputs.file.gainDb = static_cast<double>(e.outputRouteGainDb(SourceKind::File));
    doc.inputs.mic.route = routeName(e.route(SourceKind::Mic));
    doc.inputs.mic.gainDb = static_cast<double>(e.outputRouteGainDb(SourceKind::Mic));
    s.monitorMic = e.monitorMic();
    s.loopEnabled = audio.loopEnabled();
    s.loopStart = audio.loopStartSeconds();
    s.loopEnd = audio.loopEndSeconds();
    if (audio.file()) {
        s.path = audio.file()->path;
        if (!sessionFile.empty())
            s.relativePath = relativeToSession(sessionFile, fromUtf8(audio.file()->path));
    }
    doc.instances = model.capture();
    return doc;
}

void resetSession(AudioSession& audio, SessionModel& model) {
    model.clear();
    audio.closeFile();
    audio.setLoopSeconds(false, 0.0, 0.0);
    audio.engine().setMasterGainDb(0.0f);
    audio.engine().setMonitorMic(false);
    audio.engine().setOutputRouteGainDb(SourceKind::File, 0.0f);
    audio.engine().setOutputRouteGainDb(SourceKind::Mic, 0.0f);
    std::string err;
    audio.setSource(SourceKind::File, err);
}

ApplyResult applySession(const SessionDoc& doc, const std::filesystem::path& sessionFile, AudioSession& audio,
                         SessionModel& model, std::vector<SessionMessage>& messages) {
    ApplyResult result;

    if (doc.hasAudio) {
        AudioConfig want = audio.config();
        BackendKind kind;
        // A session saved under --null-audio or a test must not silence a real device: the null backend is
        // never taken from a file.
        if (parseBackendKind(doc.audio.backend, kind) && kind != BackendKind::Null) {
            want.backend = kind;
            want.inputDeviceId = doc.audio.input;
            want.outputDeviceId = doc.audio.output;
            if (doc.audio.sampleRate > 0)
                want.sampleRate = doc.audio.sampleRate;
            if (doc.audio.bufferFrames > 0)
                want.bufferFrames = doc.audio.bufferFrames;
            const AudioConfig& have = audio.config();
            if (want.backend != have.backend || want.inputDeviceId != have.inputDeviceId ||
                want.outputDeviceId != have.outputDeviceId || want.sampleRate != have.sampleRate ||
                want.bufferFrames != have.bufferFrames) {
                std::string error;
                if (!audio.applyConfig(want, error)) {
                    messages.push_back(
                        {NoticeLevel::Warning, "The audio settings saved in this session could not "
                                               "be used (" +
                                                   error + "); keeping the current ones."});
                }
            }
        }
    }

    audio.engine().setMasterGainDb(static_cast<float>(doc.masterGainDb));
    audio.engine().setMonitorMic(doc.source.monitorMic);
    audio.setLoopSeconds(doc.source.loopEnabled, doc.source.loopStart, doc.source.loopEnd);

    model.restore(doc.instances, messages);

    audio.engine().setOutputRouteGainDb(SourceKind::File, static_cast<float>(doc.inputs.file.gainDb));
    audio.engine().setOutputRouteGainDb(SourceKind::Mic, static_cast<float>(doc.inputs.mic.gainDb));
    const InputRoute fileRoute = parseRoute(doc.inputs.file.route);
    std::string error;
    if (!audio.setRoutes(fileRoute, parseRoute(doc.inputs.mic.route), error)) {
        messages.push_back({NoticeLevel::Warning, "Could not set the session's inputs: " + error});
        if (!audio.setSource(SourceKind::File, error))
            spdlog::warn("session: could not select the file source: {}", error);
    } else if (fileRoute == InputRoute::Off) {
        return result;
    }

    if (doc.source.path.empty() && doc.source.relativePath.empty()) {
        audio.closeFile();
        return result;
    }
    result.wantsFile = true;
    const std::filesystem::path file = resolveSessionAudio(sessionFile, doc.source);
    if (file.empty()) {
        result.fileMissing = true;
        result.missingPath = doc.source.path;
        audio.closeFile();
    } else {
        audio.openFile(file);
    }
    return result;
}

} // namespace dsptb
