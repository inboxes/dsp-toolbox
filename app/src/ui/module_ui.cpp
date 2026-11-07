#include "module_ui.hpp"

#include "canvas_impl.hpp"
#include "file_dialog.hpp"
#include "shell_internal.hpp"

#include <dsptb/core/module_install.hpp>
#include <dsptb/core/paths.hpp>

#include <imgui_internal.h> // ClearActiveID

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace dsptb::ui {

namespace {

const char* kChainPayload = "DSPTB_CHAIN_ROW";

uint64_t editKey(InstanceId id, uint32_t index) {
    return (static_cast<uint64_t>(id) << 16) | index;
}

std::string latencyText(const ShellContext& ctx, uint32_t frames) {
    const uint32_t sr = ctx.audio ? ctx.audio->engine().sampleRate() : 0;
    char buf[64];
    if (sr > 0)
        std::snprintf(buf, sizeof buf, "%u frames (%.1f ms)", frames, 1000.0 * frames / sr);
    else
        std::snprintf(buf, sizeof buf, "%u frames", frames);
    return buf;
}

// "%" inside a unit would be read as a format specifier by ImGui.
std::string escapePercent(const char* s) {
    std::string out;
    for (; *s; ++s) {
        out += *s;
        if (*s == '%')
            out += '%';
    }
    return out;
}

// The user's label if there is one, otherwise the module's name.
std::string instanceName(const InstanceRecord& rec) {
    return rec.label.empty() ? std::string(rec.instance->desc().name) : rec.label;
}

void startRename(ShellContext& ctx, const InstanceRecord& rec) {
    ModuleUiState& ui = ctx.moduleUi;
    ui.renameId = rec.id;
    ui.renameFocus = true;
    std::snprintf(ui.renameBuf, sizeof ui.renameBuf, "%s", rec.label.c_str());
}

// A small window for typing a label; Enter or OK applies, Esc or Cancel discards.
void drawRenameWindow(ShellContext& ctx) {
    ModuleUiState& ui = ctx.moduleUi;
    if (ui.renameId == 0)
        return;
    const InstanceRecord* rec = ctx.model->find(ui.renameId);
    if (!rec) {
        ui.renameId = 0;
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter()), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("Rename module", &open,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("%s (%s)", rec->instance->desc().name, "label shown in the chain");
        ImGui::SetNextItemWidth(260.0f * dpiScale());
        if (ui.renameFocus) {
            ImGui::SetKeyboardFocusHere();
            ui.renameFocus = false;
        }
        const bool enter = ImGui::InputText("##label", ui.renameBuf, sizeof ui.renameBuf,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = ImGui::Button("OK") || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::SameLine();
        ImGui::TextDisabled("Empty = use the module name");
        if (ok) {
            ctx.model->setLabel(ui.renameId, ui.renameBuf);
            open = false;
        } else if (cancel) {
            open = false;
        }
    }
    ImGui::End();
    if (!open)
        ui.renameId = 0;
}

} // namespace

// ----------------------------------------------------------------------------------------------------------
// parameter controls
// ----------------------------------------------------------------------------------------------------------

bool drawParamControl(ShellContext& ctx, InstanceRecord& rec, uint32_t index) {
    ParamBlock& pb = rec.instance->params();
    if (index >= pb.size() || !ctx.model)
        return false;
    const dsptb_param_desc& d = pb.desc(index);
    ModuleUiState& ui = ctx.moduleUi;
    const uint64_t key = editKey(rec.id, index);
    bool changed = false;

    // Optional help text from the module manifest (paramHelp), shown as a tooltip.
    // A reference into the manifest map (stable for the life of the package): no string copy per frame.
    static const std::string kNoHelp;
    const std::string* helpPtr = &kNoHelp;
    if (ctx.registry) {
        if (const ModulePackage* pkg = ctx.registry->findByModule(rec.instance->moduleId())) {
            if (!pkg->manifest.paramHelp.empty()) {
                const auto it = pkg->manifest.paramHelp.find(d.id ? d.id : "");
                if (it != pkg->manifest.paramHelp.end())
                    helpPtr = &it->second;
            }
        }
    }
    const std::string& help = *helpPtr;

    ImGui::PushID(static_cast<int>(index));
    ImGui::SetNextItemWidth(std::max(80.0f * dpiScale(), ImGui::GetContentRegionAvail().x * 0.60f));

    const auto resetRequested = [&] {
        const ImGuiIO& io = ImGui::GetIO();
        return ImGui::IsItemHovered() && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                                          (io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)));
    };

    switch (d.type) {
    case DSPTB_PARAM_FLOAT:
    case DSPTB_PARAM_INT: {
        const float before = pb.get(index);
        const std::string unit = d.unit && *d.unit ? " " + escapePercent(d.unit) : "";
        bool edited = false;
        if (d.type == DSPTB_PARAM_FLOAT) {
            float v = before;
            const std::string fmt = "%.2f" + unit;
            ImGuiSliderFlags flags = ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp;
            if (d.flags & DSPTB_PARAM_LOG)
                flags |= ImGuiSliderFlags_Logarithmic;
            edited = ImGui::SliderFloat(d.name, &v, d.min_value, d.max_value, fmt.c_str(), flags);
            if (edited)
                ctx.model->setParam(rec.id, index, v);
        } else {
            int v = static_cast<int>(before);
            const std::string fmt = "%d" + unit;
            edited =
                ImGui::SliderInt(d.name, &v, static_cast<int>(d.min_value), static_cast<int>(d.max_value),
                                 fmt.c_str(), ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
            if (edited)
                ctx.model->setParam(rec.id, index, static_cast<float>(v));
        }
        changed = edited;
        // One undo step per drag: remember the value when the gesture starts, commit when it ends.
        if (ImGui::IsItemActivated())
            ui.editStart[key] = before;
        const bool reset = resetRequested();
        if (ImGui::IsItemDeactivated()) {
            const auto it = ui.editStart.find(key);
            if (it != ui.editStart.end()) {
                ctx.model->commitParam(rec.id, index, it->second, pb.get(index));
                ui.editStart.erase(it);
            }
        }
        if (reset) {
            ImGui::ClearActiveID();
            ctx.model->setParamUndoable(rec.id, index, d.default_value);
            changed = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ui.typedValue = pb.get(index);
            ImGui::OpenPopup("##typed");
        }
        if (ImGui::BeginPopup("##typed")) {
            ImGui::TextDisabled("%s", d.name);
            ImGui::SetNextItemWidth(120.0f * dpiScale());
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            if (ImGui::InputFloat("##value", &ui.typedValue, 0.0f, 0.0f, "%.3f",
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
                ctx.model->setParamUndoable(rec.id, index, ui.typedValue);
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s%sDouble-click or Ctrl+click: reset to %g\nRight-click: type a value",
                              help.c_str(), help.empty() ? "" : "\n\n", static_cast<double>(d.default_value));
        break;
    }
    case DSPTB_PARAM_BOOL: {
        bool b = pb.get(index) >= 0.5f;
        if (ImGui::Checkbox(d.name, &b)) {
            ctx.model->setParamUndoable(rec.id, index, b ? 1.0f : 0.0f);
            changed = true;
        }
        if (!help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", help.c_str());
        break;
    }
    case DSPTB_PARAM_ENUM: {
        int cur = static_cast<int>(pb.get(index));
        if (ImGui::Combo(d.name, &cur, d.enum_labels, static_cast<int>(d.enum_count))) {
            ctx.model->setParamUndoable(rec.id, index, static_cast<float>(cur));
            changed = true;
        }
        if (!help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", help.c_str());
        break;
    }
    }
    ImGui::PopID();
    return changed;
}

// ----------------------------------------------------------------------------------------------------------
// adding, duplicating, rescanning
// ----------------------------------------------------------------------------------------------------------

InstanceId addModuleInstance(ShellContext& ctx, const std::string& moduleId) {
    if (!ctx.model)
        return 0;
    std::string error;
    const InstanceId id = ctx.model->add(moduleId, error);
    if (id == 0) {
        notify(ctx, NoticeLevel::Error, "Could not add " + moduleId + ": " + error);
        return 0;
    }
    ctx.moduleUi.lastAdded = id;
    ctx.moduleUi.focusRequest = id;
    ctx.moduleUi.panelOpen[id] = true;
    return id;
}

namespace {

void duplicateInstance(ShellContext& ctx, const InstanceRecord& src) {
    const std::string moduleId = src.instance->moduleId();
    const InstanceId id = addModuleInstance(ctx, moduleId);
    if (id == 0)
        return;
    InstanceRecord* rec = ctx.model->find(id);
    rec->label = src.label;
    if (src.isAnalyzer())
        ctx.model->setTap(id, src.tap);
    for (uint32_t i = 0; i < src.instance->params().size(); ++i)
        rec->instance->params().set(i, src.instance->params().get(i));
    std::string state;
    if (src.instance->saveState(state)) {
        rec->instance->loadState(state);
        rec->lastState = state;
    }
}

void rescanModules(ShellContext& ctx) {
    if (!ctx.registry)
        return;
    const size_t added = ctx.registry->scan(ctx.moduleFolders);
    notify(ctx, NoticeLevel::Info,
           added == 0 ? "Rescan: no new modules found."
                      : "Rescan: found " + std::to_string(added) + " new module package(s).");
}

// Modules are native code, so the user sees this before choosing a zip (see SECURITY.md).
bool installRequested = false;
bool installPickRequested = false;

} // namespace

void drawInstallDialog() {
    if (installRequested) {
        ImGui::OpenPopup("Install module");
        installRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f));
    if (!ImGui::BeginPopupModal("Install module", nullptr, ImGuiWindowFlags_NoResize))
        return;
    ImGui::TextWrapped("Modules run as native code with the same access to your files as this app, and they "
                       "aren't sandboxed. Only install modules from people you trust.");
    ImGui::Spacing();
    if (ImGui::Button("Choose .zip...")) {
        installPickRequested = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void requestModuleInstall() {
    installRequested = true;
}

void processModuleInstall(ShellContext& ctx, HWND owner) {
    if (!installPickRequested)
        return;
    installPickRequested = false;
    std::filesystem::path zip;
    if (!pickModuleZip(owner, zip))
        return;
    const std::filesystem::path data = dsptb::dataDir();
    const dsptb::ModuleInstallResult r =
        dsptb::installModuleZip(zip, data / "modules", data / "tmp", [&](const std::string& id) {
            return ctx.registry && ctx.registry->findPackage(id) != nullptr;
        });
    const std::string label = r.manifest.name + " " + r.manifest.version;
    if (!r.ok) {
        notify(ctx, NoticeLevel::Error, "Could not install the module. " + r.error);
    } else if (r.needsRestart) {
        notify(ctx, NoticeLevel::Warning,
               "Installed " + label + ". Restart the app to start using this version.");
    } else {
        if (ctx.registry)
            ctx.registry->scan(ctx.moduleFolders);
        notify(ctx, NoticeLevel::Info, "Installed " + label + ".");
    }
}

namespace {

void openModulesFolder() {
    const std::filesystem::path dir = dsptb::dataDir() / "modules";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

ImVec4 statusColor(PackageStatus s) {
    switch (s) {
    case PackageStatus::Ok:
        return ImVec4(0.45f, 0.80f, 0.50f, 1.0f);
    case PackageStatus::Faulted:
        return ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
    default:
        return ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
    }
}

} // namespace

// ----------------------------------------------------------------------------------------------------------
// Module Browser
// ----------------------------------------------------------------------------------------------------------

void drawModuleBrowserPanel(ShellContext& ctx) {
    if (!beginPanel(ctx, kModuleBrowser))
        return;
    if (!ctx.registry) {
        ImGui::TextDisabled("The module system is not available.");
        ImGui::End();
        return;
    }
    if (ImGui::Button("Rescan"))
        rescanModules(ctx);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Looks for new module packages. Updating a module that is already loaded needs a "
                          "restart.");
    ImGui::SameLine();
    if (ImGui::Button("Open modules folder"))
        openModulesFolder();
    // On its own line: the panel's default width has no room for a third button.
    if (ImGui::Button("Install from .zip..."))
        installRequested = true;

    const auto& packages = ctx.registry->packages();
    if (packages.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No modules found.");
        ImGui::TextWrapped("Put module packages (a folder with module.json and the DLL) into the modules "
                           "folder, then press Rescan.");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##modules", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 44.0f * dpiScale());
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (const auto& pkgPtr : packages) {
            const ModulePackage& pkg = *pkgPtr;
            for (const ManifestModule& mm : pkg.manifest.modules) {
                ImGui::TableNextRow();
                ImGui::PushID(mm.id.c_str());
                const bool addable =
                    pkg.status == PackageStatus::Ok && (mm.kind == "processor" || mm.kind == "analyzer");
                ImGui::TableSetColumnIndex(0);
                ImGui::BeginDisabled(!addable);
                if (ImGui::SmallButton("Add"))
                    addModuleInstance(ctx, mm.id);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                         ImGuiHoveredFlags_ForTooltip)) {
                    if (addable)
                        ImGui::SetTooltip("Add to session");
                    else if (mm.kind == "offline")
                        ImGui::SetTooltip("Offline modules arrive in a later version.");
                    else
                        ImGui::SetTooltip("This module cannot be used: %s", pkg.statusText.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(pkg.manifest.modules.size() == 1 ? pkg.manifest.name.c_str()
                                                                        : mm.id.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                    ImGui::SetTooltip("%s\n%s\n%s", mm.id.c_str(), pkg.manifest.description.c_str(),
                                      pkg.dllPathUtf8().c_str());
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(mm.kind.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(pkg.manifest.version.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(pkg.manifest.author.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextColored(statusColor(pkg.status), "%s", statusLabel(pkg.status));
                if (pkg.status != PackageStatus::Ok && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s", pkg.statusText.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ----------------------------------------------------------------------------------------------------------
// Chain panel
// ----------------------------------------------------------------------------------------------------------

void drawChainPanel(ShellContext& ctx) {
    if (!beginPanel(ctx, kChain))
        return;
    if (!ctx.model || (ctx.model->instances().empty() && ctx.model->placeholders().empty())) {
        ImGui::TextDisabled("The chain is empty.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Add a module from the Module Browser. Processors run top to bottom; drag a row to "
            "reorder. Analyzers (Spectrogram, Level Meter) listen to the audio and are listed below them.");
        ImGui::End();
        return;
    }
    const float s = dpiScale();
    ImGui::TextDisabled("Total latency: %s", latencyText(ctx, ctx.model->totalLatencyFrames()).c_str());
    ImGui::Separator();

    // Actions are collected and applied after the loop: they change the list being drawn.
    InstanceId removeId = 0, moveId = 0;
    size_t moveTo = 0;
    InstanceId toggleId = 0;
    bool toggleTo = false;

    const auto& records = ctx.model->instances();
    // One row. `number` is the position in the processor chain (processors) or 0 (analyzers); `procIndex` is
    // where a dragged row would land in the chain.
    const auto row = [&](const InstanceRecord& rec, size_t number, size_t procIndex) {
        ImGui::PushID(static_cast<int>(rec.id));
        const bool faulted = rec.instance->faulted();
        const bool analyzer = rec.isAnalyzer();

        bool en = rec.enabled && !rec.removing;
        ImGui::BeginDisabled(rec.removing || faulted);
        if (ImGui::Checkbox("##enabled", &en)) {
            toggleId = rec.id;
            toggleTo = en;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(faulted    ? "Faulted: bypassed"
                              : analyzer ? "Enable / pause"
                                         : "Enable / bypass");
        ImGui::SameLine();

        char label[200];
        const char* suffix = faulted ? "  [FAULTED]" : (rec.removing ? "  [removing]" : "");
        const char* moduleName = rec.label.empty() ? "" : rec.instance->desc().name;
        if (analyzer)
            std::snprintf(label, sizeof label, "%s%s%s%s  [%s]%s", instanceName(rec).c_str(),
                          rec.label.empty() ? "" : "  (", moduleName, rec.label.empty() ? "" : ")",
                          rec.tap == DSPTB_TAP_PRE ? "PRE" : "POST", suffix);
        else
            std::snprintf(label, sizeof label, "%zu  %s%s%s%s%s", number, instanceName(rec).c_str(),
                          rec.label.empty() ? "" : "  (", moduleName, rec.label.empty() ? "" : ")", suffix);
        const float rowW = ImGui::GetContentRegionAvail().x - 26.0f * s;
        if (faulted)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
        else if (!en)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool clicked = ImGui::Selectable(label, false, ImGuiSelectableFlags_None, ImVec2(rowW, 0.0f));
        if (faulted || !en)
            ImGui::PopStyleColor();
        if (clicked) {
            ctx.moduleUi.panelOpen[rec.id] = true;
            ctx.moduleUi.focusRequest = rec.id;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            if (analyzer) {
                ImGui::SetTooltip("%s\nAnalyzer: listens %s the processors; not part of the signal path\n"
                                  "Double-click to rename, click to show its panel",
                                  rec.instance->moduleId().c_str(),
                                  rec.tap == DSPTB_TAP_PRE ? "before" : "after");
            } else {
                const uint32_t lat = rec.instance->latencyFrames();
                ImGui::SetTooltip(
                    "%s\nLatency: %s\nDrag to reorder, double-click to rename, click to show its panel",
                    rec.instance->moduleId().c_str(), latencyText(ctx, lat).c_str());
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            startRename(ctx, rec);
        if (ImGui::BeginPopupContextItem("##rowmenu")) {
            if (ImGui::MenuItem("Rename..."))
                startRename(ctx, rec);
            if (ImGui::MenuItem("Remove"))
                removeId = rec.id;
            ImGui::EndPopup();
        }
        if (!analyzer) {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(kChainPayload, &rec.id, sizeof(InstanceId));
                ImGui::Text("%s", instanceName(rec).c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kChainPayload)) {
                    moveId = *static_cast<const InstanceId*>(p->Data);
                    moveTo = procIndex;
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            removeId = rec.id;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Remove from the session");
        ImGui::PopID();
    };

    size_t procCount = 0, analyzerCount = 0;
    for (const auto& rec : records)
        (rec.isAnalyzer() ? analyzerCount : procCount) += 1;
    if (procCount == 0)
        ImGui::TextDisabled("No processors. Analyzers below listen to the source.");
    size_t procIndex = 0;
    for (const auto& rec : records)
        if (!rec.isAnalyzer()) {
            row(rec, procIndex + 1, procIndex);
            ++procIndex;
        }
    if (analyzerCount > 0) {
        ImGui::Spacing();
        ImGui::SeparatorText("Analyzers");
        for (const auto& rec : records)
            if (rec.isAnalyzer())
                row(rec, 0, 0);
    }
    // Modules the session names that are not installed: greyed, no sound, data kept for the next
    // save.
    if (!ctx.model->placeholders().empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Missing modules");
        size_t dropIndex = ctx.model->placeholders().size();
        for (size_t i = 0; i < ctx.model->placeholders().size(); ++i) {
            const auto& ph = ctx.model->placeholders()[i];
            ImGui::PushID(static_cast<int>(i) + 100000);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Text("%s  [missing]", ph.doc.module.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip(
                    "%s\nIts settings are kept and saved with the session. Install the module and "
                    "reopen the session to use it.",
                    ph.reason.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                dropIndex = i;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Forget this module and its saved settings");
            ImGui::PopID();
        }
        if (dropIndex < ctx.model->placeholders().size())
            ctx.model->removePlaceholder(dropIndex);
    }
    if (ctx.model->reorderPending())
        ImGui::TextDisabled("reordering...");

    if (toggleId)
        ctx.model->setEnabled(toggleId, toggleTo);
    if (moveId)
        ctx.model->move(moveId, moveTo);
    if (removeId)
        ctx.model->remove(removeId);
    ImGui::End();
}

// ----------------------------------------------------------------------------------------------------------
// Instance panels: header, automatic parameter UI, then the module's own draw().
// ----------------------------------------------------------------------------------------------------------

void drawInstancePanels(ShellContext& ctx) {
    if (!ctx.model)
        return;
    const float s = dpiScale();
    InstanceId removeId = 0, duplicateId = 0;

    // Copy ids: the model's list changes when something is removed or duplicated.
    std::vector<InstanceId> ids;
    for (const auto& r : ctx.model->instances())
        ids.push_back(r.id);
    if (ctx.canvas)
        ctx.canvas->retainInstances(ids);

    for (InstanceId id : ids) {
        InstanceRecord* rec = ctx.model->find(id);
        if (!rec || rec->removing)
            continue;
        const auto openIt = ctx.moduleUi.panelOpen.find(id);
        bool open = openIt == ctx.moduleUi.panelOpen.end() ? true : openIt->second;
        if (!open && ctx.moduleUi.focusRequest != id)
            continue;
        if (ctx.moduleUi.focusRequest == id) {
            open = true;
            ImGui::SetNextWindowFocus();
            ctx.moduleUi.focusRequest = 0;
        }
        const std::string title =
            instanceName(*rec) + " #" + std::to_string(id) + "###inst" + std::to_string(id);
        ImGui::SetNextWindowDockID(centralDockId(), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380.0f * s, 280.0f * s), ImGuiCond_FirstUseEver);
        const bool visible = ImGui::Begin(title.c_str(), &open);
        ctx.moduleUi.panelOpen[id] = open;
        if (visible) {
            ModuleInstance& m = *rec->instance;
            const bool faulted = m.faulted();

            bool en = rec->enabled;
            ImGui::BeginDisabled(faulted);
            if (ImGui::Checkbox("Enabled", &en))
                ctx.model->setEnabled(id, en);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (rec->isAnalyzer()) {
                int tap = rec->tap == DSPTB_TAP_PRE ? 0 : 1;
                ImGui::SetNextItemWidth(150.0f * s);
                const char* taps[] = {"PRE (raw source)", "POST (after chain)"};
                if (ImGui::Combo("##tap", &tap, taps, 2))
                    ctx.model->setTap(id, tap == 0 ? DSPTB_TAP_PRE : DSPTB_TAP_POST);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("Where this analyzer listens: before the processors (the source as it "
                                      "arrives, even when the mic is not monitored) or after them.");
            } else {
                ImGui::TextDisabled("Latency: %s", latencyText(ctx, m.latencyFrames()).c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("..."))
                ImGui::OpenPopup("##instmenu");
            if (ImGui::BeginPopup("##instmenu")) {
                if (ImGui::MenuItem("Reset parameters")) {
                    for (uint32_t i = 0; i < m.params().size(); ++i)
                        ctx.model->setParamUndoable(id, i, m.params().defaultValue(i));
                }
                if (ImGui::MenuItem("Rename..."))
                    startRename(ctx, *rec);
                if (ImGui::MenuItem("Duplicate"))
                    duplicateId = id;
                if (ImGui::MenuItem("Remove"))
                    removeId = id;
                ImGui::EndPopup();
            }
            if (faulted) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
                ImGui::TextWrapped(
                    "This module faulted and is bypassed. Remove it, or restart the app to try "
                    "again.");
                ImGui::PopStyleColor();
            }
            ImGui::Separator();

            ImGui::BeginDisabled(faulted);
            ImGui::PushID(static_cast<int>(id));
            if (!(m.desc().caps & DSPTB_CAP_HIDE_AUTO_PARAMS)) {
                // An analyzer's picture is the point of its panel, so its parameters fold away (closed by
                // default) instead of pushing the view off the bottom.
                const bool fold = rec->isAnalyzer() || m.params().size() > 5;
                const bool show = !fold || ImGui::CollapsingHeader("Parameters");
                if (show)
                    for (uint32_t i = 0; i < m.params().size(); ++i) {
                        if (m.params().desc(i).flags & DSPTB_PARAM_HIDDEN)
                            continue;
                        drawParamControl(ctx, *rec, i);
                    }
            }
            ImGui::PopID();
            if (ctx.canvas) {
                ImGui::Spacing();
                const dsptb_canvas_api* api = ctx.canvas->begin(ctx, *rec);
                m.draw(api);
                ctx.canvas->end();
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    drawRenameWindow(ctx);
    if (duplicateId)
        if (const InstanceRecord* src = ctx.model->find(duplicateId))
            duplicateInstance(ctx, *src);
    if (removeId)
        ctx.model->remove(removeId);
}

// ----------------------------------------------------------------------------------------------------------
// menus and shortcuts
// ----------------------------------------------------------------------------------------------------------

void drawEditMenu(ShellContext& ctx) {
    if (!ImGui::BeginMenu("Edit"))
        return;
    SessionModel* m = ctx.model;
    const std::string undoText = m && m->canUndo() ? "Undo " + m->undoLabel() : "Undo";
    const std::string redoText = m && m->canRedo() ? "Redo " + m->redoLabel() : "Redo";
    if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false, m && m->canUndo()))
        m->undo();
    if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false, m && m->canRedo()))
        m->redo();
    ImGui::EndMenu();
}

void drawModulesMenuItems(ShellContext& ctx) {
    ImGui::MenuItem("Browser", nullptr, &ctx.panelOpen[kModuleBrowser]);
    ImGui::Separator();
    if (ImGui::MenuItem("Install from .zip..."))
        installRequested = true;
    if (ImGui::MenuItem("Open modules folder"))
        openModulesFolder();
    if (ImGui::MenuItem("Rescan"))
        rescanModules(ctx);
}

void handleModuleShortcuts(ShellContext& ctx) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!ctx.model || io.WantTextInput || !io.KeyCtrl)
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift)
            ctx.model->redo();
        else
            ctx.model->undo();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        ctx.model->redo();
    }
}

} // namespace dsptb::ui
