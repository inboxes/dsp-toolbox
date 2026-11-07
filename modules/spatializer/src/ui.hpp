#pragma once

// The Spatializer's custom UI: top-down view, elevation strip, source
// list and keyframe editor, all drawn through the canvas API. UI thread only.

#include "scene.hpp"

#include <dsptb/canvas.hpp>

#include <array>
#include <string>
#include <vector>

namespace dsptb::spat {

struct HrtfSetInfo {
    std::string id;
    std::string name;
};

// What the UI needs from the module around it.
class IUiHost {
public:
    virtual ~IUiHost() = default;
    // The scene changed: rebuild the snapshot and hand it to the audio thread.
    virtual void sceneChanged() = 0;
    // The HRTF sets found on disk (packaged and custom), what is loaded now, and where custom sets go.
    virtual const std::vector<HrtfSetInfo>& hrtfSets() const = 0;
    virtual std::string hrtfStatus() const = 0;
    virtual std::string hrtfFolder() const = 0;
    // A range preset was picked: set the attenuation reference distance and the air absorption amount.
    virtual void applyRangePreset(Range r) = 0;
    // A user edit is finished (mouse released, value typed): this is where an undo step is recorded.
    virtual void editFinished(const char* undoLabel) = 0;
    // The free clock of a path source (the play and stop buttons).
    virtual void freeStart(int source) = 0;
    virtual void freeStop(int source) = 0;
    virtual bool freeRunning(int source) const = 0;
    // The elevation band the active HRTF set was measured in (sources are held inside it).
    virtual ElevationBand elevationBand() const = 0;
};

class SpatializerUi {
public:
    // ersion changes whenever the scene was replaced from outside the UI (load_state, undo).
    void draw(Canvas& c, Scene& scene, const LivePositions& live, IUiHost& host, uint32_t version);

private:
    // ---- views ----
    void drawRangeBar(Canvas& c);
    void drawTopDown(Canvas& c, float size);
    void drawSideView(Canvas& c, float width, float height);
    float maxDistance() const;
    float headPx(float radiusPx, float uiScale) const;
    void refitElevation();
    sm::Vec3 limit(sm::Vec3 v, bool snap) const;
    sm::Vec3 shownPosition(int i) const;
    void drawSourceList(Canvas& c);
    void drawSourceSettings(Canvas& c);
    void drawStatic(Canvas& c, Source& s);
    void drawOrbit(Canvas& c, Source& s);
    void drawPath(Canvas& c, Source& s);

    // ---- helpers ----
    Source& selected() { return scene_->sources[static_cast<size_t>(scene_->selected)]; }
    void changed() { // the scene changed this frame
        host_->sceneChanged();
        dirty_ = true;
        cacheValid_ = false;
    }
    void commitIfIdle(Canvas& c);
    void edited() { // a user edit happened this frame
        dirty_ = true;
        editedThisFrame_ = true;
    }
    void setPosition(Source& s);
    void addKeyframe(Canvas& c, Source& s);
    void rebuildCurves();

    Scene* scene_ = nullptr;
    const LivePositions* live_ = nullptr;
    IUiHost* host_ = nullptr;

    bool dirty_ = false; // an edit happened and has not been committed as an undo step yet
    bool editedThisFrame_ = false;
    int idleFrames_ = 0; // frames since the last edit; the undo step is recorded after a short pause
    uint32_t seenVersion_ = 0xFFFFFFFFu;
    int sideDrag_ = -1;                              // source being dragged in the side view
    float heightAccum_ = 0.0f;                       // Shift+drag height, before snapping
    std::array<sm::Vec3, kMaxSources> lastPlaced_{}; // where a static source was before its cursor last moved
    std::string dirtyLabel_ = "Edit spatializer";
    int dragging_ = -1; // source being dragged in the top-down view or the strip
    bool dragMoved_ = false;
    int selectedKey_ = 0; // keyframe row selected in the path editor
    bool cacheValid_ = false;
    std::vector<dsptb_vec2>
        curveXz_; // selected source's orbit or path, in metres (x, z), for the top-down view
    std::vector<sm::Vec3> curve3_;
    std::vector<dsptb_vec2> screenPts_;
    float viewScale_ = 1.0f;
};

} // namespace dsptb::spat
