#include "dsptb/core/recording_canvas.hpp"

#include <algorithm>

namespace dsptb {

struct CanvasThunks {
    static RecordingCanvas& c(void* ctx) { return *static_cast<RecordingCanvas*>(ctx); }

    static float uiScale(void* x) {
        ++c(x).calls_;
        return 1.0f;
    }
    static dsptb_vec2 available(void* x) {
        ++c(x).calls_;
        return {400.0f, 300.0f};
    }
    static int beginRegion(void* x, const char* id, float w, float h) {
        auto& s = c(x);
        ++s.calls_;
        if (!id || !*id)
            s.fail("begin_region with an empty id");
        if (!(w > 0.0f) || !(h > 0.0f))
            s.fail("begin_region with a non-positive size");
        if (s.regionDepth_ > 0)
            s.fail("begin_region inside another region");
        ++s.regionDepth_;
        return s.regionVisible_ ? 1 : 0;
    }
    static void endRegion(void* x) {
        auto& s = c(x);
        ++s.calls_;
        if (s.regionDepth_ == 0)
            s.fail("end_region without begin_region");
        else
            --s.regionDepth_;
    }
    static void mouse(void* x, dsptb_mouse* out) {
        auto& s = c(x);
        ++s.calls_;
        if (!out)
            s.fail("mouse with a null output");
        else {
            const uint32_t size = out->struct_size;
            *out = dsptb_mouse{};
            out->struct_size = size;
        }
    }
    static void line(void* x, dsptb_vec2, dsptb_vec2, uint32_t, float) { c(x).draw("line"); }
    static void rect(void* x, dsptb_vec2, dsptb_vec2, uint32_t, float, int) { c(x).draw("rect"); }
    static void circle(void* x, dsptb_vec2, float, uint32_t, float, int) { c(x).draw("circle"); }
    static void polyline(void* x, const dsptb_vec2* pts, uint32_t n, uint32_t, float, int) {
        if (!pts && n > 0)
            c(x).fail("polyline with null points");
        c(x).draw("polyline");
    }
    static void text(void* x, dsptb_vec2, uint32_t, const char* t) {
        if (!t)
            c(x).fail("text with a null string");
        c(x).draw("text");
    }
    static void image(void* x, dsptb_texture tex, dsptb_vec2, dsptb_vec2, dsptb_vec2, dsptb_vec2) {
        auto& s = c(x);
        if (tex == 0 || std::find(s.textures_.begin(), s.textures_.end(), tex) == s.textures_.end())
            s.fail("image with an invalid texture");
        s.draw("image");
    }
    static void pushClip(void* x, dsptb_vec2, dsptb_vec2) {
        auto& s = c(x);
        ++s.calls_;
        ++s.clipDepth_;
    }
    static void popClip(void* x) {
        auto& s = c(x);
        ++s.calls_;
        if (s.clipDepth_ == 0)
            s.fail("pop_clip without push_clip");
        else
            --s.clipDepth_;
    }
    static void pushId(void* x, const char* id) {
        auto& s = c(x);
        ++s.calls_;
        if (!id)
            s.fail("push_id with a null id");
        ++s.idDepth_;
    }
    static void popId(void* x) {
        auto& s = c(x);
        ++s.calls_;
        if (s.idDepth_ == 0)
            s.fail("pop_id without push_id");
        else
            --s.idDepth_;
    }
    static void sameLine(void* x) { c(x).widget("same_line"); }
    static void separator(void* x) { c(x).widget("separator"); }
    static void label(void* x, const char* t) {
        if (!t)
            c(x).fail("label with a null string");
        c(x).widget("label");
    }
    static int button(void* x, const char* t) {
        if (!t)
            c(x).fail("button with a null label");
        c(x).widget("button");
        return 0;
    }
    static int checkbox(void* x, const char* t, int* v) {
        if (!t || !v)
            c(x).fail("checkbox with a null argument");
        c(x).widget("checkbox");
        return 0;
    }
    static int sliderFloat(void* x, const char* t, float* v, float mn, float mx, const char*) {
        if (!t || !v)
            c(x).fail("slider_float with a null argument");
        if (!(mn < mx))
            c(x).fail("slider_float with min >= max");
        c(x).widget("slider_float");
        return 0;
    }
    static int dragFloat3(void* x, const char* t, float* v, float, float, float, const char*) {
        if (!t || !v)
            c(x).fail("drag_float3 with a null argument");
        c(x).widget("drag_float3");
        return 0;
    }
    static int inputFloat(void* x, const char* t, float* v, const char*) {
        if (!t || !v)
            c(x).fail("input_float with a null argument");
        c(x).widget("input_float");
        return 0;
    }
    static int combo(void* x, const char* t, int* cur, const char* const* items, uint32_t n) {
        if (!t || !cur || (!items && n > 0))
            c(x).fail("combo with a null argument");
        else if (*cur < 0 || (n > 0 && static_cast<uint32_t>(*cur) >= n))
            c(x).fail("combo with a current index out of range");
        c(x).widget("combo");
        return 0;
    }
    static int selectable(void* x, const char* t, int) {
        if (!t)
            c(x).fail("selectable with a null label");
        c(x).widget("selectable");
        return 0;
    }
    static int beginChild(void* x, const char* id, float) {
        auto& s = c(x);
        s.widget("begin_child");
        if (!id)
            s.fail("begin_child with a null id");
        ++s.childDepth_;
        return 1;
    }
    static void endChild(void* x) {
        auto& s = c(x);
        s.widget("end_child");
        if (s.childDepth_ == 0)
            s.fail("end_child without begin_child");
        else
            --s.childDepth_;
    }
    static int paramWidget(void* x, uint32_t) {
        c(x).widget("param_widget");
        return 0;
    }
    static int beginColumns(void* x, const char* id, uint32_t n) {
        auto& s = c(x);
        s.widget("begin_columns");
        if (!id || n == 0)
            s.fail("begin_columns with a null id or zero columns");
        ++s.columnDepth_;
        return 1;
    }
    static void nextColumn(void* x) {
        auto& s = c(x);
        s.widget("next_column");
        if (s.columnDepth_ == 0)
            s.fail("next_column outside columns");
    }
    static void endColumns(void* x) {
        auto& s = c(x);
        s.widget("end_columns");
        if (s.columnDepth_ == 0)
            s.fail("end_columns without begin_columns");
        else
            --s.columnDepth_;
    }
    static dsptb_texture textureCreate(void* x, uint32_t w, uint32_t h, uint32_t format) {
        auto& s = c(x);
        ++s.calls_;
        if (w == 0 || h == 0 || format != DSPTB_TEX_RGBA8) {
            s.fail("texture_create with a bad size or format");
            return 0;
        }
        s.textures_.push_back(s.nextTexture_);
        return s.nextTexture_++;
    }
    static void textureUpdate(void* x, dsptb_texture t, uint32_t, uint32_t, uint32_t w, uint32_t h,
                              const void* px, uint32_t pitch) {
        auto& s = c(x);
        ++s.calls_;
        if (std::find(s.textures_.begin(), s.textures_.end(), t) == s.textures_.end())
            s.fail("texture_update on an invalid texture");
        if (!px || w == 0 || h == 0 || pitch < w * 4)
            s.fail("texture_update with bad pixel data");
    }
    static void textureDestroy(void* x, dsptb_texture t) {
        auto& s = c(x);
        ++s.calls_;
        const auto it = std::find(s.textures_.begin(), s.textures_.end(), t);
        if (it == s.textures_.end())
            s.fail("texture_destroy on an invalid texture");
        else
            s.textures_.erase(it);
    }
};

RecordingCanvas::RecordingCanvas() {
    api_.struct_size = sizeof(api_);
    api_.ctx = this;
    api_.ui_scale = &CanvasThunks::uiScale;
    api_.available_size = &CanvasThunks::available;
    api_.begin_region = &CanvasThunks::beginRegion;
    api_.end_region = &CanvasThunks::endRegion;
    api_.mouse = &CanvasThunks::mouse;
    api_.line = &CanvasThunks::line;
    api_.rect = &CanvasThunks::rect;
    api_.circle = &CanvasThunks::circle;
    api_.polyline = &CanvasThunks::polyline;
    api_.text = &CanvasThunks::text;
    api_.image = &CanvasThunks::image;
    api_.push_clip = &CanvasThunks::pushClip;
    api_.pop_clip = &CanvasThunks::popClip;
    api_.push_id = &CanvasThunks::pushId;
    api_.pop_id = &CanvasThunks::popId;
    api_.same_line = &CanvasThunks::sameLine;
    api_.separator = &CanvasThunks::separator;
    api_.label = &CanvasThunks::label;
    api_.button = &CanvasThunks::button;
    api_.checkbox = &CanvasThunks::checkbox;
    api_.slider_float = &CanvasThunks::sliderFloat;
    api_.drag_float3 = &CanvasThunks::dragFloat3;
    api_.input_float = &CanvasThunks::inputFloat;
    api_.combo = &CanvasThunks::combo;
    api_.selectable = &CanvasThunks::selectable;
    api_.begin_child = &CanvasThunks::beginChild;
    api_.end_child = &CanvasThunks::endChild;
    api_.param_widget = &CanvasThunks::paramWidget;
    api_.begin_columns = &CanvasThunks::beginColumns;
    api_.next_column = &CanvasThunks::nextColumn;
    api_.end_columns = &CanvasThunks::endColumns;
    api_.texture_create = &CanvasThunks::textureCreate;
    api_.texture_update = &CanvasThunks::textureUpdate;
    api_.texture_destroy = &CanvasThunks::textureDestroy;
}

void RecordingCanvas::fail(const char* what) {
    if (problem_.empty())
        problem_ = what;
}

void RecordingCanvas::draw(const char* what) {
    ++calls_;
    ++drawCalls_;
    if (regionDepth_ == 0)
        fail((std::string(what) + " outside a region").c_str());
}

bool RecordingCanvas::finishFrame(std::string& problem) {
    if (problem_.empty()) {
        if (regionDepth_ != 0)
            problem_ = "begin_region without end_region";
        else if (idDepth_ != 0)
            problem_ = "push_id without pop_id";
        else if (clipDepth_ != 0)
            problem_ = "push_clip without pop_clip";
        else if (childDepth_ != 0)
            problem_ = "begin_child without end_child";
        else if (columnDepth_ != 0)
            problem_ = "begin_columns without end_columns";
    }
    problem = problem_;
    const bool ok = problem_.empty();
    problem_.clear();
    regionDepth_ = idDepth_ = clipDepth_ = childDepth_ = columnDepth_ = 0;
    return ok;
}

} // namespace dsptb
