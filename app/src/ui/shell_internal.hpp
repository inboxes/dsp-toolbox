#pragma once

// Helpers shared by the shell's translation units (shell.cpp, audio_ui.cpp). Not part of the public UI API.

#include "shell.hpp"

namespace dsptb::ui {

// Current UI scale (monitor DPI x user scale), for sizes that are not covered by ImGui's style scaling.
float dpiScale();

// Begins a dockable panel with the standard context menu. Returns true if the contents should be drawn; the
// caller must then call ImGui::End().
bool beginPanel(ShellContext& ctx, int panel);

// Pushes a banner using ImGui's clock.
void notify(ShellContext& ctx, dsptb::NoticeLevel level, const std::string& text);

// audio_ui.cpp
void drawTransportPanel(ShellContext& ctx);
void drawAudioSettingsWindow(ShellContext& ctx);
void drawAudioStatusFields(ShellContext& ctx); // inside the status bar's menu bar
void handleAudioShortcuts(ShellContext& ctx);
void applyAudioSettingsFromConfig(ShellContext& ctx); // copies the active AudioConfig into Settings::audio

} // namespace dsptb::ui
