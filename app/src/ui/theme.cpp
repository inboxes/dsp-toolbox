#include "theme.hpp"

#include <dsptb/core/paths.hpp>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <filesystem>

namespace dsptb::ui {

namespace {
constexpr float kBaseFontPx = 16.0f;
}

bool loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const auto fonts = systemFontsDir();
    bool loaded = false;
    if (!fonts.empty()) {
        const auto segoe = fonts / "segoeui.ttf";
        std::error_code ec;
        if (std::filesystem::exists(segoe, ec))
            loaded = io.Fonts->AddFontFromFileTTF(segoe.string().c_str(), kBaseFontPx) != nullptr;
    }
    if (!loaded) {
        spdlog::warn("Segoe UI not found; using the built-in ImGui font");
        io.Fonts->AddFontDefault();
    }

    // Merge Segoe UI Symbol so the standard Unicode symbols for play, pause, stop, loop, folder and speaker
    // exist (Segoe UI itself lacks them). Buttons fall back to text when a glyph is missing.
    if (!fonts.empty()) {
        const auto symbols = fonts / "seguisym.ttf";
        std::error_code ec;
        if (std::filesystem::exists(symbols, ec)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            if (!io.Fonts->AddFontFromFileTTF(symbols.string().c_str(), kBaseFontPx, &cfg))
                spdlog::warn("Segoe UI Symbol could not be loaded; buttons will show text instead of icons");
        } else {
            spdlog::warn("Segoe UI Symbol not found; buttons will show text instead of icons");
        }
    }

    // CJK fallbacks, so file names in Japanese, Chinese or Korean show up instead of '?'. Merged after the
    // symbol font; ImGui takes each glyph from the first font that has it and loads glyphs on demand, so the
    // large files cost nothing until a character is used. Meiryo goes first because it covers kana and kanji.
    if (!fonts.empty()) {
        for (const char* name : {"meiryo.ttc", "msyh.ttc", "malgun.ttf"}) {
            std::error_code ec;
            const auto path = fonts / name;
            if (!std::filesystem::exists(path, ec))
                continue;
            ImFontConfig cfg;
            cfg.MergeMode = true;
            if (!io.Fonts->AddFontFromFileTTF(toUtf8(path).c_str(), kBaseFontPx, &cfg))
                spdlog::warn("{} could not be loaded; some characters may show as '?'", name);
        }
    }
    return loaded;
}

void applyTheme(float scale) {
    ImGuiStyle base;
    ImGui::StyleColorsDark(&base);
    base.FontSizeBase = kBaseFontPx;

    base.WindowRounding = 0.0f; // docked panels meet edge to edge
    base.ChildRounding = 4.0f;
    base.FrameRounding = 4.0f;
    base.PopupRounding = 4.0f;
    base.TabRounding = 4.0f;
    base.GrabRounding = 4.0f;
    base.ScrollbarRounding = 6.0f;
    base.WindowPadding = ImVec2(10, 8);
    base.FramePadding = ImVec2(8, 5);
    base.ItemSpacing = ImVec2(8, 6);
    base.WindowBorderSize = 1.0f;
    base.FrameBorderSize = 0.0f;
    base.TabBarOverlineSize = 0.0f;

    ImVec4* c = base.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.112f, 0.125f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.097f, 0.109f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.125f, 0.133f, 0.148f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.082f, 0.088f, 0.100f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.082f, 0.088f, 0.100f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.110f, 0.120f, 0.138f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.145f, 0.157f, 0.176f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.118f, 0.128f, 0.145f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.36f, 0.58f, 1.0f);
    c[ImGuiCol_TabDimmed] = ImVec4(0.100f, 0.108f, 0.122f, 1.0f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.16f, 0.26f, 0.40f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.58f, 0.55f);
    c[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.59f, 0.98f, 0.45f);

    base.ScaleAllSizes(scale);
    base.FontScaleDpi = scale;
    ImGui::GetStyle() = base;
}

} // namespace dsptb::ui
