#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dsptb::spat {

namespace {

float clampf(float v, float lo, float hi) {
    return std::min(std::max(v, lo), hi);
}

uint32_t withAlpha(uint32_t colour, uint32_t a) {
    return (colour & 0x00FFFFFFu) | (a << 24);
}

const char* motionLabel(Motion m) {
    return m == Motion::Orbit ? "Orbit" : m == Motion::Path ? "Path" : "Static";
}

float snapTo(float v, float step) {
    return std::round(v / step) * step;
}

// Both views share one scale, logarithmic in the distance from the head, so that the region next to the
// listener (where Near sources live) gets most of the pixels: the head is drawn `headPx` big and stands for
// kHeadM (an average head radius); inside it the scale is linear, outside it grows as log(d / kHeadM) until
// `radius` pixels reach `rangeM`.
constexpr float kHeadM = 0.09f;
struct Scale {
    float cx, cy, radius, rangeM, headPx;
    float rOf(float d) const { // metres -> pixels from the head's centre
        d = std::max(d, 0.0f);
        if (d <= kHeadM)
            return headPx * d / kHeadM;
        return std::min(radius, headPx + (radius - headPx) * std::log(d / kHeadM) / std::log(rangeM / kHeadM));
    }
    float dOf(float r) const { // the inverse
        r = std::max(r, 0.0f);
        if (r <= headPx)
            return kHeadM * r / headPx;
        return kHeadM * std::exp(std::min(1.0f, (r - headPx) / (radius - headPx)) * std::log(rangeM / kHeadM));
    }
};

float sgn(float v) {
    return v < 0.0f ? -1.0f : 1.0f;
}

// Top-down: front (-Z) at the top, right (+X) to the right.
dsptb_vec2 topScreen(const Scale& sc, const sm::Vec3& p) {
    const float h = std::sqrt(p.x * p.x + p.z * p.z);
    if (h < 1e-6f)
        return {sc.cx, sc.cy};
    const float r = sc.rOf(h);
    return {sc.cx + p.x / h * r, sc.cy + p.z / h * r};
}

// Side: the head looks to the right (front = -Z drawn to the right), up is +Y.
dsptb_vec2 sideScreen(const Scale& sc, const sm::Vec3& p, float halfHeightPx) {
    return {sc.cx - sgn(p.z) * sc.rOf(std::fabs(p.z)),
            sc.cy - std::min(std::max(sgn(p.y) * sc.rOf(std::fabs(p.y)), -halfHeightPx), halfHeightPx)};
}

// Which of the rings get a label: from the outside in, each one that has room after the previous label.
std::vector<bool> ringLabels(const Scale& sc, const std::vector<float>& rings, float minGapPx) {
    std::vector<bool> on(rings.size(), false);
    float last = 1e9f;
    for (size_t i = rings.size(); i-- > 0;) {
        const float r = sc.rOf(rings[i]);
        if (r >= sc.headPx * 1.4f && last - r >= minGapPx) {
            on[i] = true;
            last = r;
        }
    }
    return on;
}

// The ring distances: the 1-2-5 values up to the range, the last five.
std::vector<float> ringDistances(float rangeM) {
    std::vector<float> all;
    for (float decade = 0.01f; decade <= 100.0f; decade *= 10.0f)
        for (float m : {1.0f, 2.0f, 5.0f})
            if (decade * m <= rangeM * 1.001f)
                all.push_back(decade * m);
    if (all.size() > 5)
        all.erase(all.begin(), all.end() - 5);
    return all;
}

float elevationSin(const sm::Vec3& p) {
    const float d = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return d > 1e-5f ? clampf(p.y / d, -1.0f, 1.0f) : 0.0f;
}

void drawHead(Canvas& c, float cx, float cy, float s) {
    const uint32_t col = dsptb::rgba(220, 224, 235);
    c.circle({cx, cy}, 8.0f * s, col, 1.5f);
    const dsptb_vec2 nose[3] = {
        {cx - 4.0f * s, cy - 7.0f * s}, {cx, cy - 14.0f * s}, {cx + 4.0f * s, cy - 7.0f * s}};
    c.polyline(nose, 3, col, 1.5f, true);
    c.fillRect({cx - 11.0f * s, cy - 3.0f * s}, {cx - 8.0f * s, cy + 3.0f * s}, col);
    c.fillRect({cx + 8.0f * s, cy - 3.0f * s}, {cx + 11.0f * s, cy + 3.0f * s}, col);
}

} // namespace

// Static positions, keyframes and edit cursors are held inside the HRTF set's elevation band (a scene saved
// with another set, or a set picked later, can have them outside). Orbits are bounded on the audio thread.
void SpatializerUi::refitElevation() {
    const ElevationBand band = host_->elevationBand();
    bool any = false;
    const auto fit = [&](sm::Vec3& v) {
        const sm::Vec3 n = limitElevation(v, band);
        if (n.x != v.x || n.y != v.y || n.z != v.z) {
            v = n;
            any = true;
        }
    };
    for (Source& src : scene_->sources) {
        fit(src.position);
        for (uint32_t i = 0; i < src.path.spec.count; ++i)
            fit(src.path.spec.keys[i].position);
    }
    if (any) {
        cacheValid_ = false;
        dirtyLabel_ = "Fit to HRTF set";
        edited();
        changed();
    }
}

// The head is drawn big in the Near range (where the sources are within a few head sizes of it) and small in
// the others.
float SpatializerUi::headPx(float radiusPx, float uiScale) const {
    return scene_->range == Range::Near ? 0.2f * radiusPx : 8.0f * uiScale;
}

float SpatializerUi::maxDistance() const {
    return rangeMaxM(scene_->range);
}

// A position typed or dragged by the user: snapped (when asked), then held inside the range.
sm::Vec3 SpatializerUi::limit(sm::Vec3 v, bool snap) const {
    if (snap && scene_->snap) {
        const float g = rangeGridM(scene_->range);
        v = {snapTo(v.x, g), snapTo(v.y, g), snapTo(v.z, g)};
    }
    const float d = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const float mx = maxDistance();
    if (d > mx) {
        const float k = mx / d;
        v = {v.x * k, v.y * k, v.z * k};
    }
    if (d < kMinDistanceM && std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) < kMinDistanceM)
        v = {0.0f, 0.0f, -kMinDistanceM};
    return limitElevation(v, host_->elevationBand()); // the HRTF set's measured band; keeps the distance
}

void SpatializerUi::draw(Canvas& c, Scene& scene, const LivePositions& live, IUiHost& host,
                         uint32_t version) {
    scene_ = &scene;
    live_ = &live;
    host_ = &host;
    scene_->selected = std::clamp(scene_->selected, 0, static_cast<int>(scene_->sources.size()) - 1);
    if (version != seenVersion_) {
        seenVersion_ = version;
        cacheValid_ = false;
        selectedKey_ = 0;
    }
    drawRangeBar(c);
    refitElevation();
    if (!cacheValid_)
        rebuildCurves();

    const float s = c.uiScale();
    const dsptb_vec2 avail = c.availableSize();
    const float width = std::max(avail.x, 300.0f * s);
    viewScale_ = s;
    // Side by side when the panel is wide enough, otherwise stacked with a shorter side view.
    const bool wide = width >= 560.0f * s;
    const float gap = 8.0f * s;
    float size;
    if (wide)
        size = std::min({(width - gap) * 0.5f, 440.0f * s, std::max(240.0f * s, avail.y - 120.0f * s)});
    else
        size = std::min({width, 460.0f * s, std::max(240.0f * s, avail.y - 120.0f * s)});
    const float sideHeight = wide ? size : size * 0.62f;

    drawTopDown(c, size);
    if (wide)
        c.sameLine();
    drawSideView(c, size, sideHeight);
    c.label("Top view: drag a dot (Shift+drag = height). Side view: drag a dot for height and front/back. "
            "Wheel over a dot = distance. Distances are logarithmic from the head: fine control up close.");
    {
        const ElevationBand b = host_->elevationBand();
        if (b.minDeg > -90.0f || b.maxDeg < 90.0f) {
            char t[200];
            std::snprintf(t, sizeof t,
                          "Red zones: this HRTF set was only measured from %g to +%g degrees of height, so "
                          "sources are held inside it.",
                          static_cast<double>(b.minDeg), static_cast<double>(b.maxDeg));
            c.label(t);
        }
    }
    c.label(host_->hrtfStatus().c_str());
    c.label(("The range also sets the attenuation reference distance and the air absorption amount. Your own "
             "sets (SimpleFreeFieldHRIR .sofa, one distance): " +
             host_->hrtfFolder())
                .c_str());
    c.separator();
    drawSourceList(c);
    c.separator();
    drawSourceSettings(c);
    commitIfIdle(c);
}

void SpatializerUi::drawRangeBar(Canvas& c) {
    int range = static_cast<int>(scene_->range);
    static const char* const kRanges[] = {"Near (ASMR, up to 2 m)", "Mid (room, up to 10 m)",
                                          "Far (theater / outdoors, up to 50 m)"};
    if (c.combo("Range", range, kRanges, 3) && range != static_cast<int>(scene_->range)) {
        scene_->range = static_cast<Range>(range);
        fitToRange(*scene_, scene_->range);
        host_->applyRangePreset(scene_->range);
        dirtyLabel_ = "Change range";
        edited();
        changed();
    }
    if (c.checkbox("Snap to grid", scene_->snap)) {
        dirtyLabel_ = "Toggle snap";
        edited();
        changed();
    }
    c.sameLine();
    char t[96];
    std::snprintf(t, sizeof t, "grid %g m", static_cast<double>(rangeGridM(scene_->range)));
    c.label(t);

    // HRTF set: automatic per range, the built-in one, or any set found in the module's assets folders.
    const std::vector<HrtfSetInfo>& sets = host_->hrtfSets();
    std::vector<std::string> names = {"Automatic for the range", "Steam Audio built-in"};
    for (const HrtfSetInfo& s : sets)
        names.push_back(s.name);
    std::vector<const char*> items;
    for (const std::string& n : names)
        items.push_back(n.c_str());
    int cur = scene_->hrtf == "auto" ? 0 : scene_->hrtf == "builtin" ? 1 : 0;
    for (size_t i = 0; i < sets.size(); ++i)
        if (sets[i].id == scene_->hrtf)
            cur = static_cast<int>(i) + 2;
    if (c.combo("HRTF set", cur, items.data(), static_cast<uint32_t>(items.size()))) {
        scene_->hrtf = cur == 0 ? "auto" : cur == 1 ? "builtin" : sets[static_cast<size_t>(cur) - 2].id;
        dirtyLabel_ = "Change HRTF set";
        edited();
        changed();
    }
}

// A burst of edits (a slider drag, a dragged dot) is one undo step: it is recorded once the edits have
// stopped.
void SpatializerUi::commitIfIdle(Canvas&) {
    if (!dirty_) {
        idleFrames_ = 0;
        return;
    }
    if (editedThisFrame_) {
        idleFrames_ = 0;
        editedThisFrame_ = false;
        return;
    }
    if (++idleFrames_ >= 8 && dragging_ < 0 && sideDrag_ < 0) {
        host_->editFinished(dirtyLabel_.c_str());
        dirty_ = false;
        idleFrames_ = 0;
        dirtyLabel_ = "Edit spatializer";
    }
}

void SpatializerUi::rebuildCurves() {
    cacheValid_ = true;
    curve3_.clear();
    const Source& s = scene_->sources[static_cast<size_t>(scene_->selected)];
    if (s.motion == Motion::Orbit) {
        const sm::OrbitParams p = toOrbitParams(s.orbit);
        constexpr int kPoints = 128;
        for (int i = 0; i <= kPoints; ++i)
            curve3_.push_back(
                sm::orbitPosition(p, static_cast<double>(i) / kPoints * static_cast<double>(p.periodS)));
    } else if (s.motion == Motion::Path && s.path.spec.count >= 2) {
        // The whole pass, 64 samples per segment, at speed 1 (the curve does not depend on the speed).
        sm::PathSpec spec = s.path.spec;
        spec.speed = 1.0f;
        spec.mode = sm::PlayMode::Once;
        sm::PathEvaluator ev;
        ev.rebuild(spec);
        const int samples = static_cast<int>(spec.count - 1) * 64;
        for (int i = 0; i <= samples; ++i)
            curve3_.push_back(ev.positionAt(static_cast<double>(i) / samples * ev.duration()));
        if (s.path.spec.mode == sm::PlayMode::Loop && s.path.spec.closeLoop)
            curve3_.push_back(spec.keys[0].position);
    }
}

// Where a source is drawn: the audio thread's live position for a moving one.
sm::Vec3 SpatializerUi::shownPosition(int i) const {
    const Source& src = scene_->sources[static_cast<size_t>(i)];
    return src.motion == Motion::Static || live_->count <= i ? src.position
                                                             : live_->pos[static_cast<size_t>(i)];
}

// ----------------------------------------------------------------------------------------------------------
// top-down view
// ----------------------------------------------------------------------------------------------------------

void SpatializerUi::drawTopDown(Canvas& c, float size) {
    auto region = c.region("topdown", size, size);
    if (!region)
        return;
    const float s = viewScale_;
    const float R = size * 0.5f - 16.0f * s;
    const Scale sc{size * 0.5f, size * 0.5f, R, maxDistance(), headPx(R, s)};
    const uint32_t ink = dsptb::rgba(170, 176, 190);

    c.fillRect({0, 0}, {size, size}, dsptb::rgba(16, 18, 24));
    // Snap grid (only while snapping): a dot at every grid point, then the distance rings.
    if (scene_->snap) {
        const float gm = rangeGridM(scene_->range);
        const int n = static_cast<int>(sc.rangeM / gm);
        for (int i = -n; i <= n; ++i)
            for (int j = -n; j <= n; ++j) {
                if (std::sqrt(static_cast<float>(i * i + j * j)) * gm > sc.rangeM)
                    continue;
                const dsptb_vec2 q =
                    topScreen(sc, {static_cast<float>(i) * gm, 0.0f, static_cast<float>(j) * gm});
                c.fillRect({q.x - 1.0f * s, q.y - 1.0f * s}, {q.x + 1.0f * s, q.y + 1.0f * s},
                           dsptb::rgba(255, 255, 255, 60));
            }
    }
    const std::vector<float> rings = ringDistances(sc.rangeM);
    const std::vector<bool> labelled = ringLabels(sc, rings, 44.0f * s);
    for (size_t q = 0; q < rings.size(); ++q) {
        const float d = rings[q];
        const float r = sc.rOf(d);
        if (d >= sc.rangeM * 0.999f) // the range limit: nothing can be placed beyond it
            c.circle({sc.cx, sc.cy}, r, dsptb::rgba(230, 90, 70, 200), 2.0f);
        else
            c.circle({sc.cx, sc.cy}, r, dsptb::rgba(255, 255, 255, 38), 1.0f);
        if (!labelled[q])
            continue;
        char t[16];
        std::snprintf(t, sizeof t, "%g m", static_cast<double>(d));
        c.text({sc.cx + r + 2.0f * s, sc.cy + 1.0f * s}, dsptb::rgba(130, 136, 150), t);
    }
    c.line({sc.cx, sc.cy - sc.radius}, {sc.cx, sc.cy + sc.radius}, dsptb::rgba(255, 255, 255, 30), 1.0f);
    c.line({sc.cx - sc.radius, sc.cy}, {sc.cx + sc.radius, sc.cy}, dsptb::rgba(255, 255, 255, 30), 1.0f);
    // Where the selected source cannot go at its current height (the HRTF set was not measured there).
    {
        const ElevationBand band = host_->elevationBand();
        const float y = selected().position.y;
        float rd = 0.0f;
        constexpr float kRad = 3.14159265f / 180.0f;
        if (y > 1e-4f && band.maxDeg < 90.0f && band.maxDeg > 0.0f)
            rd = y / std::tan(band.maxDeg * kRad);
        else if (y < -1e-4f && band.minDeg > -90.0f && band.minDeg < 0.0f)
            rd = -y / std::tan(-band.minDeg * kRad);
        if (rd > 0.0f) {
            const float rr = sc.rOf(std::min(rd, sc.rangeM));
            c.circle({sc.cx, sc.cy}, rr, dsptb::rgba(230, 70, 60, 45), 1.0f, true);
            c.circle({sc.cx, sc.cy}, rr, dsptb::rgba(230, 90, 70, 170), 1.5f);
        }
    }
    c.text({sc.cx - 16.0f * s, 1.0f * s}, ink, "Front");
    c.text({sc.cx - 14.0f * s, size - 15.0f * s}, ink, "Back");
    c.text({2.0f * s, sc.cy - 7.0f * s}, ink, "Left");
    c.text({size - 34.0f * s, sc.cy - 7.0f * s}, ink, "Right");
    drawHead(c, sc.cx, sc.cy, sc.headPx / 8.0f);

    Source& sel = selected();
    const uint32_t selColour = sel.colour;

    // The selected source's orbit or path, with numbered keyframe markers.
    if (curve3_.size() >= 2) {
        screenPts_.clear();
        for (const sm::Vec3& p : curve3_)
            screenPts_.push_back(topScreen(sc, p));
        c.polyline(screenPts_.data(), static_cast<uint32_t>(screenPts_.size()), withAlpha(selColour, 190),
                   1.5f, sel.motion == Motion::Orbit);
    }
    if (sel.motion == Motion::Path) {
        for (uint32_t i = 0; i < sel.path.spec.count; ++i) {
            const dsptb_vec2 p = topScreen(sc, sel.path.spec.keys[i].position);
            const bool on = static_cast<int>(i) == selectedKey_;
            c.circle(p, 7.0f * s, on ? dsptb::rgba(255, 255, 255) : withAlpha(selColour, 255),
                     on ? 2.0f : 1.5f);
            char t[8];
            std::snprintf(t, sizeof t, "%u", i + 1);
            c.text({p.x - 3.0f * s, p.y - 7.0f * s}, dsptb::rgba(255, 255, 255), t);
        }
    }

    // Sources, drawn where the audio thread says they are right now.
    struct Hit {
        int index;
        dsptb_vec2 pos;
        float radius;
    };
    Hit hits[kMaxSources];
    int hitCount = 0;
    for (int i = 0; i < static_cast<int>(scene_->sources.size()); ++i) {
        const Source& src = scene_->sources[static_cast<size_t>(i)];
        const sm::Vec3 at = shownPosition(i);
        const dsptb_vec2 p = topScreen(sc, at);
        const float rr = (5.0f + 3.0f * (1.0f + elevationSin(at))) * s; // bigger = higher
        const bool on = i == scene_->selected;
        c.circle(p, rr, withAlpha(src.colour, src.mute ? 90 : 255), 1.0f, true);
        if (on)
            c.circle(p, rr + 2.5f * s, dsptb::rgba(255, 255, 255), 2.0f);
        char t[8];
        std::snprintf(t, sizeof t, "%d", i + 1);
        c.text({p.x - 3.0f * s, p.y - 7.0f * s}, dsptb::rgba(10, 10, 14), t);
        // height tick beside the dot: up for above the ear plane, down for below
        const float tick = elevationSin(at) * 12.0f * s;
        c.line({p.x + rr + 4.0f * s, p.y}, {p.x + rr + 4.0f * s, p.y - tick}, withAlpha(src.colour, 255),
               2.0f);
        hits[hitCount++] = {i, p, rr + 4.0f * s};
    }
    // The edit cursor of a moving source: a hollow ring where "Set position" / "Add keyframe" would use.
    if (sel.motion != Motion::Static) {
        const dsptb_vec2 p = topScreen(sc, sel.position);
        c.circle(p, 6.0f * s, dsptb::rgba(255, 255, 255), 1.5f);
        c.line({p.x - 9.0f * s, p.y}, {p.x + 9.0f * s, p.y}, dsptb::rgba(255, 255, 255, 160), 1.0f);
        c.line({p.x, p.y - 9.0f * s}, {p.x, p.y + 9.0f * s}, dsptb::rgba(255, 255, 255, 160), 1.0f);
    }

    // ---- mouse ----
    const dsptb_mouse m = c.mouse();
    int over = -1;
    float best = 1e9f;
    for (int h = 0; h < hitCount; ++h) {
        const float dx = m.pos.x - hits[h].pos.x, dy = m.pos.y - hits[h].pos.y;
        const float dd = std::sqrt(dx * dx + dy * dy);
        if (dd <= hits[h].radius && dd < best) {
            best = dd;
            over = hits[h].index;
        }
    }
    const auto groundAt = [&](dsptb_vec2 px, float y) {
        const float dx = px.x - sc.cx, dy = px.y - sc.cy;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float h = sc.dOf(r);
        return limit(r < 1e-3f ? sm::Vec3{0.0f, y, 0.0f} : sm::Vec3{dx / r * h, y, dy / r * h}, true);
    };
    if (m.clicked[0] && over >= 0) {
        scene_->selected = over;
        cacheValid_ = false;
        dragging_ = over;
        heightAccum_ = scene_->sources[static_cast<size_t>(over)].position.y;
        lastPlaced_[static_cast<size_t>(over)] = scene_->sources[static_cast<size_t>(over)].position;
    }
    if (dragging_ >= 0 && !(m.down[0] || m.active)) {
        dragging_ = -1;
    } else if (dragging_ >= 0 && m.down[0]) {
        Source& d = scene_->sources[static_cast<size_t>(dragging_)];
        if (m.shift) { // Shift: height (Y) instead of the ground plane
            // Accumulated unsnapped, so a snapped drag still moves in steps of the grid.
            const sm::Vec3& q = d.position;
            const float r0 = sc.rOf(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z));
            heightAccum_ -= m.drag_delta.y * (sc.dOf(r0 + 1.0f) - sc.dOf(r0)); // metres per pixel here
            d.position = limit({d.position.x, heightAccum_, d.position.z}, true);
        } else {
            d.position = groundAt(m.pos, d.position.y);
        }
        dirtyLabel_ = "Move source";
        edited();
        changed();
    }
    if (over >= 0 && m.wheel != 0.0f) { // wheel: distance
        Source& d = scene_->sources[static_cast<size_t>(over)];
        const float f = 1.0f + 0.1f * clampf(m.wheel, -3.0f, 3.0f);
        sm::Vec3 v{d.position.x * f, d.position.y * f, d.position.z * f};
        d.position = limit(v, false);
        dirtyLabel_ = "Change distance";
        edited();
        changed();
    }
    if (m.double_clicked[0] && over < 0) { // double-click on empty space: put the cursor there
        Source& d = selected();
        lastPlaced_[static_cast<size_t>(scene_->selected)] = d.position;
        d.position = groundAt(m.pos, d.position.y);
        dirtyLabel_ = "Move source";
        edited();
        changed();
    }
}

// ----------------------------------------------------------------------------------------------------------
// side view: front/back across, height up (the same scale as the top view)
// ----------------------------------------------------------------------------------------------------------

void SpatializerUi::drawSideView(Canvas& c, float width, float height) {
    auto region = c.region("side", width, height);
    if (!region)
        return;
    const float s = viewScale_;
    const float R = width * 0.5f - 16.0f * s;
    const Scale sc{width * 0.5f, height * 0.5f, R, maxDistance(), headPx(R, s)};
    const float halfH = height * 0.5f - 6.0f * s;
    const uint32_t ink = dsptb::rgba(170, 176, 190);
    c.fillRect({0, 0}, {width, height}, dsptb::rgba(16, 18, 24));
    // The elevations the HRTF set was not measured at: hatched, with the limit drawn as a line.
    {
        const ElevationBand band = host_->elevationBand();
        const auto ray = [&](float elDeg, float sideSign, uint32_t col, float thick) {
            constexpr float kRad = 3.14159265f / 180.0f;
            dsptb_vec2 pts[12];
            for (int i = 0; i < 12; ++i) {
                const float d = kHeadM * std::pow(sc.rangeM / kHeadM, static_cast<float>(i) / 11.0f);
                pts[i] = sideScreen(
                    sc, {0.0f, d * std::sin(elDeg * kRad), -sideSign * d * std::cos(elDeg * kRad)}, halfH);
            }
            c.polyline(pts, 12, col, thick);
        };
        const uint32_t hatch = dsptb::rgba(230, 70, 60, 50), edge = dsptb::rgba(230, 90, 70, 190);
        for (const float sideSign : {-1.0f, 1.0f}) {
            if (band.maxDeg < 90.0f) {
                for (float e = band.maxDeg + 4.0f; e < 90.0f; e += 4.0f)
                    ray(e, sideSign, hatch, 2.0f);
                ray(band.maxDeg, sideSign, edge, 1.5f);
            }
            if (band.minDeg > -90.0f) {
                for (float e = band.minDeg - 4.0f; e > -90.0f; e -= 4.0f)
                    ray(e, sideSign, hatch, 2.0f);
                ray(band.minDeg, sideSign, edge, 1.5f);
            }
        }
    }
    { // the range limit seen from the side: the points at the maximum distance, all around the head
        constexpr int kN = 64;
        dsptb_vec2 pts[kN];
        for (int i = 0; i < kN; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / kN;
            pts[i] = sideScreen(sc, {0.0f, sc.rangeM * std::sin(a), sc.rangeM * std::cos(a)}, halfH);
        }
        c.polyline(pts, kN, dsptb::rgba(230, 90, 70, 200), 2.0f, true);
    }
    if (scene_->snap) {
        const float gm = rangeGridM(scene_->range);
        const int n = static_cast<int>(sc.rangeM / gm);
        for (int i = -n; i <= n; ++i)
            for (int j = -n; j <= n; ++j) {
                if (std::sqrt(static_cast<float>(i * i + j * j)) * gm > sc.rangeM)
                    continue;
                const dsptb_vec2 q =
                    sideScreen(sc, {0.0f, static_cast<float>(j) * gm, static_cast<float>(i) * gm}, halfH);
                c.fillRect({q.x - 1.0f * s, q.y - 1.0f * s}, {q.x + 1.0f * s, q.y + 1.0f * s},
                           dsptb::rgba(255, 255, 255, 60));
            }
    }
    const std::vector<float> sideRings = ringDistances(sc.rangeM);
    const std::vector<bool> sideLabelled = ringLabels(sc, sideRings, 44.0f * s);
    for (size_t q = 0; q < sideRings.size(); ++q) { // the same distance rings, as tick marks on the axis
        const float d = sideRings[q];
        const float r = sc.rOf(d);
        c.line({sc.cx + r, sc.cy - 4.0f * s}, {sc.cx + r, sc.cy + 4.0f * s}, dsptb::rgba(255, 255, 255, 60),
               1.0f);
        c.line({sc.cx - r, sc.cy - 4.0f * s}, {sc.cx - r, sc.cy + 4.0f * s}, dsptb::rgba(255, 255, 255, 60),
               1.0f);
        if (!sideLabelled[q])
            continue;
        char t[16];
        std::snprintf(t, sizeof t, "%g m", static_cast<double>(d));
        c.text({sc.cx + r - 12.0f * s, sc.cy + 6.0f * s}, dsptb::rgba(130, 136, 150), t);
    }
    c.line({sc.cx - sc.radius, sc.cy}, {sc.cx + sc.radius, sc.cy}, dsptb::rgba(255, 255, 255, 60), 1.0f);
    c.line({sc.cx, sc.cy - halfH}, {sc.cx, sc.cy + halfH}, dsptb::rgba(255, 255, 255, 30), 1.0f);
    c.text({width - 36.0f * s, sc.cy - 15.0f * s}, ink, "Front");
    c.text({2.0f * s, sc.cy - 15.0f * s}, ink, "Back");
    c.text({sc.cx + 4.0f * s, 1.0f * s}, ink, "Up");
    c.text({sc.cx + 4.0f * s, height - 15.0f * s}, ink, "Down");
    // The head seen from the side, facing right (nose = front).
    const uint32_t col = dsptb::rgba(220, 224, 235);
    const float hs = sc.headPx / 8.0f; // the head, drawn at the scale's head size
    c.circle({sc.cx, sc.cy}, 8.0f * hs, col, 1.5f);
    const dsptb_vec2 nose[3] = {{sc.cx + 7.0f * hs, sc.cy - 4.0f * hs},
                                {sc.cx + 14.0f * hs, sc.cy},
                                {sc.cx + 7.0f * hs, sc.cy + 4.0f * hs}};
    c.polyline(nose, 3, col, 1.5f, true);

    Source& sel = selected();
    if (curve3_.size() >= 2) {
        screenPts_.clear();
        for (const sm::Vec3& p : curve3_)
            screenPts_.push_back(sideScreen(sc, p, halfH));
        c.polyline(screenPts_.data(), static_cast<uint32_t>(screenPts_.size()), withAlpha(sel.colour, 190),
                   1.5f, sel.motion == Motion::Orbit);
    }
    if (sel.motion == Motion::Path) {
        for (uint32_t i = 0; i < sel.path.spec.count; ++i) {
            const dsptb_vec2 p = sideScreen(sc, sel.path.spec.keys[i].position, halfH);
            c.circle(p, 6.0f * s, withAlpha(sel.colour, 255), 1.5f);
        }
    }

    struct Hit {
        int index;
        dsptb_vec2 pos;
    };
    Hit hits[kMaxSources];
    int hitCount = 0;
    for (int i = 0; i < static_cast<int>(scene_->sources.size()); ++i) {
        const Source& src = scene_->sources[static_cast<size_t>(i)];
        const dsptb_vec2 p = sideScreen(sc, shownPosition(i), halfH);
        const bool on = i == scene_->selected;
        c.circle(p, 6.0f * s, withAlpha(src.colour, src.mute ? 90 : 255), 1.0f, true);
        if (on)
            c.circle(p, 8.5f * s, dsptb::rgba(255, 255, 255), 1.5f);
        char t[8];
        std::snprintf(t, sizeof t, "%d", i + 1);
        c.text({p.x - 3.0f * s, p.y - 7.0f * s}, dsptb::rgba(10, 10, 14), t);
        hits[hitCount++] = {i, p};
    }
    if (sel.motion != Motion::Static) {
        const dsptb_vec2 p = sideScreen(sc, sel.position, halfH);
        c.circle(p, 6.0f * s, dsptb::rgba(255, 255, 255), 1.5f);
    }

    const dsptb_mouse m = c.mouse();
    if (m.clicked[0]) {
        float best = 12.0f * s;
        for (int i = 0; i < hitCount; ++i) {
            const float dx = m.pos.x - hits[i].pos.x, dy = m.pos.y - hits[i].pos.y;
            const float dd = std::sqrt(dx * dx + dy * dy);
            if (dd < best) {
                best = dd;
                scene_->selected = hits[i].index;
                cacheValid_ = false;
                sideDrag_ = hits[i].index;
            }
        }
        if (sideDrag_ < 0 && m.hovered)
            sideDrag_ = scene_->selected; // empty space: the selected source's cursor follows
        if (sideDrag_ >= 0)
            lastPlaced_[static_cast<size_t>(sideDrag_)] =
                scene_->sources[static_cast<size_t>(sideDrag_)].position;
    }
    if (sideDrag_ >= 0 && !(m.down[0] || m.active)) {
        sideDrag_ = -1;
    } else if (sideDrag_ >= 0 && m.down[0]) {
        Source& d = scene_->sources[static_cast<size_t>(sideDrag_)];
        const float dx = m.pos.x - sc.cx, dy = m.pos.y - sc.cy;
        const float z = -sgn(dx) * sc.dOf(std::fabs(dx)); // front (-Z) is to the right
        const float y = -sgn(dy) * sc.dOf(std::fabs(dy));
        d.position = limit({d.position.x, y, z}, true);
        dirtyLabel_ = "Move source";
        edited();
        changed();
    }
}

// ----------------------------------------------------------------------------------------------------------
// source list and settings
// ----------------------------------------------------------------------------------------------------------

void SpatializerUi::drawSourceList(Canvas& c) {
    c.label("Sources");
    for (int i = 0; i < static_cast<int>(scene_->sources.size()); ++i) {
        const Source& src = scene_->sources[static_cast<size_t>(i)];
        auto id = c.idScope(("src" + std::to_string(i)).c_str());
        char t[96];
        std::snprintf(t, sizeof t, "%d  %s   [%s]%s%s", i + 1, src.name.c_str(), motionLabel(src.motion),
                      src.mute ? "  muted" : "", src.solo ? "  solo" : "");
        if (c.selectable(t, i == scene_->selected)) {
            scene_->selected = i;
            cacheValid_ = false;
        }
    }
    const bool canAdd = scene_->sources.size() < static_cast<size_t>(kMaxSources);
    if (c.button(canAdd ? "Add source" : "Add source (8 max)") && canAdd) {
        scene_->sources.push_back(makeSource(static_cast<int>(scene_->sources.size())));
        scene_->selected = static_cast<int>(scene_->sources.size()) - 1;
        dirtyLabel_ = "Add source";
        edited();
        changed();
    }
    c.sameLine();
    const bool canRemove = scene_->sources.size() > 1;
    if (c.button("Remove source") && canRemove) {
        scene_->sources.erase(scene_->sources.begin() + scene_->selected);
        scene_->selected = std::clamp(scene_->selected, 0, static_cast<int>(scene_->sources.size()) - 1);
        dirtyLabel_ = "Remove source";
        edited();
        changed();
    }
}

void SpatializerUi::drawSourceSettings(Canvas& c) {
    Source& src = selected();
    auto id = c.idScope(("sel" + std::to_string(scene_->selected)).c_str());
    char t[96];
    std::snprintf(t, sizeof t, "Source %d: %s", scene_->selected + 1, src.name.c_str());
    c.label(t);

    int input = static_cast<int>(src.input);
    static const char* const kInputs[] = {"Mono sum (L+R)/2", "Left", "Right"};
    if (c.combo("Input", input, kInputs, 3)) {
        src.input = static_cast<InputSel>(input);
        dirtyLabel_ = "Change source input";
        edited();
        changed();
    }
    if (c.sliderFloat("Gain (dB)", src.gainDb, -60.0f, 12.0f, "%.1f dB")) {
        dirtyLabel_ = "Change source gain";
        edited();
        changed();
    }
    if (c.checkbox("Mute", src.mute)) {
        dirtyLabel_ = "Mute source";
        edited();
        changed();
    }
    c.sameLine();
    if (c.checkbox("Solo", src.solo)) {
        dirtyLabel_ = "Solo source";
        edited();
        changed();
    }

    int motion = static_cast<int>(src.motion);
    static const char* const kMotions[] = {"Static", "Orbit", "Path"};
    if (c.combo("Motion", motion, kMotions, 3)) {
        src.motion = static_cast<Motion>(motion);
        dirtyLabel_ = "Change motion";
        edited();
        changed();
    }
    switch (src.motion) {
    case Motion::Static:
        drawStatic(c, src);
        break;
    case Motion::Orbit:
        drawOrbit(c, src);
        break;
    case Motion::Path:
        drawPath(c, src);
        break;
    }
}

void SpatializerUi::drawStatic(Canvas& c, Source& s) {
    float xyz[3] = {s.position.x, s.position.y, s.position.z};
    if (c.dragFloat3("x (right)  y (up)  z (back)  m", xyz, 0.05f, -maxDistance(), maxDistance(), "%.2f")) {
        s.position = limit({xyz[0], xyz[1], xyz[2]}, true);
        dirtyLabel_ = "Move source";
        edited();
        changed();
    }
    sm::Spherical sp = sm::toSpherical(s.position);
    float aed[3] = {sp.azimuthDeg, sp.elevationDeg, sp.distance};
    if (c.dragFloat3("azimuth  elevation  distance", aed, 0.5f, -180.0f, maxDistance(), "%.1f")) {
        s.position = limit(fromSph(aed[0], aed[1], aed[2]), true);
        dirtyLabel_ = "Move source";
        edited();
        changed();
    }
    c.label("Drag the dot in either view; Shift+drag in the top view changes height; wheel over a dot "
            "changes distance.");
    if (c.button("Add keyframe"))
        addKeyframe(c, s);
}

void SpatializerUi::drawOrbit(Canvas& c, Source& s) {
    OrbitSettings& o = s.orbit;
    bool ch = false;
    int shape = o.shape == sm::OrbitShape::Figure8 ? 1 : 0;
    static const char* const kShapes[] = {"Circle", "Figure-8"};
    if (c.combo("Shape", shape, kShapes, 2)) {
        o.shape = shape == 1 ? sm::OrbitShape::Figure8 : sm::OrbitShape::Circle;
        ch = true;
    }
    float centre[3] = {o.center.x, o.center.y, o.center.z};
    if (c.dragFloat3("Centre (m)", centre, 0.05f, -maxDistance(), maxDistance(), "%.2f")) {
        o.center = limit({centre[0], centre[1], centre[2]}, true);
        ch = true;
    }
    ch |= c.sliderFloat("Radius (m)", o.radius, 0.02f, maxDistance(), "%.2f");
    ch |= c.sliderFloat("Height tilt of the orbit plane (deg)", o.elevationDeg, -90.0f, 90.0f, "%.0f");
    ch |= c.sliderFloat("Period (s)", o.periodS, 0.5f, 120.0f, "%.1f");
    int dir = o.direction == sm::Direction::CounterClockwise ? 1 : 0;
    static const char* const kDirs[] = {"Clockwise", "Counter-clockwise"};
    if (c.combo("Direction", dir, kDirs, 2)) {
        o.direction = dir == 1 ? sm::Direction::CounterClockwise : sm::Direction::Clockwise;
        ch = true;
    }
    ch |= c.sliderFloat("Start angle (deg)", o.startAngleDeg, -360.0f, 360.0f, "%.0f");
    char t[64];
    std::snprintf(t, sizeof t, "Speed: %.1f deg/s", static_cast<double>(360.0f / o.periodS));
    c.label(t);
    if (ch) {
        dirtyLabel_ = "Change orbit";
        edited();
        changed();
    }
    c.label("The ring in the view is the edit cursor (drag a dot to move it).");
    if (c.button("Set position"))
        setPosition(s);
    c.sameLine();
    if (c.button("Add keyframe"))
        addKeyframe(c, s);
}

void SpatializerUi::setPosition(Source& s) {
    s.motion = Motion::Static;
    dirtyLabel_ = "Set position";
    edited();
    changed();
}

void SpatializerUi::addKeyframe(Canvas&, Source& s) {
    sm::PathSpec& spec = s.path.spec;
    if (s.motion != Motion::Path) {
        // The path starts where the source is now: for a moving source its live position, for a static one
        // the place it had before the cursor was moved.
        const size_t index = static_cast<size_t>(scene_->selected);
        const sm::Vec3 here = s.motion == Motion::Static
                                  ? lastPlaced_[index]
                                  : (live_->count > scene_->selected ? live_->pos[index] : s.position);
        s.motion = Motion::Path;
        if (spec.count == 0) {
            sm::Keyframe first;
            first.position = here;
            spec.keys[spec.count++] = first;
        }
    }
    if (spec.count < sm::kMaxKeyframes) {
        sm::Keyframe k; // travel 2 s, ease Smooth (the defaults)
        k.position = s.position;
        spec.keys[spec.count++] = k;
        selectedKey_ = static_cast<int>(spec.count) - 1;
    }
    dirtyLabel_ = "Add keyframe";
    edited();
    changed();
}

void SpatializerUi::drawPath(Canvas& c, Source& s) {
    PathSettings& p = s.path;
    sm::PathSpec& spec = p.spec;
    bool ch = false;

    int mode = static_cast<int>(spec.mode);
    static const char* const kModes[] = {"Once", "Loop", "Ping-pong"};
    if (c.combo("Mode", mode, kModes, 3)) {
        spec.mode = static_cast<sm::PlayMode>(mode);
        ch = true;
    }
    ch |= c.sliderFloat("Speed", spec.speed, 0.1f, 10.0f, "%.2fx");
    ch |= c.checkbox("Close loop (travel back to keyframe 1 when looping)", spec.closeLoop);
    int clock = static_cast<int>(p.clock);
    static const char* const kClocks[] = {"Auto (transport for a file, free for the mic)", "Transport",
                                          "Free"};
    if (c.combo("Clock", clock, kClocks, 3)) {
        p.clock = static_cast<ClockSel>(clock);
        ch = true;
    }
    ch |= c.inputFloat("Start offset (s)", p.startOffsetS, "%.2f");
    p.startOffsetS = clampf(p.startOffsetS, -3600.0f, 3600.0f);
    const int index = scene_->selected;
    if (host_->freeRunning(index)) {
        if (c.button("Stop free clock"))
            host_->freeStop(index);
    } else if (c.button("Play free clock")) {
        host_->freeStart(index);
    }
    c.sameLine();
    c.label("(for the Free clock: path time = seconds since Play)");

    c.separator();
    char t[48];
    std::snprintf(t, sizeof t, "Keyframes (%u of %zu)", spec.count, sm::kMaxKeyframes);
    c.label(t);
    // One row per keyframe: position as az / el / distance, travel and hold times, ease, and row actions.
    int removeAt = -1, moveUp = -1, moveDown = -1;
    for (uint32_t i = 0; i < spec.count; ++i) {
        auto id = c.idScope(("kf" + std::to_string(i)).c_str());
        sm::Keyframe& k = spec.keys[i];
        if (c.selectable((std::to_string(i + 1) + (i == 0 ? "  (start)" : "")).c_str(),
                         static_cast<int>(i) == selectedKey_))
            selectedKey_ = static_cast<int>(i);
        const sm::Spherical sp = sm::toSpherical(k.position);
        float aed[3] = {sp.azimuthDeg, sp.elevationDeg, sp.distance};
        if (c.dragFloat3("azimuth  elevation  distance", aed, 0.5f, -180.0f, maxDistance(), "%.1f")) {
            k.position = limit(fromSph(aed[0], aed[1], aed[2]), true);
            ch = true;
        }
        if (i > 0) {
            ch |= c.inputFloat("travel (s)", k.travelTimeS, "%.2f");
            k.travelTimeS = clampf(k.travelTimeS, 0.0f, 600.0f);
        } else if (spec.mode == sm::PlayMode::Loop && spec.closeLoop) {
            ch |= c.inputFloat("closing travel (s)", k.travelTimeS, "%.2f");
            k.travelTimeS = clampf(k.travelTimeS, 0.0f, 600.0f);
        }
        ch |= c.inputFloat("hold (s)", k.holdS, "%.2f");
        k.holdS = clampf(k.holdS, 0.0f, 600.0f);
        int ease = static_cast<int>(k.ease);
        static const char* const kEases[] = {"Step", "Linear", "Smooth"};
        if (c.combo("ease into", ease, kEases, 3)) {
            k.ease = static_cast<sm::Ease>(ease);
            ch = true;
        }
        if (c.button("Go to")) // moves the edit cursor to this keyframe
            s.position = k.position, selectedKey_ = static_cast<int>(i), ch = true;
        c.sameLine();
        if (c.button("Up") && i > 0)
            moveUp = static_cast<int>(i);
        c.sameLine();
        if (c.button("Down") && i + 1 < spec.count)
            moveDown = static_cast<int>(i);
        c.sameLine();
        if (c.button("Delete"))
            removeAt = static_cast<int>(i);
        c.separator();
    }
    if (moveUp > 0) {
        std::swap(spec.keys[static_cast<size_t>(moveUp)], spec.keys[static_cast<size_t>(moveUp) - 1]);
        selectedKey_ = moveUp - 1;
        ch = true;
    }
    if (moveDown >= 0) {
        std::swap(spec.keys[static_cast<size_t>(moveDown)], spec.keys[static_cast<size_t>(moveDown) + 1]);
        selectedKey_ = moveDown + 1;
        ch = true;
    }
    if (removeAt >= 0) {
        for (uint32_t i = static_cast<uint32_t>(removeAt); i + 1 < spec.count; ++i)
            spec.keys[i] = spec.keys[i + 1];
        --spec.count;
        selectedKey_ = std::clamp(selectedKey_, 0, std::max(0, static_cast<int>(spec.count) - 1));
        ch = true;
    }
    if (ch) {
        dirtyLabel_ = "Edit path";
        edited();
        changed();
    }
    c.label("The ring in the view is the edit cursor (drag a dot to move it).");
    if (c.button("Set position"))
        setPosition(s);
    c.sameLine();
    if (c.button("Add keyframe"))
        addKeyframe(c, s);
}

} // namespace dsptb::spat
