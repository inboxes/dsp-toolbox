#pragma once

// Motion maths for the 3D Spatializer. Spherical conversions, orbit shapes (circle and figure-8) and a
// keyframe path evaluator with Step, Linear and Smooth (centripetal
// Catmull-Rom) easing in Once, Loop and PingPong modes.
//
// Everything here is real-time code: after construction (PathEvaluator: after
// rebuild) nothing allocates, locks or throws, and every function is a pure
// function of its arguments / the stored PathSpec — no clocks, no randomness
// — so two evaluators built from the same spec give bit-identical results.
//
// Thread safety: the free functions are stateless. A PathEvaluator must not
// be rebuilt concurrently with positionAt / segmentAt calls on the same
// instance; distinct instances are independent.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dsptb::spatial {

// A point or offset in listener space, in metres: +X right, +Y up,
// -Z forward (the listener sits at the origin, facing -Z).
struct Vec3 {
    float x, y, z;
};

// Spherical position in listener space: azimuth 0 = front, +90 = right,
// +/-180 = behind; elevation +90 = straight up, -90 = straight down;
// distance in metres.
struct Spherical {
    float azimuthDeg, elevationDeg, distance;
};

// Converts spherical to Cartesian with
//   x = d*cos(el)*sin(az), y = d*sin(el), z = -d*cos(el)*cos(az)
// (angles given in degrees, converted internally).
Vec3 fromSpherical(const Spherical& spherical) noexcept;

// Inverse of fromSpherical. The azimuth is reported in (-180, 180]. A zero
// vector (distance 0) reports azimuth = elevation = 0.
Spherical toSpherical(const Vec3& position) noexcept;

enum class OrbitShape : std::uint8_t { Circle, Figure8 };

// Orbit direction as seen from above, with the front of the room at the top:
// Clockwise travels front -> right -> back -> left.
enum class Direction : std::uint8_t { Clockwise, CounterClockwise };

struct OrbitParams {
    OrbitShape shape = OrbitShape::Circle;
    Vec3 center{0, 0, 0}; // metres
    float radius = 2;     // metres (Figure8: the half-width)
    float tiltDeg = 0;    // rotation about the X axis; positive raises the front
    float periodS = 8;    // seconds per revolution
    Direction direction = Direction::Clockwise;
    float startAngleDeg = 0; // angle at t = 0 (0 is the front for Circle)
};

// Position on the orbit at tSeconds (any real value; the motion is periodic
// with periodS). A non-positive periodS yields the static start-angle point.
Vec3 orbitPosition(const OrbitParams& params, double tSeconds) noexcept;

// How a path segment moves into its keyframe.
enum class Ease : std::uint8_t {
    Step,   // stay at the previous keyframe for the whole travel, then jump
    Linear, // lerp
    Smooth, // centripetal Catmull-Rom through the neighbouring keyframes,
            // with the segment parameter warped by u^2 * (3 - 2u)
};

enum class PlayMode : std::uint8_t { Once, Loop, PingPong };

struct Keyframe {
    Vec3 position;
    // Seconds to travel from the previous keyframe to this one (ignored for
    // keyframe 0 in the forward timeline; in Loop + closeLoop it is the
    // closing segment's travel time). <= 0 means an instant jump.
    float travelTimeS = 2;
    float holdS = 0; // seconds to stay at this keyframe before moving on
    Ease ease = Ease::Smooth; // easing of the segment arriving at this keyframe
};

constexpr std::size_t kMaxKeyframes = 64;

struct PathSpec {
    std::array<Keyframe, kMaxKeyframes> keys{};
    std::uint32_t count = 0; // number of valid entries in keys (clamped to kMaxKeyframes)
    PlayMode mode = PlayMode::Once;
    // Loop only: finish the pass with a segment travelling back to keyframe 0
    // (using keys[0].travelTimeS / keys[0].ease) instead of jumping back.
    bool closeLoop = true;
    float speed = 1; // time multiplier, applied inside positionAt / segmentAt
};

// Evaluates a keyframe path. rebuild() precomputes the segment start times
// into fixed member storage; positionAt() is then a closed-form lookup, so
// it can run on the audio thread (the Spatializer calls it every 256 samples
// per source). A default-constructed evaluator behaves as count == 0.
class PathEvaluator {
public:
    // Stores the spec and precomputes segment start times. No allocation.
    // Negative travel / hold times are treated as 0 (travel <= 0 is an
    // instant jump, per the spec).
    void rebuild(const PathSpec& spec) noexcept;

    // Length of one pass in path-time seconds (i.e. before the speed
    // multiplier): the sum of holds and travels, plus — in Loop mode with
    // closeLoop — the closing segment back to keyframe 0.
    double duration() const noexcept { return duration_; }

    // Position at path clock tSeconds. The stored speed multiplier is
    // applied inside (speed 2 halves the times). Negative t is treated as 0.
    // count == 0 yields the origin; count == 1 yields that keyframe.
    Vec3 positionAt(double tSeconds) const noexcept;

    // Index of the keyframe being travelled to or held at tSeconds (for UI
    // display); the closing segment reports 0. Same time mapping as
    // positionAt. count == 0 yields 0.
    std::uint32_t segmentAt(double tSeconds) const noexcept;

private:
    struct Located {
        Vec3 position;
        std::uint32_t segment;
    };

    // Maps a raw clock value through the negative clamp, the speed
    // multiplier and the play mode, giving a one-pass time in [0, duration].
    double mapTime(double tSeconds) const noexcept;
    // Position and target keyframe at an already-mapped one-pass time.
    Located locate(double pathTime) const noexcept;

    PathSpec spec_{};
    // The authoritative keyframe count: spec_.count clamped to
    // kMaxKeyframes (spec_.count itself keeps the raw requested value).
    // Every lookup in the evaluator reads count_, never spec_.count.
    std::uint32_t count_ = 0;
    // arrival_[i] = one-pass time at which keyframe i is reached:
    // arrival_[0] = 0, arrival_[i] = arrival_[i-1] + hold_{i-1} + travel_i.
    std::array<double, kMaxKeyframes> arrival_{};
    double duration_ = 0.0;
    bool closing_ = false; // a closing segment (Loop + closeLoop) ends the pass
};

} // namespace dsptb::spatial
