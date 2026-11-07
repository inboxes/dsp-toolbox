#include "canvas_impl.hpp"

#include "module_ui.hpp"
#include "shell_internal.hpp"

#include <imgui.h>

#include <algorithm>

namespace dsptb::ui {

namespace {
constexpr ImVec2 toIm(dsptb_vec2 v) {
    return ImVec2(v.x, v.y);
}
} // namespace

// The C callbacks. `ctx` is the CanvasHost.
struct CanvasCallbacks {
    static CanvasHost& h(void* ctx) { return *static_cast<CanvasHost*>(ctx); }
    static ImVec2 org(CanvasHost& c, dsptb_vec2 v) { return ImVec2(c.originX_ + v.x, c.originY_ + v.y); }

    static float uiScale(void*) { return dpiScale(); }
    static dsptb_vec2 availableSize(void*) {
        const ImVec2 a = ImGui::GetContentRegionAvail();
        return {a.x, a.y};
    }

    static int beginRegion(void* ctx, const char* id, float w, float hgt) {
        CanvasHost& c = h(ctx);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 size(std::max(w, 1.0f), std::max(hgt, 1.0f));
        ImGui::InvisibleButton(id ? id : "##region", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        c.originX_ = p.x;
        c.originY_ = p.y;
        c.regionW_ = size.x;
        c.regionH_ = size.y;
        c.regionHovered_ = ImGui::IsItemHovered();
        c.regionActive_ = ImGui::IsItemActive();
        c.drawList_ = ImGui::GetWindowDrawList();
        c.drawList_->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
        ++c.regionDepth_;
        return ImGui::IsRectVisible(p, ImVec2(p.x + size.x, p.y + size.y)) ? 1 : 0;
    }
    static void endRegion(void* ctx) {
        CanvasHost& c = h(ctx);
        if (c.regionDepth_ > 0 && c.drawList_) {
            c.drawList_->PopClipRect();
            --c.regionDepth_;
        }
    }
    static void mouse(void* ctx, dsptb_mouse* out) {
        CanvasHost& c = h(ctx);
        if (!out)
            return;
        const ImGuiIO& io = ImGui::GetIO();
        const uint32_t size = out->struct_size;
        *out = dsptb_mouse{};
        out->struct_size = size;
        out->pos = {io.MousePos.x - c.originX_, io.MousePos.y - c.originY_};
        out->hovered = c.regionHovered_ ? 1 : 0;
        out->active = c.regionActive_ ? 1 : 0;
        if (c.regionActive_ && io.MouseDown[0])
            out->drag_delta = {io.MouseDelta.x, io.MouseDelta.y};
        if (c.regionHovered_)
            out->wheel = io.MouseWheel;
        for (int b = 0; b < 3; ++b) {
            out->down[b] = (c.regionHovered_ || c.regionActive_) && io.MouseDown[b] ? 1 : 0;
            out->clicked[b] = c.regionHovered_ && ImGui::IsMouseClicked(b) ? 1 : 0;
            out->released[b] = (c.regionHovered_ || c.regionActive_) && ImGui::IsMouseReleased(b) ? 1 : 0;
            out->double_clicked[b] = c.regionHovered_ && ImGui::IsMouseDoubleClicked(b) ? 1 : 0;
        }
        out->ctrl = io.KeyCtrl ? 1 : 0;
        out->shift = io.KeyShift ? 1 : 0;
        out->alt = io.KeyAlt ? 1 : 0;
    }

    // ---- drawing: region-local coordinates ----
    static void line(void* ctx, dsptb_vec2 a, dsptb_vec2 b, uint32_t col, float t) {
        CanvasHost& c = h(ctx);
        if (c.drawList_)
            c.drawList_->AddLine(org(c, a), org(c, b), col, t);
    }
    static void rect(void* ctx, dsptb_vec2 a, dsptb_vec2 b, uint32_t col, float t, int filled) {
        CanvasHost& c = h(ctx);
        if (!c.drawList_)
            return;
        if (filled)
            c.drawList_->AddRectFilled(org(c, a), org(c, b), col);
        else
            c.drawList_->AddRect(org(c, a), org(c, b), col, 0.0f, 0, t);
    }
    static void circle(void* ctx, dsptb_vec2 ce, float r, uint32_t col, float t, int filled) {
        CanvasHost& c = h(ctx);
        if (!c.drawList_)
            return;
        if (filled)
            c.drawList_->AddCircleFilled(org(c, ce), r, col);
        else
            c.drawList_->AddCircle(org(c, ce), r, col, 0, t);
    }
    static void polyline(void* ctx, const dsptb_vec2* pts, uint32_t n, uint32_t col, float t, int closed) {
        CanvasHost& c = h(ctx);
        if (!c.drawList_ || !pts || n < 2)
            return;
        std::vector<ImVec2> v(n);
        for (uint32_t i = 0; i < n; ++i)
            v[i] = org(c, pts[i]);
        c.drawList_->AddPolyline(v.data(), static_cast<int>(n), col, closed ? ImDrawFlags_Closed : 0, t);
    }
    static void text(void* ctx, dsptb_vec2 pos, uint32_t col, const char* utf8) {
        CanvasHost& c = h(ctx);
        if (c.drawList_ && utf8)
            c.drawList_->AddText(org(c, pos), col, utf8);
    }
    static void image(void* ctx, dsptb_texture tex, dsptb_vec2 a, dsptb_vec2 b, dsptb_vec2 uv0,
                      dsptb_vec2 uv1) {
        CanvasHost& c = h(ctx);
        const auto it = c.textures_.find(tex);
        if (!c.drawList_ || it == c.textures_.end())
            return;
        c.drawList_->AddImage(reinterpret_cast<ImTextureID>(it->second.srv.Get()), org(c, a), org(c, b),
                              toIm(uv0), toIm(uv1));
    }
    static void pushClip(void* ctx, dsptb_vec2 a, dsptb_vec2 b) {
        CanvasHost& c = h(ctx);
        if (c.drawList_)
            c.drawList_->PushClipRect(org(c, a), org(c, b), true);
    }
    static void popClip(void* ctx) {
        CanvasHost& c = h(ctx);
        if (c.drawList_)
            c.drawList_->PopClipRect();
    }

    // ---- widgets ----
    static void pushId(void*, const char* id) { ImGui::PushID(id ? id : ""); }
    static void popId(void*) { ImGui::PopID(); }
    static void sameLine(void*) { ImGui::SameLine(); }
    static void separator(void*) { ImGui::Separator(); }
    static void label(void*, const char* t) { ImGui::TextUnformatted(t ? t : ""); }
    static int button(void*, const char* t) { return ImGui::Button(t ? t : "##button") ? 1 : 0; }
    static int checkbox(void*, const char* t, int* v) {
        bool b = v && *v != 0;
        const bool changed = ImGui::Checkbox(t ? t : "##check", &b);
        if (v)
            *v = b ? 1 : 0;
        return changed ? 1 : 0;
    }
    static int sliderFloat(void*, const char* t, float* v, float mn, float mx, const char* fmt) {
        return v && ImGui::SliderFloat(t ? t : "##slider", v, mn, mx, fmt ? fmt : "%.3f") ? 1 : 0;
    }
    static int dragFloat3(void*, const char* t, float* v, float speed, float mn, float mx, const char* fmt) {
        return v && ImGui::DragFloat3(t ? t : "##drag3", v, speed, mn, mx, fmt ? fmt : "%.2f") ? 1 : 0;
    }
    static int inputFloat(void*, const char* t, float* v, const char* fmt) {
        return v && ImGui::InputFloat(t ? t : "##input", v, 0.0f, 0.0f, fmt ? fmt : "%.3f") ? 1 : 0;
    }
    static int combo(void*, const char* t, int* cur, const char* const* items, uint32_t n) {
        return cur && items && ImGui::Combo(t ? t : "##combo", cur, items, static_cast<int>(n)) ? 1 : 0;
    }
    static int selectable(void*, const char* t, int selected) {
        return ImGui::Selectable(t ? t : "##sel", selected != 0) ? 1 : 0;
    }
    static int beginChild(void*, const char* id, float height) {
        return ImGui::BeginChild(id ? id : "##child", ImVec2(0.0f, height), ImGuiChildFlags_Borders) ? 1 : 0;
    }
    static void endChild(void*) { ImGui::EndChild(); }
    static int paramWidget(void* ctx, uint32_t index) {
        CanvasHost& c = h(ctx);
        return (c.shell_ && c.rec_ && drawParamControl(*c.shell_, *c.rec_, index)) ? 1 : 0;
    }
    static int beginColumns(void*, const char* id, uint32_t n) {
        ImGui::Columns(static_cast<int>(std::max(1u, n)), id, true);
        return 1;
    }
    static void nextColumn(void*) { ImGui::NextColumn(); }
    static void endColumns(void*) { ImGui::Columns(1); }

    // ---- textures ----
    static dsptb_texture textureCreate(void* ctx, uint32_t w, uint32_t hgt, uint32_t format) {
        CanvasHost& c = h(ctx);
        if (w == 0 || hgt == 0 || format != DSPTB_TEX_RGBA8 || w > 8192 || hgt > 8192 || !c.rec_)
            return 0;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = hgt;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CanvasHost::Texture t;
        if (FAILED(c.device_->CreateTexture2D(&d, nullptr, t.tex.GetAddressOf())) ||
            FAILED(c.device_->CreateShaderResourceView(t.tex.Get(), nullptr, t.srv.GetAddressOf())))
            return 0;
        t.owner = c.rec_->id;
        t.width = w;
        t.height = hgt;
        const uint64_t handle = c.nextTexture_++;
        c.textures_[handle] = std::move(t);
        return handle;
    }
    static void textureUpdate(void* ctx, dsptb_texture tex, uint32_t x, uint32_t y, uint32_t w, uint32_t hgt,
                              const void* pixels, uint32_t pitch) {
        CanvasHost& c = h(ctx);
        const auto it = c.textures_.find(tex);
        if (it == c.textures_.end() || !pixels || w == 0 || hgt == 0)
            return;
        const auto& t = it->second;
        if (x + w > t.width || y + hgt > t.height)
            return;
        const D3D11_BOX box{x, y, 0, x + w, y + hgt, 1};
        c.context_->UpdateSubresource(t.tex.Get(), 0, &box, pixels, pitch, 0);
    }
    static void textureDestroy(void* ctx, dsptb_texture tex) { h(ctx).textures_.erase(tex); }
};

CanvasHost::CanvasHost(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context) {
    api_.struct_size = sizeof(api_);
    api_.ctx = this;
    api_.ui_scale = &CanvasCallbacks::uiScale;
    api_.available_size = &CanvasCallbacks::availableSize;
    api_.begin_region = &CanvasCallbacks::beginRegion;
    api_.end_region = &CanvasCallbacks::endRegion;
    api_.mouse = &CanvasCallbacks::mouse;
    api_.line = &CanvasCallbacks::line;
    api_.rect = &CanvasCallbacks::rect;
    api_.circle = &CanvasCallbacks::circle;
    api_.polyline = &CanvasCallbacks::polyline;
    api_.text = &CanvasCallbacks::text;
    api_.image = &CanvasCallbacks::image;
    api_.push_clip = &CanvasCallbacks::pushClip;
    api_.pop_clip = &CanvasCallbacks::popClip;
    api_.push_id = &CanvasCallbacks::pushId;
    api_.pop_id = &CanvasCallbacks::popId;
    api_.same_line = &CanvasCallbacks::sameLine;
    api_.separator = &CanvasCallbacks::separator;
    api_.label = &CanvasCallbacks::label;
    api_.button = &CanvasCallbacks::button;
    api_.checkbox = &CanvasCallbacks::checkbox;
    api_.slider_float = &CanvasCallbacks::sliderFloat;
    api_.drag_float3 = &CanvasCallbacks::dragFloat3;
    api_.input_float = &CanvasCallbacks::inputFloat;
    api_.combo = &CanvasCallbacks::combo;
    api_.selectable = &CanvasCallbacks::selectable;
    api_.begin_child = &CanvasCallbacks::beginChild;
    api_.end_child = &CanvasCallbacks::endChild;
    api_.param_widget = &CanvasCallbacks::paramWidget;
    api_.begin_columns = &CanvasCallbacks::beginColumns;
    api_.next_column = &CanvasCallbacks::nextColumn;
    api_.end_columns = &CanvasCallbacks::endColumns;
    api_.texture_create = &CanvasCallbacks::textureCreate;
    api_.texture_update = &CanvasCallbacks::textureUpdate;
    api_.texture_destroy = &CanvasCallbacks::textureDestroy;
}

CanvasHost::~CanvasHost() = default;

const dsptb_canvas_api* CanvasHost::begin(ShellContext& shell, InstanceRecord& rec) {
    shell_ = &shell;
    rec_ = &rec;
    drawList_ = ImGui::GetWindowDrawList();
    regionDepth_ = 0;
    ImGui::PushID(static_cast<int>(rec.id)); // every instance gets its own id scope
    return &api_;
}

void CanvasHost::end() {
    // A module that forgot end_region must not leave a clip rect pushed.
    while (regionDepth_ > 0 && drawList_) {
        drawList_->PopClipRect();
        --regionDepth_;
    }
    ImGui::PopID();
    shell_ = nullptr;
    rec_ = nullptr;
    drawList_ = nullptr;
}

void CanvasHost::retainInstances(const std::vector<InstanceId>& live) {
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (std::find(live.begin(), live.end(), it->second.owner) == live.end())
            it = textures_.erase(it);
        else
            ++it;
    }
}

} // namespace dsptb::ui
