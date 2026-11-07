#pragma once

// Module-system UI shared by the shell's translation units: browser, chain, instance panels, parameter
// controls, undo/redo menu and shortcuts. Not part of the public UI API.

#include "shell.hpp"

#include <dsptb/core/session_model.hpp>

#include <imgui.h>

#include <Windows.h>

#include <cstdint>
#include <map>
#include <string>

namespace dsptb::ui {

// Draws one parameter's standard control: slider/checkbox/combo with the unit, double-click or
// Ctrl+click to reset to the default, right-click to type a value; edits go through the SessionModel's undo
// history. Returns true when the value changed.
bool drawParamControl(ShellContext& ctx, dsptb::InstanceRecord& rec, uint32_t index);

// Starts the instance's offline module on the loaded file. False when there's no file or it's already running.
bool startOfflineRun(ShellContext& ctx, dsptb::InstanceId id);
// Cancels every offline run and waits for them. Call before the session model goes away.
void stopOfflineRuns(ShellContext& ctx);

void drawModuleBrowserPanel(ShellContext& ctx);
void drawChainPanel(ShellContext& ctx);
void drawInstancePanels(ShellContext& ctx);
// Edit menu (Undo/Redo) and the Modules menu entries that act on the registry.
void drawEditMenu(ShellContext& ctx);
void drawModulesMenuItems(ShellContext& ctx);
// "Install from .zip...": the trust warning, then the file picker and install. The picker is a modal system
// dialog, so processModuleInstall runs between frames.
void requestModuleInstall();
void drawInstallDialog();
void processModuleInstall(ShellContext& ctx, HWND owner);
// Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z.
void handleModuleShortcuts(ShellContext& ctx);

// Adds an instance of `moduleId` and reports problems as a banner. Returns its id, or 0.
InstanceId addModuleInstance(ShellContext& ctx, const std::string& moduleId);

// shell.cpp: the dock node that module panels open in.
ImGuiID centralDockId();

} // namespace dsptb::ui
