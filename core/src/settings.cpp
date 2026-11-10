#include "dsptb/core/settings.hpp"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace dsptb {

using nlohmann::json;

int snapUiScalePercent(int percent) {
    constexpr int steps[] = {100, 125, 150, 200};
    int best = steps[0];
    for (int s : steps)
        if (std::abs(s - percent) < std::abs(best - percent))
            best = s;
    return best;
}

void Settings::pushRecent(std::vector<std::string>& list, const std::string& path) {
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > static_cast<size_t>(kMaxRecent))
        list.resize(static_cast<size_t>(kMaxRecent));
}

std::string Settings::toJsonString() const {
    json j;
    j["version"] = kVersion;
    j["window"] = {
        {"x", window.x}, {"y", window.y}, {"w", window.w}, {"h", window.h}, {"maximized", window.maximized}};
    j["uiScalePercent"] = uiScalePercent;
    j["audio"] = {{"backend", audio.backend},
                  {"inputDeviceId", audio.inputDeviceId},
                  {"outputDeviceId", audio.outputDeviceId},
                  {"sampleRate", audio.sampleRate},
                  {"bufferFrames", audio.bufferFrames}};
    j["gpu"] = {{"runtimeDir", gpu.runtimeDir}, {"device", gpu.device}, {"sync", gpu.sync}};
    j["micMonitorWarningShown"] = micMonitorWarningShown;
    j["micGateEnabled"] = micGateEnabled;
    j["micGateThresholdDb"] = micGateThresholdDb;
    j["recentSessions"] = recentSessions;
    j["recentAudioFiles"] = recentAudioFiles;
    j["recordFolder"] = recordFolder;
    j["recordTap"] = recordTap;
    j["layoutIni"] = layoutIni;
    j["panelsOpen"] = panelsOpen;
    return j.dump(2);
}

bool Settings::fromJsonString(const std::string& text) {
    // Parse into a temporary so a corrupt file leaves *this untouched.
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object())
        return false;
    Settings s;
    if (auto it = j.find("window"); it != j.end() && it->is_object()) {
        s.window.x = it->value("x", s.window.x);
        s.window.y = it->value("y", s.window.y);
        s.window.w = it->value("w", s.window.w);
        s.window.h = it->value("h", s.window.h);
        s.window.maximized = it->value("maximized", false);
        s.window.valid = it->contains("w") && it->contains("h");
    }
    s.uiScalePercent = snapUiScalePercent(j.value("uiScalePercent", 100));
    if (auto it = j.find("audio"); it != j.end() && it->is_object()) {
        s.audio.backend = it->value("backend", s.audio.backend);
        s.audio.inputDeviceId = it->value("inputDeviceId", std::string{});
        s.audio.outputDeviceId = it->value("outputDeviceId", std::string{});
        s.audio.sampleRate = it->value("sampleRate", s.audio.sampleRate);
        s.audio.bufferFrames = it->value("bufferFrames", s.audio.bufferFrames);
    }
    if (auto it = j.find("gpu"); it != j.end() && it->is_object()) {
        s.gpu.runtimeDir = it->value("runtimeDir", std::string{});
        s.gpu.device = std::max(0, it->value("device", 0));
        s.gpu.sync = it->value("sync", std::string("blocking")) == "spin" ? "spin" : "blocking";
    }
    s.micMonitorWarningShown = j.value("micMonitorWarningShown", false);
    s.micGateEnabled = j.value("micGateEnabled", false);
    s.micGateThresholdDb = std::clamp(j.value("micGateThresholdDb", -50.0f), -80.0f, -20.0f);
    auto readList = [&](const char* key, std::vector<std::string>& out) {
        if (auto it = j.find(key); it != j.end() && it->is_array())
            for (const auto& e : *it)
                if (e.is_string())
                    out.push_back(e.get<std::string>());
        if (out.size() > static_cast<size_t>(kMaxRecent))
            out.resize(static_cast<size_t>(kMaxRecent));
    };
    readList("recentSessions", s.recentSessions);
    readList("recentAudioFiles", s.recentAudioFiles);
    s.recordFolder = j.value("recordFolder", std::string{});
    s.recordTap = j.value("recordTap", std::string("post")) == "pre" ? "pre" : "post";
    s.layoutIni = j.value("layoutIni", std::string{});
    if (auto it = j.find("panelsOpen"); it != j.end() && it->is_object())
        for (auto e = it->begin(); e != it->end(); ++e)
            if (e.value().is_boolean())
                s.panelsOpen[e.key()] = e.value().get<bool>();
    *this = std::move(s);
    return true;
}

bool Settings::save(const std::filesystem::path& path) const {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << toJsonString();
        if (!out.good())
            return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

Settings::LoadResult Settings::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return LoadResult::Missing;
    std::stringstream ss;
    ss << in.rdbuf();
    return fromJsonString(ss.str()) ? LoadResult::Loaded : LoadResult::Corrupt;
}

} // namespace dsptb
