// Tests for dsptb::spatial motion maths. Every acceptance
// criterion of the issue maps to a test case here. Expected values are
// hand-derived from the spec formulas (the arithmetic is shown in comments),
// never by calling the implementation under test.

#include <dsptb/spatial_motion/motion.hpp>

#include "support/alloc_counter.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <utility>

using dsptb::spatial::Direction;
using dsptb::spatial::Ease;
using dsptb::spatial::Keyframe;
using dsptb::spatial::OrbitParams;
using dsptb::spatial::OrbitShape;
using dsptb::spatial::PathEvaluator;
using dsptb::spatial::PathSpec;
using dsptb::spatial::PlayMode;
using dsptb::spatial::Spherical;
using dsptb::spatial::Vec3;
using dsptb::spatial::fromSpherical;
using dsptb::spatial::orbitPosition;
using dsptb::spatial::toSpherical;

// Real-time entry points must be noexcept (spec §5); pin it at compile time.
static_assert(noexcept(fromSpherical(std::declval<const Spherical&>())));
static_assert(noexcept(toSpherical(std::declval<const Vec3&>())));
static_assert(noexcept(orbitPosition(std::declval<const OrbitParams&>(), 0.0)));
static_assert(noexcept(std::declval<PathEvaluator&>().rebuild(std::declval<const PathSpec&>())));
static_assert(noexcept(std::declval<const PathEvaluator&>().positionAt(0.0)));
static_assert(noexcept(std::declval<const PathEvaluator&>().segmentAt(0.0)));
static_assert(noexcept(std::declval<const PathEvaluator&>().duration()));

namespace {

bool near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

void checkVec(const Vec3& p, double x, double y, double z, double tolerance) {
    CHECK(near(p.x, x, tolerance));
    CHECK(near(p.y, y, tolerance));
    CHECK(near(p.z, z, tolerance));
}

void checkVecNear(const Vec3& a, const Vec3& b, double tolerance) {
    CHECK(near(a.x, b.x, tolerance));
    CHECK(near(a.y, b.y, tolerance));
    CHECK(near(a.z, b.z, tolerance));
}

void checkVecExact(const Vec3& a, const Vec3& b) {
    CHECK(a.x == b.x);
    CHECK(a.y == b.y);
    CHECK(a.z == b.z);
}

double vecLength(const Vec3& p) {
    const double x = p.x;
    const double y = p.y;
    const double z = p.z;
    return std::sqrt(x * x + y * y + z * z);
}

double vecDistance(const Vec3& a, const Vec3& b) {
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    const double dz = static_cast<double>(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Smallest signed difference a - b in degrees, wrapped to [-180, 180).
double azimuthDifference(double a, double b) {
    double diff = std::fmod(a - b + 540.0, 360.0);
    if (diff < 0.0)
        diff += 360.0;
    return diff - 180.0;
}

Keyframe key(float x, float y, float z, float travelTimeS, float holdS, Ease ease) {
    Keyframe k{};
    k.position = {x, y, z};
    k.travelTimeS = travelTimeS;
    k.holdS = holdS;
    k.ease = ease;
    return k;
}

PathSpec specOf(const Keyframe* keys, std::uint32_t count, PlayMode mode) {
    PathSpec spec{};
    spec.count = count;
    spec.mode = mode;
    for (std::uint32_t i = 0; i < count; ++i)
        spec.keys[i] = keys[i];
    return spec;
}

// The issue's hand-computed timeline: A (hold 1 s), B (travel 2 s, hold
// 0.5 s), C (travel 1 s), all Linear.
//   phases: hold A [0,1) | travel B [1,3) | hold B [3,3.5) | travel C [3.5,4.5)
//   duration = 1 + 2 + 0.5 + 1 = 4.5 s
PathSpec handTimelineSpec(float speed) {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 1.0f, Ease::Linear),   // A
        key(2.0f, 0.0f, 0.0f, 2.0f, 0.5f, Ease::Linear),   // B
        key(2.0f, 0.0f, -2.0f, 1.0f, 0.0f, Ease::Linear),  // C
    };
    PathSpec spec = specOf(keys, 3, PlayMode::Once);
    spec.speed = speed;
    return spec;
}

} // namespace

TEST_CASE("motion: spherical conventions") {
    // az 0, el 0: straight ahead (-Z). az +90: right (+X). az -90: left.
    // el +90: straight up (+Y). All at d = 2.
    checkVec(fromSpherical({0.0f, 0.0f, 2.0f}), 0.0, 0.0, -2.0, 1e-6);
    checkVec(fromSpherical({90.0f, 0.0f, 2.0f}), 2.0, 0.0, 0.0, 1e-5);
    checkVec(fromSpherical({-90.0f, 0.0f, 2.0f}), -2.0, 0.0, 0.0, 1e-5);
    checkVec(fromSpherical({0.0f, 90.0f, 2.0f}), 0.0, 2.0, 0.0, 1e-5);
    checkVec(fromSpherical({180.0f, 0.0f, 2.0f}), 0.0, 0.0, 2.0, 1e-5);

    // The inverse on the axis points: behind must report +180, not -180.
    const Spherical front = toSpherical({0.0f, 0.0f, -2.0f});
    CHECK(near(front.azimuthDeg, 0.0, 1e-4));
    CHECK(near(front.elevationDeg, 0.0, 1e-4));
    CHECK(near(front.distance, 2.0, 1e-4));
    const Spherical right = toSpherical({2.0f, 0.0f, 0.0f});
    CHECK(near(right.azimuthDeg, 90.0, 1e-4));
    const Spherical back = toSpherical({0.0f, 0.0f, 2.0f});
    CHECK(near(back.azimuthDeg, 180.0, 1e-4));
    const Spherical up = toSpherical({0.0f, 2.0f, 0.0f});
    CHECK(near(up.elevationDeg, 90.0, 1e-4));
}

TEST_CASE("motion: spherical round-trip over a grid, and d = 0") {
    const float azimuths[] = {-180.0f, -135.0f, -90.0f, -45.0f, 0.0f,
                              45.0f,   90.0f,   135.0f, 180.0f};
    const float elevations[] = {-90.0f, -45.0f, 0.0f, 45.0f, 90.0f};
    const float distances[] = {0.5f, 2.0f, 10.0f};
    for (const float az : azimuths) {
        for (const float el : elevations) {
            for (const float d : distances) {
                const Spherical back = toSpherical(fromSpherical({az, el, d}));
                CHECK(near(back.distance, d, 1e-4));
                CHECK(near(back.elevationDeg, el, 1e-4));
                CHECK(near(azimuthDifference(back.azimuthDeg, az), 0.0, 1e-4));
            }
        }
    }
    // d = 0 -> az = el = 0, exactly.
    const Spherical zero = toSpherical({0.0f, 0.0f, 0.0f});
    CHECK(zero.azimuthDeg == 0.0f);
    CHECK(zero.elevationDeg == 0.0f);
    CHECK(zero.distance == 0.0f);
}

TEST_CASE("motion: circle keeps its radius and its period is exact") {
    OrbitParams params{};
    params.center = {1.0f, 2.0f, 3.0f};
    params.radius = 2.0f;
    params.periodS = 8.0f;
    params.tiltDeg = 30.0f; // tilt is a rotation: it must preserve the radius
    for (int i = 0; i < 200; ++i) {
        const double t = 0.041 * static_cast<double>(i);
        const Vec3 p = orbitPosition(params, t);
        const Vec3 rel{p.x - params.center.x, p.y - params.center.y, p.z - params.center.z};
        CHECK(near(vecLength(rel), 2.0, 1e-5));
    }
    params.tiltDeg = 0.0f;
    for (const double t : {0.0, 0.7, 2.3, 5.9}) {
        checkVecNear(orbitPosition(params, t + 8.0), orbitPosition(params, t), 1e-5);
        checkVecNear(orbitPosition(params, t + 16.0), orbitPosition(params, t), 1e-5);
    }
}

TEST_CASE("motion: clockwise circle order, direction and start angle") {
    OrbitParams params{}; // centre origin, radius 2, period 8, Clockwise
    // Clockwise as seen from above, front at top: front -> right -> back -> left.
    checkVec(orbitPosition(params, 0.0), 0.0, 0.0, -2.0, 1e-5); // front
    checkVec(orbitPosition(params, 2.0), 2.0, 0.0, 0.0, 1e-5);  // right (quarter period)
    checkVec(orbitPosition(params, 4.0), 0.0, 0.0, 2.0, 1e-5);  // back
    checkVec(orbitPosition(params, 6.0), -2.0, 0.0, 0.0, 1e-5); // left
    // Counter-clockwise goes the other way: quarter period is the left point.
    params.direction = Direction::CounterClockwise;
    checkVec(orbitPosition(params, 2.0), -2.0, 0.0, 0.0, 1e-5);
    checkVec(orbitPosition(params, 6.0), 2.0, 0.0, 0.0, 1e-5);
    // startAngleDeg is the angle at t = 0.
    params.direction = Direction::Clockwise;
    params.startAngleDeg = 90.0f;
    checkVec(orbitPosition(params, 0.0), 2.0, 0.0, 0.0, 1e-5);
    params.startAngleDeg = -90.0f;
    checkVec(orbitPosition(params, 0.0), -2.0, 0.0, 0.0, 1e-5);
    params.startAngleDeg = 180.0f;
    checkVec(orbitPosition(params, 0.0), 0.0, 0.0, 2.0, 1e-5);
}

TEST_CASE("motion: tilt 90 gives a vertical circle with the front point at +y") {
    OrbitParams params{};
    params.tiltDeg = 90.0f;
    // At t = 0 the base point is the front (0,0,-2); the tilt raises it to +y.
    checkVec(orbitPosition(params, 0.0), 0.0, 2.0, 0.0, 1e-5);
    // The whole circle lies in the XY plane (z stays 0) at radius 2.
    for (int i = 0; i < 100; ++i) {
        const Vec3 p = orbitPosition(params, 0.08 * static_cast<double>(i));
        CHECK(near(p.z, 0.0, 1e-5));
        CHECK(near(std::sqrt(static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y), 2.0,
                   1e-5));
    }
}

TEST_CASE("motion: figure-8 spans, centre crossings and closure") {
    OrbitParams params{};
    params.shape = OrbitShape::Figure8; // radius 2, period 8, Clockwise
    // phi = 0: right tip. phi = 180: left tip. z at phi = 45 deg:
    // -r * sin(45) * cos(45) = -2 * 0.5 = -1.
    checkVec(orbitPosition(params, 0.0), 2.0, 0.0, 0.0, 1e-5);
    checkVec(orbitPosition(params, 4.0), -2.0, 0.0, 0.0, 1e-5);
    checkVec(orbitPosition(params, 1.0), std::sqrt(2.0), 0.0, -1.0, 1e-5);
    // Crosses the centre at phi = 90 and phi = 270: twice per period.
    checkVec(orbitPosition(params, 2.0), 0.0, 0.0, 0.0, 1e-5);
    checkVec(orbitPosition(params, 6.0), 0.0, 0.0, 0.0, 1e-5);
    // Sweeps: |x| reaches but never exceeds r; |z| reaches but never
    // exceeds r/2; the point never leaves the radius-r disc.
    double maxAbsX = 0.0;
    double maxAbsZ = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p = orbitPosition(params, 8.0 * static_cast<double>(i) / 2000.0);
        maxAbsX = std::max(maxAbsX, std::abs(static_cast<double>(p.x)));
        maxAbsZ = std::max(maxAbsZ, std::abs(static_cast<double>(p.z)));
        CHECK(vecLength(p) <= 2.0 + 1e-5);
    }
    CHECK(maxAbsX <= 2.0 + 1e-5);
    CHECK(maxAbsX > 1.999);
    CHECK(maxAbsZ <= 1.0 + 1e-5);
    CHECK(maxAbsZ > 0.999);
    // Closed: p(t) = p(t + period).
    checkVecNear(orbitPosition(params, 0.3 + 8.0), orbitPosition(params, 0.3), 1e-5);
    checkVecNear(orbitPosition(params, 3.7 + 8.0), orbitPosition(params, 3.7), 1e-5);
}

TEST_CASE("motion: path linear timeline matches hand-computed positions") {
    PathEvaluator ev;
    ev.rebuild(handTimelineSpec(1.0f));
    CHECK(ev.duration() == doctest::Approx(4.5)); // 1 + 2 + 0.5 + 1
    // Hand computation on A = (0,0,0), B = (2,0,0), C = (2,0,-2) with the
    // phases in handTimelineSpec's comment:
    //   t = 0.5: inside hold A                     -> A = (0,0,0)
    //   t = 1.0: travel B starts (u = 0)           -> A = (0,0,0)
    //   t = 2.0: travel B halfway (u = 0.5)        -> A + 0.5*(B-A) = (1,0,0)
    //   t = 3.25: inside hold B                     -> B = (2,0,0)
    //   t = 4.0: travel C halfway (u = 0.5)        -> B + 0.5*(C-B) = (2,0,-1)
    //   t = 4.5: end of the pass                   -> C = (2,0,-2)
    checkVec(ev.positionAt(0.5), 0.0, 0.0, 0.0, 1e-6);
    checkVec(ev.positionAt(1.0), 0.0, 0.0, 0.0, 1e-6);
    checkVec(ev.positionAt(2.0), 1.0, 0.0, 0.0, 1e-6);
    checkVec(ev.positionAt(3.25), 2.0, 0.0, 0.0, 1e-6);
    checkVec(ev.positionAt(4.0), 2.0, 0.0, -1.0, 1e-6);
    checkVec(ev.positionAt(4.5), 2.0, 0.0, -2.0, 1e-6);
    // segmentAt reports the keyframe being held or travelled to.
    CHECK(ev.segmentAt(0.5) == 0);
    CHECK(ev.segmentAt(2.0) == 1);
    CHECK(ev.segmentAt(3.25) == 1);
    CHECK(ev.segmentAt(4.0) == 2);
    CHECK(ev.segmentAt(4.5) == 2);
}

TEST_CASE("motion: step ease holds the previous position, then jumps") {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Step),
        key(4.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Step),
    };
    PathEvaluator ev;
    ev.rebuild(specOf(keys, 2, PlayMode::Once));
    CHECK(ev.duration() == doctest::Approx(2.0));
    checkVec(ev.positionAt(0.0), 0.0, 0.0, 0.0, 0.0);
    checkVec(ev.positionAt(1.0), 0.0, 0.0, 0.0, 0.0);   // still holding A mid-travel
    checkVec(ev.positionAt(1.999), 0.0, 0.0, 0.0, 0.0); // ... right up to the end
    checkVec(ev.positionAt(2.0), 4.0, 0.0, 0.0, 0.0);   // then it jumps
}

TEST_CASE("motion: smooth passes through every keyframe exactly at arrival") {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, Ease::Smooth),
        key(1.0f, 0.0f, -1.0f, 1.0f, 0.25f, Ease::Smooth),
        key(2.0f, 0.0f, 0.0f, 1.0f, 0.25f, Ease::Smooth),
        key(3.0f, 0.0f, -1.0f, 1.0f, 0.25f, Ease::Smooth),
    };
    PathEvaluator ev;
    ev.rebuild(specOf(keys, 4, PlayMode::Once));
    // Arrivals: 0, 0.25+1 = 1.25, 1.25+0.25+1 = 2.5, 2.5+0.25+1 = 3.75.
    CHECK(ev.duration() == doctest::Approx(4.0));
    checkVecExact(ev.positionAt(0.0), keys[0].position);
    checkVecExact(ev.positionAt(1.25), keys[1].position);
    checkVecExact(ev.positionAt(2.5), keys[2].position);
    checkVecExact(ev.positionAt(3.75), keys[3].position);
    // With zero holds the arrival instant is the next segment's u = 0, which
    // is still the keyframe itself.
    const Keyframe noHold[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Smooth),
        key(1.0f, 0.0f, -1.0f, 1.0f, 0.0f, Ease::Smooth),
        key(2.0f, 0.0f, 0.0f, 1.0f, 0.0f, Ease::Smooth),
    };
    PathEvaluator ev2;
    ev2.rebuild(specOf(noHold, 3, PlayMode::Once));
    checkVec(ev2.positionAt(1.0), 1.0, 0.0, -1.0, 1e-6);
}

TEST_CASE("motion: smooth is C1 across keyframes (finite differences)") {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, Ease::Smooth),
        key(1.0f, 0.0f, -1.0f, 1.0f, 0.25f, Ease::Smooth),
        key(2.0f, 0.0f, 0.0f, 1.0f, 0.25f, Ease::Smooth),
        key(3.0f, 0.0f, -1.0f, 1.0f, 0.25f, Ease::Smooth),
    };
    PathEvaluator ev;
    ev.rebuild(specOf(keys, 4, PlayMode::Once));
    // Evenly spaced keys (1 s travels, |step| = sqrt(2) m): one-sided
    // velocities either side of an arrival must agree to < 1 %.
    const double h = 1e-3;
    const double avgSpeed = std::sqrt(2.0); // |P(i) - P(i-1)| / 1 s
    for (const double arrival : {1.25, 2.5}) {
        const Vec3 at = ev.positionAt(arrival);
        const Vec3 before = ev.positionAt(arrival - h);
        const Vec3 after = ev.positionAt(arrival + h);
        const double vMinus[3] = {(static_cast<double>(at.x) - before.x) / h,
                                  (static_cast<double>(at.y) - before.y) / h,
                                  (static_cast<double>(at.z) - before.z) / h};
        const double vPlus[3] = {(static_cast<double>(after.x) - at.x) / h,
                                 (static_cast<double>(after.y) - at.y) / h,
                                 (static_cast<double>(after.z) - at.z) / h};
        double jumpSq = 0.0;
        double minusSq = 0.0;
        double plusSq = 0.0;
        for (int c = 0; c < 3; ++c) {
            const double d = vPlus[c] - vMinus[c];
            jumpSq += d * d;
            minusSq += vMinus[c] * vMinus[c];
            plusSq += vPlus[c] * vPlus[c];
        }
        const double scale = std::max(avgSpeed, std::max(std::sqrt(minusSq), std::sqrt(plusSq)));
        CHECK(std::sqrt(jumpSq) < 0.01 * scale);
    }
}

TEST_CASE("motion: smooth with coincident points falls back to linear, no NaN") {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Smooth),
        key(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, Ease::Smooth),
        key(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, Ease::Smooth), // coincides with the previous key
        key(2.0f, 0.0f, 0.0f, 1.0f, 0.0f, Ease::Smooth),
    };
    PathEvaluator ev;
    ev.rebuild(specOf(keys, 4, PlayMode::Once));
    CHECK(ev.duration() == doctest::Approx(3.0));
    for (int i = 0; i <= 300; ++i) {
        const Vec3 p = ev.positionAt(3.0 * static_cast<double>(i) / 300.0);
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.y));
        CHECK(std::isfinite(p.z));
    }
    // Segment into the duplicated key is a constant (1,0,0); the segment out
    // of it is Linear, so its midpoint is the plain midpoint (1.5, 0, 0).
    checkVec(ev.positionAt(1.5), 1.0, 0.0, 0.0, 1e-6);
    checkVec(ev.positionAt(2.5), 1.5, 0.0, 0.0, 1e-6);
    // Quarter points pin the fallback to Linear rather than merely "no NaN":
    // at u = 0.25 / 0.75 the Smooth warp and the guarded Catmull-Rom both
    // deviate from the chord, so only a true Linear segment passes through
    // the plain quarter positions. (The reviewer's probe of this fixture
    // returned exactly these values.)
    checkVec(ev.positionAt(0.25), 0.25, 0.0, 0.0, 1e-6); // into key 1, before the duplicate
    checkVec(ev.positionAt(0.75), 0.75, 0.0, 0.0, 1e-6);
    checkVec(ev.positionAt(2.25), 1.25, 0.0, 0.0, 1e-6); // out of the duplicated key
    checkVec(ev.positionAt(2.75), 1.75, 0.0, 0.0, 1e-6);
}

TEST_CASE("motion: once clamps, negative t is treated as 0") {
    PathEvaluator ev;
    ev.rebuild(handTimelineSpec(1.0f));
    checkVecExact(ev.positionAt(-3.0), ev.positionAt(0.0));
    checkVec(ev.positionAt(-3.0), 0.0, 0.0, 0.0, 0.0);
    checkVec(ev.positionAt(100.0), 2.0, 0.0, -2.0, 0.0); // clamped on the last keyframe
    CHECK(ev.segmentAt(-1.0) == 0);
}

TEST_CASE("motion: loop with closeLoop is continuous at the wrap") {
    const Keyframe keys[] = {
        key(-2.0f, 0.0f, 0.0f, 1.5f, 0.0f, Ease::Linear), // travelTimeS = closing segment's
        key(2.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Linear),
    };
    PathSpec spec = specOf(keys, 2, PlayMode::Loop);
    spec.closeLoop = true;
    PathEvaluator ev;
    ev.rebuild(spec);
    // Duration = travel to key 1 (2 s) + closing segment (keys[0].travelTimeS).
    CHECK(ev.duration() == doctest::Approx(3.5));
    const double eps = 1e-5;
    CHECK(vecDistance(ev.positionAt(3.5 - eps), ev.positionAt(3.5 + eps)) < 1e-4);
    checkVecExact(ev.positionAt(3.5), ev.positionAt(0.0)); // wraps exactly onto key 0
    CHECK(ev.segmentAt(3.4) == 0);                          // closing segment targets key 0
    CHECK(ev.segmentAt(1.0) == 1);
    // Without closeLoop the pass is just the forward timeline (2 s) and the
    // position jumps back at the wrap.
    spec.closeLoop = false;
    PathEvaluator ev2;
    ev2.rebuild(spec);
    CHECK(ev2.duration() == doctest::Approx(2.0));
    checkVecExact(ev2.positionAt(2.5), ev2.positionAt(0.5));
}

TEST_CASE("motion: pingpong mirrors around the duration") {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.5f, Ease::Linear),
        key(3.0f, 0.0f, 0.0f, 1.5f, 0.25f, Ease::Linear),
        key(3.0f, 0.0f, -3.0f, 1.0f, 0.0f, Ease::Linear),
    };
    PathEvaluator ev;
    ev.rebuild(specOf(keys, 3, PlayMode::PingPong));
    const double d = 0.5 + 1.5 + 0.25 + 1.0; // 3.25 s
    CHECK(ev.duration() == doctest::Approx(d));
    for (const double x : {0.1, 0.7, 1.3}) {
        checkVecNear(ev.positionAt(d + x), ev.positionAt(d - x), 1e-6);
    }
    checkVecExact(ev.positionAt(2.0 * d), ev.positionAt(0.0)); // full ping-pong cycle
}

TEST_CASE("motion: speed 2 halves the times") {
    PathEvaluator ev1;
    ev1.rebuild(handTimelineSpec(1.0f));
    PathEvaluator ev2;
    ev2.rebuild(handTimelineSpec(2.0f));
    CHECK(ev2.duration() == doctest::Approx(4.5)); // duration() is pre-speed path time
    for (const double t : {0.25, 0.8, 1.6, 2.2}) {
        checkVecExact(ev2.positionAt(t), ev1.positionAt(2.0 * t));
    }
}

TEST_CASE("motion: count 0 is the origin, count 1 is that keyframe") {
    PathEvaluator empty; // never rebuilt
    checkVec(empty.positionAt(5.0), 0.0, 0.0, 0.0, 0.0);
    CHECK(empty.duration() == 0.0);
    CHECK(empty.segmentAt(5.0) == 0);

    PathSpec zero{};
    zero.count = 0;
    PathEvaluator ev0;
    ev0.rebuild(zero);
    checkVec(ev0.positionAt(-1.0), 0.0, 0.0, 0.0, 0.0);
    checkVec(ev0.positionAt(9.0), 0.0, 0.0, 0.0, 0.0);

    for (const PlayMode mode : {PlayMode::Once, PlayMode::Loop, PlayMode::PingPong}) {
        const Keyframe keys[] = {key(5.0f, -2.0f, 7.0f, 2.0f, 0.5f, Ease::Smooth)};
        PathEvaluator ev1;
        ev1.rebuild(specOf(keys, 1, mode));
        checkVec(ev1.positionAt(-10.0), 5.0, -2.0, 7.0, 0.0);
        checkVec(ev1.positionAt(0.0), 5.0, -2.0, 7.0, 0.0);
        checkVec(ev1.positionAt(123.0), 5.0, -2.0, 7.0, 0.0);
        CHECK(ev1.segmentAt(3.0) == 0);
    }
}

TEST_CASE("motion: a non-finite clock is treated as t = 0") {
    // Maintainer nit on PR #28: std::fmod(inf, duration) is NaN, and the
    // negative/NaN check in mapTime does not catch +inf — without the
    // isfinite guard the NaN would leak into positionAt on Loop/PingPong.
    const Keyframe keys[] = {
        key(0.0f, 0.0f, 0.0f, 2.0f, 0.5f, Ease::Linear),
        key(3.0f, 0.0f, 0.0f, 1.5f, 0.25f, Ease::Linear),
        key(3.0f, 0.0f, -3.0f, 1.0f, 0.0f, Ease::Linear),
    };
    const double inf = std::numeric_limits<double>::infinity();
    for (const PlayMode mode : {PlayMode::Once, PlayMode::Loop, PlayMode::PingPong}) {
        PathEvaluator ev;
        ev.rebuild(specOf(keys, 3, mode));
        checkVec(ev.positionAt(inf), 0.0, 0.0, 0.0, 0.0);
        CHECK(ev.segmentAt(inf) == 0);
        // NaN clock was already guarded by the !(t >= 0.0) check; pin it too.
        checkVec(ev.positionAt(std::numeric_limits<double>::quiet_NaN()), 0.0, 0.0, 0.0, 0.0);
        CHECK(ev.segmentAt(std::numeric_limits<double>::quiet_NaN()) == 0);
    }
}

TEST_CASE("motion: spatializer path timing scenario") {
    // spatializer.md acceptance row "path timing": a path from az -90 to
    // az +90 at 2 m, Linear, travel 2 s. Positions: (-2,0,~0) -> (2,0,~0),
    // so x(t) = -2 + 4 * (t / 2): x(0.25) = -1.5 and x(1.75) = +1.5.
    PathSpec spec{};
    spec.count = 2;
    spec.mode = PlayMode::Once;
    spec.keys[0] = key(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Linear);
    spec.keys[0].position = fromSpherical({-90.0f, 0.0f, 2.0f});
    spec.keys[1] = key(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, Ease::Linear);
    spec.keys[1].position = fromSpherical({90.0f, 0.0f, 2.0f});
    PathEvaluator ev;
    ev.rebuild(spec);
    const Vec3 early = ev.positionAt(0.25);
    const Vec3 late = ev.positionAt(1.75);
    CHECK(early.x < 0.0f);
    CHECK(late.x > 0.0f);
    CHECK(near(early.x, -1.5, 1e-4));
    CHECK(near(late.x, 1.5, 1e-4));
}

TEST_CASE("motion: determinism - same spec, bit-identical evaluators") {
    const Ease eases[] = {Ease::Step, Ease::Linear, Ease::Smooth};
    const PlayMode modes[] = {PlayMode::Once, PlayMode::Loop, PlayMode::PingPong};
    for (const Ease ease : eases) {
        for (const PlayMode mode : modes) {
            const Keyframe keys[] = {
                key(0.0f, 0.0f, -2.0f, 1.5f, 0.3f, ease),
                key(2.0f, 0.5f, 0.0f, 1.1f, 0.0f, ease),
                key(-1.0f, 1.0f, 1.0f, 0.7f, 0.2f, ease),
                key(0.5f, -0.5f, -1.0f, 1.9f, 0.0f, ease),
            };
            PathSpec spec = specOf(keys, 4, mode);
            spec.speed = 1.5f;
            PathEvaluator a;
            PathEvaluator b;
            a.rebuild(spec);
            b.rebuild(spec);
            CHECK(a.duration() == b.duration());
            for (int i = 0; i < 350; ++i) {
                const double t = -1.0 + 0.0371 * static_cast<double>(i);
                checkVecExact(a.positionAt(t), b.positionAt(t));
                CHECK(a.segmentAt(t) == b.segmentAt(t));
            }
        }
    }
}

TEST_CASE("motion: rebuild, positionAt and orbitPosition do not allocate") {
    const Keyframe keys[] = {
        key(0.0f, 0.0f, -2.0f, 1.5f, 0.3f, Ease::Smooth),
        key(2.0f, 0.5f, 0.0f, 1.1f, 0.0f, Ease::Smooth),
        key(-1.0f, 1.0f, 1.0f, 0.7f, 0.2f, Ease::Smooth),
    };
    const PathSpec spec = specOf(keys, 3, PlayMode::Loop);
    OrbitParams orbit{};
    orbit.shape = OrbitShape::Figure8;
    PathEvaluator ev;
    Vec3 sink{0.0f, 0.0f, 0.0f};
    std::size_t allocs = 0;
    {
        dsptb::test::AllocGuard guard;
        ev.rebuild(spec);
        for (int i = 0; i < 100; ++i) {
            sink = ev.positionAt(0.05 * static_cast<double>(i));
            sink = orbitPosition(orbit, 0.11 * static_cast<double>(i));
        }
        allocs = guard.count();
    }
    CHECK(allocs == 0);
    CHECK(std::isfinite(sink.x));
}

TEST_CASE("motion: positionAt micro-benchmark (report only)") {
    // Spec S6 budget: <= 0.3 us per positionAt call, measured on the CI
    // runner. Like the loudness benchmark this prints the number and only
    // asserts a loose sanity bound, so a slow machine cannot flake CI.
    const Keyframe keys[] = {
        key(0.0f, 0.0f, -2.0f, 1.5f, 0.3f, Ease::Smooth),
        key(2.0f, 0.5f, 0.0f, 1.1f, 0.0f, Ease::Smooth),
        key(-1.0f, 1.0f, 1.0f, 0.7f, 0.2f, Ease::Smooth),
        key(0.5f, -0.5f, -1.0f, 1.9f, 0.0f, Ease::Smooth),
        key(1.5f, 0.0f, -2.5f, 0.9f, 0.1f, Ease::Smooth),
        key(-2.0f, -1.0f, 0.5f, 1.3f, 0.0f, Ease::Smooth),
        key(0.0f, 2.0f, 0.0f, 0.5f, 0.4f, Ease::Smooth),
        key(1.0f, 1.0f, 1.0f, 1.7f, 0.0f, Ease::Smooth),
    };
    PathSpec spec = specOf(keys, 8, PlayMode::Loop);
    PathEvaluator ev;
    ev.rebuild(spec);
    constexpr int kIterations = 20000;
    double sink = 0.0;
    const auto runOnce = [&]() {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            const Vec3 p = ev.positionAt(0.0013 * static_cast<double>(i));
            sink += static_cast<double>(p.x);
        }
        const auto stop = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(stop - start).count();
    };
    (void)runOnce(); // warm-up
    double bestUs = 1e30;
    for (int rep = 0; rep < 7; ++rep)
        bestUs = std::min(bestUs, runOnce());
    const double usPerCall = bestUs / static_cast<double>(kIterations);
    std::printf("[bench] PathEvaluator::positionAt (8-key Smooth loop): %.4f us/call "
                "(budget <= 0.3 us, spec S6)\n",
                usPerCall);
    CHECK(std::isfinite(sink));
    CHECK(usPerCall < 2.0); // sanity bound only; the budget number is the printout
}
