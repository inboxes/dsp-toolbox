#pragma once

// Thin C++ wrapper over dsptb_canvas_api (MODULE_SDK §3). UI thread only. Scopes (region, id, clip) are RAII
// so a module cannot leave them unbalanced on an early return.

#include "plugin_abi.h"

#include <cstdint>

namespace dsptb {

// ImGui IM_COL32 layout: 0xAABBGGRR.
constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) noexcept {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

class Canvas {
public:
    explicit Canvas(const dsptb_canvas_api* api) : api_(api) {}

    float uiScale() const { return api_->ui_scale(api_->ctx); }
    dsptb_vec2 availableSize() const { return api_->available_size(api_->ctx); }
    dsptb_mouse mouse() const {
        dsptb_mouse m{};
        m.struct_size = sizeof(m);
        api_->mouse(api_->ctx, &m);
        return m;
    }

    // Custom drawing region. Draw only when visible() is true; the region ends on destruction.
    class Region {
    public:
        Region(const dsptb_canvas_api* api, const char* id, float w, float h)
            : api_(api), visible_(api->begin_region(api->ctx, id, w, h) != 0) {}
        ~Region() { api_->end_region(api_->ctx); }
        Region(const Region&) = delete;
        Region& operator=(const Region&) = delete;
        bool visible() const { return visible_; }
        explicit operator bool() const { return visible_; }

    private:
        const dsptb_canvas_api* api_;
        bool visible_;
    };
    Region region(const char* id, float w, float h) const { return Region(api_, id, w, h); }

    class IdScope {
    public:
        IdScope(const dsptb_canvas_api* api, const char* id) : api_(api) { api->push_id(api->ctx, id); }
        ~IdScope() { api_->pop_id(api_->ctx); }
        IdScope(const IdScope&) = delete;
        IdScope& operator=(const IdScope&) = delete;

    private:
        const dsptb_canvas_api* api_;
    };
    IdScope idScope(const char* id) const { return IdScope(api_, id); }

    class ClipScope {
    public:
        ClipScope(const dsptb_canvas_api* api, dsptb_vec2 a, dsptb_vec2 b) : api_(api) {
            api->push_clip(api->ctx, a, b);
        }
        ~ClipScope() { api_->pop_clip(api_->ctx); }
        ClipScope(const ClipScope&) = delete;
        ClipScope& operator=(const ClipScope&) = delete;

    private:
        const dsptb_canvas_api* api_;
    };
    ClipScope clip(dsptb_vec2 a, dsptb_vec2 b) const { return ClipScope(api_, a, b); }

    // drawing (region-local coordinates)
    void line(dsptb_vec2 a, dsptb_vec2 b, uint32_t col, float thickness = 1.0f) const {
        api_->line(api_->ctx, a, b, col, thickness);
    }
    void rect(dsptb_vec2 a, dsptb_vec2 b, uint32_t col, float thickness = 1.0f, bool filled = false) const {
        api_->rect(api_->ctx, a, b, col, thickness, filled ? 1 : 0);
    }
    void fillRect(dsptb_vec2 a, dsptb_vec2 b, uint32_t col) const { rect(a, b, col, 1.0f, true); }
    void circle(dsptb_vec2 c, float r, uint32_t col, float thickness = 1.0f, bool filled = false) const {
        api_->circle(api_->ctx, c, r, col, thickness, filled ? 1 : 0);
    }
    void polyline(const dsptb_vec2* pts, uint32_t n, uint32_t col, float thickness = 1.0f,
                  bool closed = false) const {
        api_->polyline(api_->ctx, pts, n, col, thickness, closed ? 1 : 0);
    }
    void text(dsptb_vec2 pos, uint32_t col, const char* utf8) const { api_->text(api_->ctx, pos, col, utf8); }
    void image(dsptb_texture tex, dsptb_vec2 a, dsptb_vec2 b, dsptb_vec2 uv0 = {0, 0},
               dsptb_vec2 uv1 = {1, 1}) const {
        api_->image(api_->ctx, tex, a, b, uv0, uv1);
    }

    // widgets
    void sameLine() const { api_->same_line(api_->ctx); }
    void separator() const { api_->separator(api_->ctx); }
    void label(const char* utf8) const { api_->label(api_->ctx, utf8); }
    bool button(const char* label) const { return api_->button(api_->ctx, label) != 0; }
    bool checkbox(const char* label, bool& value) const {
        int v = value ? 1 : 0;
        const bool changed = api_->checkbox(api_->ctx, label, &v) != 0;
        value = v != 0;
        return changed;
    }
    bool sliderFloat(const char* label, float& v, float mn, float mx, const char* fmt = "%.3f") const {
        return api_->slider_float(api_->ctx, label, &v, mn, mx, fmt) != 0;
    }
    bool dragFloat3(const char* label, float v[3], float speed, float mn, float mx,
                    const char* fmt = "%.2f") const {
        return api_->drag_float3(api_->ctx, label, v, speed, mn, mx, fmt) != 0;
    }
    bool inputFloat(const char* label, float& v, const char* fmt = "%.3f") const {
        return api_->input_float(api_->ctx, label, &v, fmt) != 0;
    }
    bool combo(const char* label, int& current, const char* const* items, uint32_t count) const {
        return api_->combo(api_->ctx, label, &current, items, count) != 0;
    }
    bool selectable(const char* label, bool selected) const {
        return api_->selectable(api_->ctx, label, selected ? 1 : 0) != 0;
    }
    bool paramWidget(uint32_t index) const { return api_->param_widget(api_->ctx, index) != 0; }

    // textures (freed by the host together with the instance)
    dsptb_texture textureCreate(uint32_t w, uint32_t h) const {
        return api_->texture_create(api_->ctx, w, h, DSPTB_TEX_RGBA8);
    }
    void textureUpdate(dsptb_texture t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* px,
                       uint32_t pitch) const {
        api_->texture_update(api_->ctx, t, x, y, w, h, px, pitch);
    }
    void textureDestroy(dsptb_texture t) const { api_->texture_destroy(api_->ctx, t); }

    const dsptb_canvas_api* raw() const { return api_; }

private:
    const dsptb_canvas_api* api_;
};

} // namespace dsptb
