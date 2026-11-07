#include "dsptb/core/session_file.hpp"

#include "dsptb/core/paths.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <fstream>
#include <sstream>

namespace dsptb {

namespace fs = std::filesystem;
using nlohmann::json;

std::string sessionToJson(const SessionDoc& doc) {
    json j;
    j["format"] = "dsptb-session";
    j["version"] = SessionDoc::kVersion;
    j["app"] = {{"name", doc.appName}, {"version", doc.appVersion}};
    j["audio"] = {{"backend", doc.audio.backend},
                  {"input", doc.audio.input},
                  {"output", doc.audio.output},
                  {"sampleRate", doc.audio.sampleRate},
                  {"bufferFrames", doc.audio.bufferFrames}};
    const SessionSourceDoc& s = doc.source;
    j["source"] = {{"type", s.type},
                   {"path", s.path},
                   {"relativePath", s.relativePath},
                   {"loop", {{"enabled", s.loopEnabled}, {"start", s.loopStart}, {"end", s.loopEnd}}},
                   {"monitorMic", s.monitorMic},
                   {"backingGainDb", s.backingGainDb}};
    j["masterGainDb"] = doc.masterGainDb;
    json instances = json::array();
    for (const SessionInstanceDoc& i : doc.instances) {
        json ji;
        ji["instanceId"] = i.instanceId;
        ji["module"] = i.module;
        ji["moduleVersion"] = i.moduleVersion;
        ji["enabled"] = i.enabled;
        ji["role"] = i.role;
        ji["tap"] = i.tap.empty() ? json(nullptr) : json(i.tap);
        ji["label"] = i.label;
        ji["panelOpen"] = i.panelOpen;
        json params = json::object();
        for (const auto& [id, v] : i.params)
            params[id] = v;
        ji["params"] = std::move(params);
        json state = i.stateJson.empty() ? json::object()
                                         : json::parse(i.stateJson, nullptr, /*allow_exceptions=*/false);
        if (state.is_discarded()) {
            spdlog::warn("session: the state of {} is not valid JSON; saved as empty", i.module);
            state = json::object();
        }
        ji["state"] = std::move(state);
        instances.push_back(std::move(ji));
    }
    j["instances"] = std::move(instances);
    j["layout"] = doc.layout;
    return j.dump(2) + "\n";
}

namespace {
bool parseSession(const std::string& text, SessionDoc& out, std::string& error) {
    out = SessionDoc{};
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        error = "the session file is not a JSON object";
        return false;
    }
    if (j.value("format", std::string()) != "dsptb-session") {
        error = "not a dsptb-session file (missing \"format\": \"dsptb-session\")";
        return false;
    }
    if (j.value("version", 1) > SessionDoc::kVersion) {
        error = "this session was saved by a newer version of the app (format version " +
                std::to_string(j.value("version", 0)) + ")";
        return false;
    }
    if (j.contains("app") && j["app"].is_object()) {
        out.appName = j["app"].value("name", std::string());
        out.appVersion = j["app"].value("version", std::string());
    }
    if (j.contains("audio") && j["audio"].is_object()) {
        const json& a = j["audio"];
        out.hasAudio = true;
        out.audio.backend = a.value("backend", out.audio.backend);
        out.audio.input = a.value("input", std::string());
        out.audio.output = a.value("output", std::string());
        out.audio.sampleRate = a.value("sampleRate", 0u); // 0 keeps the file's rate (offline)
        out.audio.bufferFrames = a.value("bufferFrames", out.audio.bufferFrames);
    } else {
        out.audio.sampleRate = 0;
    }
    if (j.contains("source") && j["source"].is_object()) {
        const json& s = j["source"];
        SessionSourceDoc& d = out.source;
        d.type = s.value("type", d.type);
        d.path = s.value("path", std::string());
        d.relativePath = s.value("relativePath", std::string());
        if (s.contains("loop") && s["loop"].is_object()) {
            d.loopEnabled = s["loop"].value("enabled", false);
            d.loopStart = s["loop"].value("start", 0.0);
            d.loopEnd = s["loop"].value("end", 0.0);
        }
        d.monitorMic = s.value("monitorMic", false);
        d.backingGainDb = s.value("backingGainDb", 0.0);
    }
    out.masterGainDb = j.value("masterGainDb", 0.0);
    out.layout = j.value("layout", std::string());

    if (j.contains("instances")) {
        if (!j["instances"].is_array()) {
            error = "\"instances\" must be an array";
            return false;
        }
        size_t n = 0;
        for (const json& ji : j["instances"]) {
            ++n;
            if (!ji.is_object() || !ji.contains("module") || !ji["module"].is_string()) {
                error = "every instance needs a \"module\" id";
                return false;
            }
            SessionInstanceDoc d;
            d.instanceId = ji.value("instanceId", "i" + std::to_string(n));
            d.module = ji["module"].get<std::string>();
            d.moduleVersion = ji.value("moduleVersion", std::string());
            d.enabled = ji.value("enabled", true);
            d.role = ji.value("role", std::string());
            if (ji.contains("tap") && ji["tap"].is_string())
                d.tap = ji["tap"].get<std::string>();
            d.label = ji.value("label", std::string());
            d.panelOpen = ji.value("panelOpen", true);
            if (ji.contains("params")) {
                if (!ji["params"].is_object()) {
                    error = "\"params\" must be an object";
                    return false;
                }
                for (auto it = ji["params"].begin(); it != ji["params"].end(); ++it) {
                    if (it.value().is_boolean())
                        d.params[it.key()] = it.value().get<bool>() ? 1.0 : 0.0;
                    else if (it.value().is_number())
                        d.params[it.key()] = it.value().get<double>();
                    else {
                        error = "parameter \"" + it.key() + "\" must be a number or a boolean";
                        return false;
                    }
                }
            }
            if (ji.contains("state") && ji["state"].is_object())
                d.stateJson = ji["state"].dump();
            out.instances.push_back(std::move(d));
        }
    }
    return true;
}
} // namespace

bool sessionFromJson(const std::string& text, SessionDoc& out, std::string& error) {
    // json::value() throws when a key has the wrong type (e.g. "masterGainDb": "loud"); a hand-edited file must
    // be refused with a message, never crash the caller.
    try {
        return parseSession(text, out, error);
    } catch (const json::exception& e) {
        out = SessionDoc{};
        error = std::string("the session file has a value of the wrong type (") + e.what() + ")";
        return false;
    }
}

std::string relativeToSession(const fs::path& sessionFile, const fs::path& audio) {
    std::error_code ec;
    const fs::path base = fs::absolute(sessionFile, ec).parent_path();
    const fs::path rel = fs::relative(audio, base, ec);
    if (ec || rel.empty())
        return {};
    const auto u8 = rel.generic_u8string(); // '/' separators on every platform
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path resolveSessionAudio(const fs::path& sessionFile, const SessionSourceDoc& source) {
    std::error_code ec;
    if (!source.path.empty()) {
        const fs::path abs = fromUtf8(source.path);
        if (fs::exists(abs, ec))
            return abs;
    }
    if (!source.relativePath.empty()) {
        const fs::path beside = fs::absolute(sessionFile, ec).parent_path() / fromUtf8(source.relativePath);
        if (fs::exists(beside, ec))
            return beside.lexically_normal();
    }
    return {};
}

bool writeFileAtomic(const fs::path& file, const std::string& bytes, std::string& error) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + toUtf8(tmp);
            return false;
        }
        out << bytes;
        out.flush();
        if (!out.good()) {
            error = "writing " + toUtf8(tmp) + " failed (disk full?)";
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace " + toUtf8(file);
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool writeSessionFile(const fs::path& file, const SessionDoc& doc, std::string& error) {
    return writeFileAtomic(file, sessionToJson(doc), error);
}

bool readSessionFile(const fs::path& file, SessionDoc& out, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + toUtf8(file);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return sessionFromJson(ss.str(), out, error);
}

} // namespace dsptb
