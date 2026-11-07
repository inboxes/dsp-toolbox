#pragma once

#include <dsptb/plugin_abi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dsptb {

// A fake dsptb_canvas_api that draws nothing and records what a module's draw() does. It
// checks that calls are well formed: regions, ids, clips, child areas and columns balanced; drawing and
// region-only calls inside a region; no invalid textures. Used by `dsptb-cli validate` and the tests.
class RecordingCanvas {
public:
    RecordingCanvas();
    RecordingCanvas(const RecordingCanvas&) = delete;
    RecordingCanvas& operator=(const RecordingCanvas&) = delete;

    const dsptb_canvas_api* api() const { return &api_; }
    // Call between frames: returns false and describes the first problem found since the last call.
    bool finishFrame(std::string& problem);

    size_t calls() const { return calls_; }
    size_t drawCalls() const { return drawCalls_; }
    size_t widgetCalls() const { return widgetCalls_; }
    size_t liveTextures() const { return textures_.size(); }
    // What begin_region reports (default: visible).
    void setRegionVisible(bool v) { regionVisible_ = v; }

private:
    friend struct CanvasThunks;
    void fail(const char* what);
    void draw(const char* what);
    void widget(const char*) {
        ++calls_;
        ++widgetCalls_;
    }

    dsptb_canvas_api api_{};
    size_t calls_ = 0, drawCalls_ = 0, widgetCalls_ = 0;
    int regionDepth_ = 0, idDepth_ = 0, clipDepth_ = 0, childDepth_ = 0, columnDepth_ = 0;
    bool regionVisible_ = true;
    std::string problem_;
    std::vector<uint64_t> textures_;
    uint64_t nextTexture_ = 1;
};

} // namespace dsptb
