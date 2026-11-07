#include "scene.hpp"

#include <algorithm>
#include <cmath>

namespace dsptb::spat {

namespace {

constexpr uint32_t kPalette[kMaxSources] = {0xFF5AA0E6, 0xFF6BC46B, 0xFFE6A05A, 0xFFD070D0,
                                            0xFF5AD2E6, 0xFF6A6AE6, 0xFFE65A8C, 0xFFB4B4B4};

float clampf(float v, float lo, float hi) {
    if (!std::isfinite(v))
        return lo;
    return std::min(std::max(v, lo), hi);
}

Json vec3Json(const sm::Vec3& v) {
    return Json::array({v.x, v.y, v.z});
}

sm::Vec3 vec3From(const Json& j, const sm::Vec3& fallback) {
    if (!j.is_array() || j.size() != 3)
        return fallback;
    sm::Vec3 v = fallback;
    float* parts[3] = {&v.x, &v.y, &v.z};
    for (size_t i = 0; i < 3; ++i)
        if (j[i].is_number())
            *parts[i] = clampf(j[i].get<float>(), -kMaxDistanceM, kMaxDistanceM);
    return v;
}

const char* inputName(InputSel v) {
    return v == InputSel::Left ? "left" : v == InputSel::Right ? "right" : "mono";
}
const char* motionName(Motion v) {
    return v == Motion::Orbit ? "orbit" : v == Motion::Path ? "path" : "static";
}
const char* clockName(ClockSel v) {
    return v == ClockSel::Transport ? "transport" : v == ClockSel::Free ? "free" : "auto";
}
const char* easeName(sm::Ease v) {
    return v == sm::Ease::Step ? "step" : v == sm::Ease::Linear ? "linear" : "smooth";
}
const char* modeName(sm::PlayMode v) {
    return v == sm::PlayMode::Loop ? "loop" : v == sm::PlayMode::PingPong ? "pingpong" : "once";
}

// Cuts `s` to at most `maxBytes` without splitting a UTF-8 sequence (a split one would make json dump throw).
std::string truncateUtf8(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes)
        return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) // s[cut] is a continuation byte
        --cut;
    s.resize(cut);
    return s;
}

std::string str(const Json& j, const char* key, const std::string& fallback) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}
float num(const Json& j, const char* key, float fallback, float lo, float hi) {
    return j.contains(key) && j[key].is_number() ? clampf(j[key].get<float>(), lo, hi) : fallback;
}
bool flag(const Json& j, const char* key, bool fallback) {
    return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback;
}

} // namespace

Source makeSource(int index) {
    Source s;
    s.name = "Source " + std::to_string(index + 1);
    s.colour = kPalette[std::clamp(index, 0, kMaxSources - 1)];
    // Spread new sources round the front half so they do not sit on top of each other.
    const float az =
        index == 0 ? 0.0f : (index % 2 ? 1.0f : -1.0f) * 30.0f * static_cast<float>((index + 1) / 2);
    s.position = sm::fromSpherical({az, 0.0f, 2.0f});
    return s;
}

void fitToRange(Scene& scene, Range r) {
    const float mx = rangeMaxM(r);
    const auto fit = [mx](sm::Vec3& v) {
        const float d = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (d > mx) {
            const float k = mx / d;
            v = {v.x * k, v.y * k, v.z * k};
        }
    };
    for (Source& s : scene.sources) {
        fit(s.position);
        fit(s.orbit.center);
        s.orbit.radius = std::min(s.orbit.radius, mx);
        for (uint32_t i = 0; i < s.path.spec.count; ++i)
            fit(s.path.spec.keys[i].position);
    }
}

Scene defaultScene() {
    Scene s;
    s.sources.push_back(makeSource(0));
    return s;
}

sm::Spherical toSph(const sm::Vec3& v) {
    return sm::toSpherical(v);
}
sm::Vec3 fromSph(float azDeg, float elDeg, float distance) {
    return sm::fromSpherical(
        {azDeg, clampf(elDeg, -90.0f, 90.0f), clampf(distance, kMinDistanceM, kMaxDistanceM)});
}

sm::OrbitParams toOrbitParams(const OrbitSettings& o) {
    sm::OrbitParams p;
    p.shape = o.shape;
    p.center = o.center;
    p.radius = o.radius;
    p.tiltDeg = o.elevationDeg;
    p.periodS = o.periodS;
    p.direction = o.direction;
    p.startAngleDeg = o.startAngleDeg;
    return p;
}

Json toJson(const Scene& scene) {
    Json sources = Json::array();
    for (const Source& s : scene.sources) {
        Json keys = Json::array();
        for (uint32_t i = 0; i < s.path.spec.count; ++i) {
            const sm::Keyframe& k = s.path.spec.keys[i];
            keys.push_back({{"position", vec3Json(k.position)},
                            {"travel_s", k.travelTimeS},
                            {"hold_s", k.holdS},
                            {"ease", easeName(k.ease)}});
        }
        sources.push_back(
            {{"name", s.name},
             {"input", inputName(s.input)},
             {"colour", Json::array({s.colour & 0xFF, (s.colour >> 8) & 0xFF, (s.colour >> 16) & 0xFF})},
             {"gain_db", s.gainDb},
             {"mute", s.mute},
             {"solo", s.solo},
             {"motion", motionName(s.motion)},
             {"position", vec3Json(s.position)},
             {"orbit",
              {{"shape", s.orbit.shape == sm::OrbitShape::Figure8 ? "figure8" : "circle"},
               {"center", vec3Json(s.orbit.center)},
               {"radius", s.orbit.radius},
               {"elevation", s.orbit.elevationDeg},
               {"period_s", s.orbit.periodS},
               {"direction", s.orbit.direction == sm::Direction::CounterClockwise ? "ccw" : "cw"},
               {"start_angle", s.orbit.startAngleDeg}}},
             {"path",
              {{"keys", keys},
               {"mode", modeName(s.path.spec.mode)},
               {"speed", s.path.spec.speed},
               {"close_loop", s.path.spec.closeLoop},
               {"clock", clockName(s.path.clock)},
               {"start_offset_s", s.path.startOffsetS}}}});
    }
    return {{"version", 2},
            {"selected", scene.selected},
            {"range", scene.range == Range::Near  ? "near"
                      : scene.range == Range::Far ? "far"
                                                  : "mid"},
            {"snap", scene.snap},
            {"hrtf", scene.hrtf},
            {"sources", sources}};
}

Scene sceneFromJson(const Json& j) {
    Scene scene;
    if (j.contains("sources") && j["sources"].is_array()) {
        int index = 0;
        for (const Json& js : j["sources"]) {
            if (!js.is_object() || index >= kMaxSources)
                continue;
            Source s = makeSource(index);
            s.name = truncateUtf8(str(js, "name", s.name), 48);
            const std::string in = str(js, "input", "mono");
            s.input = in == "left" ? InputSel::Left : in == "right" ? InputSel::Right : InputSel::MonoSum;
            if (js.contains("colour") && js["colour"].is_array() && js["colour"].size() == 3) {
                uint32_t c[3];
                for (int i = 0; i < 3; ++i)
                    c[i] = js["colour"][static_cast<size_t>(i)].is_number()
                               ? static_cast<uint32_t>(
                                     clampf(js["colour"][static_cast<size_t>(i)].get<float>(), 0, 255))
                               : 128u;
                s.colour = 0xFF000000u | (c[2] << 16) | (c[1] << 8) | c[0];
            }
            s.gainDb = num(js, "gain_db", 0.0f, -60.0f, 12.0f);
            s.mute = flag(js, "mute", false);
            s.solo = flag(js, "solo", false);
            const std::string mo = str(js, "motion", "static");
            s.motion = mo == "orbit" ? Motion::Orbit : mo == "path" ? Motion::Path : Motion::Static;
            if (js.contains("position"))
                s.position = vec3From(js["position"], s.position);
            if (js.contains("orbit") && js["orbit"].is_object()) {
                const Json& jo = js["orbit"];
                s.orbit.shape = str(jo, "shape", "circle") == "figure8" ? sm::OrbitShape::Figure8
                                                                        : sm::OrbitShape::Circle;
                if (jo.contains("center"))
                    s.orbit.center = vec3From(jo["center"], s.orbit.center);
                s.orbit.radius = num(jo, "radius", 2.0f, 0.02f, kMaxDistanceM);
                s.orbit.elevationDeg = num(jo, "elevation", 0.0f, -90.0f, 90.0f);
                s.orbit.periodS = num(jo, "period_s", 8.0f, 0.5f, 120.0f);
                s.orbit.direction = str(jo, "direction", "cw") == "ccw" ? sm::Direction::CounterClockwise
                                                                        : sm::Direction::Clockwise;
                s.orbit.startAngleDeg = num(jo, "start_angle", 0.0f, -360.0f, 360.0f);
            }
            if (js.contains("path") && js["path"].is_object()) {
                const Json& jp = js["path"];
                const std::string mode = str(jp, "mode", "once");
                s.path.spec.mode = mode == "loop"       ? sm::PlayMode::Loop
                                   : mode == "pingpong" ? sm::PlayMode::PingPong
                                                        : sm::PlayMode::Once;
                s.path.spec.speed = num(jp, "speed", 1.0f, 0.1f, 10.0f);
                s.path.spec.closeLoop = flag(jp, "close_loop", true);
                const std::string clk = str(jp, "clock", "auto");
                s.path.clock = clk == "transport" ? ClockSel::Transport
                               : clk == "free"    ? ClockSel::Free
                                                  : ClockSel::Auto;
                s.path.startOffsetS = num(jp, "start_offset_s", 0.0f, -3600.0f, 3600.0f);
                s.path.spec.count = 0;
                if (jp.contains("keys") && jp["keys"].is_array())
                    for (const Json& jk : jp["keys"]) {
                        if (!jk.is_object() || s.path.spec.count >= sm::kMaxKeyframes)
                            continue;
                        sm::Keyframe k;
                        k.position =
                            jk.contains("position") ? vec3From(jk["position"], k.position) : k.position;
                        k.travelTimeS = num(jk, "travel_s", 2.0f, 0.0f, 600.0f);
                        k.holdS = num(jk, "hold_s", 0.0f, 0.0f, 600.0f);
                        const std::string e = str(jk, "ease", "smooth");
                        k.ease = e == "step"     ? sm::Ease::Step
                                 : e == "linear" ? sm::Ease::Linear
                                                 : sm::Ease::Smooth;
                        s.path.spec.keys[s.path.spec.count++] = k;
                    }
            }
            scene.sources.push_back(std::move(s));
            ++index;
        }
    }
    if (scene.sources.empty())
        scene.sources.push_back(makeSource(0));
    scene.selected =
        j.contains("selected") && j["selected"].is_number_integer() ? j["selected"].get<int>() : 0;
    scene.selected = std::clamp(scene.selected, 0, static_cast<int>(scene.sources.size()) - 1);
    // Scenes saved before the range presets (version 1) keep the whole 50 m reach.
    const std::string rg = str(j, "range", j.contains("version") && j["version"] == 1 ? "far" : "mid");
    scene.range = rg == "near" ? Range::Near : rg == "far" ? Range::Far : Range::Mid;
    scene.snap = flag(j, "snap", false);
    scene.hrtf = truncateUtf8(str(j, "hrtf", "auto"), 120);
    return scene;
}

SceneSnapshot makeSnapshot(const Scene& scene) {
    SceneSnapshot snap;
    snap.count = std::min<int>(static_cast<int>(scene.sources.size()), kMaxSources);
    for (int i = 0; i < snap.count; ++i) {
        const Source& s = scene.sources[static_cast<size_t>(i)];
        RtSource& r = snap.src[static_cast<size_t>(i)];
        r.input = s.input;
        r.gainDb = s.gainDb;
        r.mute = s.mute;
        r.motion = s.motion;
        r.position = s.position;
        r.orbit = toOrbitParams(s.orbit);
        r.path.rebuild(s.path.spec);
        r.clock = s.path.clock;
        r.startOffsetS = s.path.startOffsetS;
        snap.solo[static_cast<size_t>(i)] = s.solo;
        snap.anySolo = snap.anySolo || s.solo;
    }
    return snap;
}

} // namespace dsptb::spat
