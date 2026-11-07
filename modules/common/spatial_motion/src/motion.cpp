// Motion maths for the 3D Spatializer.
// All maths is done in double internally and rounded to float once at the
// API boundary, so the published formulas hold to float precision.

#include <dsptb/spatial_motion/motion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dsptb::spatial {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

// Spec §5 (Smooth): if neighbouring points coincide (distance < 1e-6), the
// segment falls back to Linear. This also keeps every centripetal knot
// interval strictly positive.
constexpr double kCoincidentDistance = 1e-6;

struct DVec3 {
    double x, y, z;
};

DVec3 toD(const Vec3& v) noexcept {
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

Vec3 toF(const DVec3& v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

DVec3 lerpD(const DVec3& a, const DVec3& b, double u) noexcept {
    return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
}

double distanceD(const DVec3& a, const DVec3& b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Negative travel / hold times act as zero-length phases (a travel time of
// <= 0 is an instant jump).
double phaseSeconds(float seconds) noexcept {
    return seconds > 0.0f ? static_cast<double>(seconds) : 0.0;
}

// Centripetal Catmull-Rom through p0..p3, evaluated at u in [0, 1] on the
// segment p1 -> p2, using the Barry-Goldman pyramid form of Yuksel,
// Schaefer & Keyser, "Parameterization and applications of Catmull-Rom
// curves", Computer-Aided Design 43(7), 2011: knots are spaced by
// |P(i+1) - P(i)|^alpha with alpha = 0.5 (centripetal), which guarantees no
// cusps or self-intersections inside the segment. The caller guarantees the
// segment chord |p2 - p1| is >= kCoincidentDistance, so the middle knot
// interval is never 0. A boundary interval can still be 0 — that is the
// spec's duplicated endpoint (p0 == p1 or p2 == p3 at a path end) — and the
// blend over a zero-width interval is simply the shared point.
DVec3 catmullRom(const DVec3& p0, const DVec3& p1, const DVec3& p2, const DVec3& p3, double u) noexcept {
    const double t0 = 0.0;
    const double t1 = t0 + std::sqrt(distanceD(p0, p1));
    const double t2 = t1 + std::sqrt(distanceD(p1, p2));
    const double t3 = t2 + std::sqrt(distanceD(p2, p3));
    const double t = t1 + u * (t2 - t1);
    const auto blend = [](const DVec3& a, const DVec3& b, double ta, double tb, double tt) noexcept {
        if (tb <= ta)
            return a; // zero-width knot interval: a and b are the same point
        return lerpD(a, b, (tt - ta) / (tb - ta));
    };
    const DVec3 a1 = blend(p0, p1, t0, t1, t);
    const DVec3 a2 = blend(p1, p2, t1, t2, t);
    const DVec3 a3 = blend(p2, p3, t2, t3, t);
    const DVec3 b1 = blend(a1, a2, t0, t2, t);
    const DVec3 b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

// Position at segment parameter u in [0, 1] while travelling from p1 to p2;
// p0 and p3 are the neighbouring points used by the Smooth curve.
// smoothFallback is the spec §5 rule "if neighbouring points coincide
// (distance < 1e-6), fall back to Linear", as decided by the caller: it
// covers coincident *keyframe* points. The duplicated endpoint the spec
// substitutes at a path end (p0 == p1 by construction, not a keyframe
// coincidence) does not trigger it — the curve evaluation handles that case
// directly, and treating it as a coincidence would make the first and last
// segments of every open path Linear, breaking the C1 behaviour the spec
// requires across keyframes.
Vec3 evalTravel(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, Ease ease, double u,
                bool smoothFallback) noexcept {
    if (ease == Ease::Step)
        return p1; // hold the previous position for the whole travel
    const DVec3 d1 = toD(p1);
    const DVec3 d2 = toD(p2);
    if (ease == Ease::Linear || smoothFallback)
        return toF(lerpD(d1, d2, u));
    // Ease-in-out time warp: u' = u^2 * (3 - 2u). Its slope is 0 at
    // both ends, so a Smooth segment arrives and departs with zero velocity.
    const double warped = u * u * (3.0 - 2.0 * u);
    return toF(catmullRom(toD(p0), d1, d2, toD(p3), warped));
}

// Travel segment arriving at keyframe `target` (>= 1). Neighbouring points
// for the Smooth curve wrap around when the loop is closed and are duplicated
// at the ends otherwise.
Vec3 evalSegment(const PathSpec& spec, std::size_t count, bool wrapped, std::size_t target,
                 double u) noexcept {
    const Keyframe& to = spec.keys[target];
    const Vec3& from = spec.keys[target - 1].position;
    Vec3 p0{};
    Vec3 p3{};
    if (wrapped) {
        p0 = spec.keys[(target + count - 2) % count].position;
        p3 = spec.keys[(target + 1) % count].position;
    } else {
        p0 = spec.keys[target >= 2 ? target - 2 : 0].position;
        p3 = spec.keys[target + 1 < count ? target + 1 : count - 1].position;
    }
    // Coincident-keyframe check for the Linear fallback: the segment chord,
    // plus each neighbour pair that names two distinct keyframes (in a
    // wrapped loop every pair does; at an open path end the clamped index
    // repeats the end keyframe, which is the duplicated-endpoint case, not
    // a coincidence).
    const DVec3 dFrom = toD(from);
    const DVec3 dTo = toD(to.position);
    bool fallback = distanceD(dFrom, dTo) < kCoincidentDistance;
    if (!fallback && (wrapped || target >= 2))
        fallback = distanceD(toD(p0), dFrom) < kCoincidentDistance;
    if (!fallback && (wrapped || target + 1 < count))
        fallback = distanceD(dTo, toD(p3)) < kCoincidentDistance;
    return evalTravel(p0, from, to.position, p3, to.ease, u, fallback);
}

// The closing segment of a closed loop: last keyframe back to keyframe 0,
// eased by keys[0].ease, with wrapped neighbours (every neighbour pair names
// two distinct keyframes here, so all three pairs take the fallback check).
Vec3 evalClosing(const PathSpec& spec, std::size_t count, double u) noexcept {
    const Vec3& p0 = spec.keys[count - 2].position;
    const Vec3& from = spec.keys[count - 1].position;
    const Vec3& to = spec.keys[0].position;
    const Vec3& p3 = spec.keys[1].position;
    const bool fallback = distanceD(toD(p0), toD(from)) < kCoincidentDistance ||
                          distanceD(toD(from), toD(to)) < kCoincidentDistance ||
                          distanceD(toD(to), toD(p3)) < kCoincidentDistance;
    return evalTravel(p0, from, to, p3, spec.keys[0].ease, u, fallback);
}

} // namespace

Vec3 fromSpherical(const Spherical& spherical) noexcept {
    const double az = static_cast<double>(spherical.azimuthDeg) * kDegToRad;
    const double el = static_cast<double>(spherical.elevationDeg) * kDegToRad;
    const double d = static_cast<double>(spherical.distance);
    const double cosEl = std::cos(el);
    return {static_cast<float>(d * cosEl * std::sin(az)), static_cast<float>(d * std::sin(el)),
            static_cast<float>(-d * cosEl * std::cos(az))};
}

Spherical toSpherical(const Vec3& position) noexcept {
    const double x = static_cast<double>(position.x);
    const double y = static_cast<double>(position.y);
    const double z = static_cast<double>(position.z);
    const double d = std::sqrt(x * x + y * y + z * z);
    if (d == 0.0)
        return {0.0f, 0.0f, 0.0f}; // distance 0 -> az = el = 0
    double sinEl = y / d;
    if (sinEl > 1.0)
        sinEl = 1.0;
    if (sinEl < -1.0)
        sinEl = -1.0;
    const double el = std::asin(sinEl) * kRadToDeg;
    double az = std::atan2(x, -z) * kRadToDeg;
    if (az <= -180.0)
        az += 360.0; // report azimuth in (-180, 180]
    return {static_cast<float>(az), static_cast<float>(el), static_cast<float>(d)};
}

Vec3 orbitPosition(const OrbitParams& params, double tSeconds) noexcept {
    double angleDeg = static_cast<double>(params.startAngleDeg);
    const double period = static_cast<double>(params.periodS);
    if (period > 0.0) {
        const double sweepDeg = 360.0 * tSeconds / period;
        angleDeg += params.direction == Direction::Clockwise ? sweepDeg : -sweepDeg;
    }
    // Reduce in degrees before converting: keeps the trig accurate (and the
    // period exact) for large t.
    const double angle = std::fmod(angleDeg, 360.0) * kDegToRad;
    const double r = static_cast<double>(params.radius);
    double baseX = 0.0;
    double baseZ = 0.0;
    if (params.shape == OrbitShape::Circle) {
        // (r sin, 0, -r cos): angle 0 is in front, 90 deg is to the right.
        baseX = r * std::sin(angle);
        baseZ = -r * std::cos(angle);
    } else {
        // Lemniscate of Gerono: half-width r, half-depth r/2, lobes left and
        // right, crossing the centre at 90 and 270 degrees.
        baseX = r * std::cos(angle);
        baseZ = -r * std::sin(angle) * std::cos(angle);
    }
    // Tilt about the X axis: y' = -z sin(tilt), z' = z cos(tilt)
    // for a base point with y = 0; positive tilt raises the front.
    const double tilt = static_cast<double>(params.tiltDeg) * kDegToRad;
    const double y = -baseZ * std::sin(tilt);
    const double z = baseZ * std::cos(tilt);
    return {static_cast<float>(baseX + static_cast<double>(params.center.x)),
            static_cast<float>(y + static_cast<double>(params.center.y)),
            static_cast<float>(z + static_cast<double>(params.center.z))};
}

void PathEvaluator::rebuild(const PathSpec& spec) noexcept {
    spec_ = spec;
    const std::size_t count =
        std::min(static_cast<std::size_t>(spec.count), kMaxKeyframes);
    count_ = static_cast<std::uint32_t>(count);
    double t = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        arrival_[i] = t;
        t += phaseSeconds(spec_.keys[i].holdS);
        if (i + 1 < count)
            t += phaseSeconds(spec_.keys[i + 1].travelTimeS);
    }
    // The closing segment exists only in Loop mode: Once
    // ends holding the last keyframe and PingPong reverses from it.
    closing_ = spec_.mode == PlayMode::Loop && spec_.closeLoop && count >= 2;
    duration_ = t + (closing_ ? phaseSeconds(spec_.keys[0].travelTimeS) : 0.0);
}

double PathEvaluator::mapTime(double tSeconds) const noexcept {
    if (count_ == 0 || duration_ <= 0.0)
        return 0.0;
    double t = tSeconds < 0.0 ? 0.0 : tSeconds; // negative t is treated as 0
    t *= static_cast<double>(spec_.speed);      // speed is applied inside
    // A non-finite clock (NaN, or +inf from an unbounded clock) must stay
    // at the start: std::fmod(inf, d) is NaN, which would leak into
    // positionAt, and the !(t >= 0.0) check alone does not catch +inf.
    if (!(t >= 0.0) || !std::isfinite(t))
        t = 0.0; // negative speed, a NaN clock, or an infinite clock
    switch (spec_.mode) {
    case PlayMode::Once:
        return t < duration_ ? t : duration_;
    case PlayMode::Loop:
        return std::fmod(t, duration_);
    case PlayMode::PingPong: {
        const double period = 2.0 * duration_;
        const double wrapped = std::fmod(t, period);
        return wrapped <= duration_ ? wrapped : period - wrapped;
    }
    }
    return 0.0;
}

PathEvaluator::Located PathEvaluator::locate(double pathTime) const noexcept {
    const std::size_t count = static_cast<std::size_t>(count_);
    for (std::size_t i = 0; i < count; ++i) {
        const Keyframe& key = spec_.keys[i];
        const double holdEnd = arrival_[i] + phaseSeconds(key.holdS);
        if (pathTime < holdEnd)
            return {key.position, static_cast<std::uint32_t>(i)};
        if (i + 1 < count) {
            if (pathTime < arrival_[i + 1]) {
                const double travel = phaseSeconds(spec_.keys[i + 1].travelTimeS);
                const double u = travel > 0.0 ? (pathTime - holdEnd) / travel : 0.0;
                return {evalSegment(spec_, count, closing_, i + 1, u),
                        static_cast<std::uint32_t>(i + 1)};
            }
        } else if (closing_ && pathTime < duration_) {
            const double travel = phaseSeconds(spec_.keys[0].travelTimeS);
            const double u = travel > 0.0 ? (pathTime - holdEnd) / travel : 0.0;
            return {evalClosing(spec_, count, u), 0};
        }
    }
    // pathTime == duration (Once / PingPong endpoint, or an all-zero
    // timeline): the pass ends on the last keyframe.
    return {spec_.keys[count - 1].position, static_cast<std::uint32_t>(count - 1)};
}

Vec3 PathEvaluator::positionAt(double tSeconds) const noexcept {
    if (count_ == 0)
        return {0.0f, 0.0f, 0.0f};
    if (count_ == 1)
        return spec_.keys[0].position;
    return locate(mapTime(tSeconds)).position;
}

std::uint32_t PathEvaluator::segmentAt(double tSeconds) const noexcept {
    if (count_ <= 1)
        return 0;
    return locate(mapTime(tSeconds)).segment;
}

} // namespace dsptb::spatial
