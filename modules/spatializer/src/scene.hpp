#pragma once

#include <cmath>

// The Spatializer's scene model: up to 8 sources, each static, on an orbit or
// on a keyframe path. The UI thread owns a `Scene`; the audio thread only ever sees an immutable
// `SceneSnapshot` built from it and handed over through RtSwap.

#include <dsptb/spatial_motion/motion.hpp>
#include <dsptb/state_json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dsptb::spat {

namespace sm = dsptb::spatial;

constexpr int kMaxSources = 8;
constexpr float kMinDistanceM =
    0.01f; // HRTF singularity guard (spec: Processing 5); the Near range goes to the ears
constexpr float kMaxDistanceM = 50.0f;

// How far from the head the views reach and how finely the grid snaps. The Near range (ASMR: whispers at
// the ear) tops out at 2 m with no practical minimum.
enum class Range : uint8_t { Near, Mid, Far };
constexpr float rangeMaxM(Range r) {
    return r == Range::Near ? 2.0f : r == Range::Mid ? 10.0f : kMaxDistanceM;
}
constexpr float rangeGridM(Range r) {
    return r == Range::Near ? 0.25f : r == Range::Mid ? 1.0f : 5.0f;
}
// What the range preset sets the module's parameters to: attenuation reference distance (m) and the air
// absorption amount (x the physical coefficients, exaggerated at close range where real air does nothing).
constexpr float rangeRefDistanceM(Range r) {
    return r == Range::Near ? 0.1f : r == Range::Mid ? 1.0f : 5.0f;
}
constexpr float rangeAirAmount(Range r) {
    return r == Range::Near ? 20.0f : r == Range::Mid ? 5.0f : 1.0f;
}

enum class InputSel : uint8_t { MonoSum, Left, Right };
enum class Motion : uint8_t { Static, Orbit, Path };
// Auto: the transport for a file (and always in an offline render), the free clock for the microphone.
enum class ClockSel : uint8_t { Auto, Transport, Free };

struct OrbitSettings {
    sm::OrbitShape shape = sm::OrbitShape::Circle;
    sm::Vec3 center{0, 0, 0};
    float radius = 2.0f;       // 0.2..20 m
    float elevationDeg = 0.0f; // tilts the plane, -90..90
    float periodS = 8.0f;      // 0.5..120 s
    sm::Direction direction = sm::Direction::Clockwise;
    float startAngleDeg = 0.0f;
};

struct PathSettings {
    sm::PathSpec spec; // keys (1..64 once edited), mode, closeLoop, speed
    ClockSel clock = ClockSel::Auto;
    float startOffsetS = 0.0f; // path time = clock - startOffset
};

struct Source {
    std::string name = "Source 1";
    InputSel input = InputSel::MonoSum;
    uint32_t colour = 0xFF5AA0E6; // 0xAABBGGRR, display only
    float gainDb = 0.0f;
    bool mute = false;
    bool solo = false;
    Motion motion = Motion::Static;
    sm::Vec3 position{0, 0, -2}; // Static position, and the edit cursor
    OrbitSettings orbit;
    PathSettings path;
};

struct Scene {
    std::vector<Source> sources; // 1..8
    int selected = 0;
    Range range = Range::Mid;
    // HRTF set: "auto" (the packaged set for the range), "builtin" (Steam Audio's), or a set id.
    std::string hrtf = "auto";
    bool snap = false; // snap dragged positions to the range's grid (view only; never touches the audio)
};

// The elevation band (degrees) in which a packaged HRTF set was measured. Steam Audio answers a direction
// outside the band with the nearest measured one, so a source there would keep the azimuth but get no height
// cue, and near the pole the azimuth swings wildly (measured: left/right flips for tiny moves). The module
// therefore bounds the elevation of sources to the band. Sets not listed (built-in, custom, SADIE) cover the
// whole sphere or are unknown, and are not bounded.
struct ElevationBand {
    float minDeg = -90.0f, maxDeg = 90.0f;
};
inline ElevationBand elevationBandOf(const std::string& hrtfId) {
    if (hrtfId == "near-kemar-20cm")
        return {-5.0f, 35.0f};
    if (hrtfId == "near-kemar-50cm")
        return {-25.0f, 35.0f};
    return {};
}
// Moves `pos` (listener-centric, +y up) into the band, keeping its distance and azimuth.
inline sm::Vec3 limitElevation(const sm::Vec3& pos, const ElevationBand& band) {
    const float d = std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
    if (d < 1e-6f || (band.minDeg <= -90.0f && band.maxDeg >= 90.0f))
        return pos;
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    const float elDeg = std::asin(std::fmax(-1.0f, std::fmin(1.0f, pos.y / d))) / kDeg;
    if (elDeg >= band.minDeg && elDeg <= band.maxDeg)
        return pos;
    const float lim = (elDeg < band.minDeg ? band.minDeg : band.maxDeg) * kDeg;
    const float h = std::sqrt(pos.x * pos.x + pos.z * pos.z);
    const float want = d * std::cos(lim);
    if (h < 1e-6f) // straight above or below: azimuth is undefined, use the front
        return {0.0f, d * std::sin(lim), -want};
    return {pos.x * want / h, d * std::sin(lim), pos.z * want / h};
}

// Pulls every position, keyframe and orbit radius of the scene inside `r` (after the range was narrowed).
void fitToRange(Scene& scene, Range r);

Scene defaultScene();
Source makeSource(int index); // name, colour and a spread-out start position for source number `index`
Json toJson(const Scene& scene);
// Reads a scene leniently: missing or invalid fields take defaults, numbers are clamped to their ranges.
// Always leaves a valid scene (at least one source).
Scene sceneFromJson(const Json& j);

// Conversions for the UI's typed fields.
sm::Spherical toSph(const sm::Vec3& v);
sm::Vec3 fromSph(float azDeg, float elDeg, float distance);

// Orbit settings -> spatial_motion parameters (the module's "elevation" is the plane's tilt).
sm::OrbitParams toOrbitParams(const OrbitSettings& o);

// ------------------------------------------------------------------------------------------------------------
// audio-thread view
// ------------------------------------------------------------------------------------------------------------

struct RtSource {
    InputSel input = InputSel::MonoSum;
    float gainDb = 0.0f;
    bool mute = false;
    Motion motion = Motion::Static;
    sm::Vec3 position{0, 0, -2};
    sm::OrbitParams orbit;
    sm::PathEvaluator path;
    ClockSel clock = ClockSel::Auto;
    float startOffsetS = 0.0f;
};

struct SceneSnapshot {
    int count = 0;
    bool anySolo = false;
    std::array<bool, kMaxSources> solo{};
    std::array<RtSource, kMaxSources> src;
};

SceneSnapshot makeSnapshot(const Scene& scene);

// Live positions the audio thread reports to the UI after each internal block.
struct LivePositions {
    int count = 0;
    std::array<sm::Vec3, kMaxSources> pos{};
};

} // namespace dsptb::spat
