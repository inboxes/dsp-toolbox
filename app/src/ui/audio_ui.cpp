// Audio-facing UI: Transport panel, Audio Settings window, status bar
// fields and keyboard shortcuts (Space, Esc, Ctrl+O).
#include "session_ui.hpp"
#include "shell_internal.hpp"

#include <dsptb/core/gpu_runtime_loader.hpp>
#include <dsptb/core/paths.hpp>
#include <dsptb/core/settings.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dsptb::ui {

namespace {

constexpr uint32_t kRates[] = {44100, 48000, 88200, 96000};
constexpr uint32_t kBuffers[] = {64, 128, 256, 512, 1024, 2048};

std::string formatTime(double seconds) {
    seconds = std::max(0.0, seconds);
    const int total = static_cast<int>(seconds);
    const int ms = static_cast<int>((seconds - total) * 1000.0 + 0.5) % 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d.%03d", total / 60, total % 60, ms);
    return buf;
}

float toDb(float linear) {
    return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f;
}

void togglePlay(ShellContext& ctx) {
    Engine& e = ctx.audio->engine();
    if (e.playing())
        e.pause();
    else
        e.play();
}

void switchSource(ShellContext& ctx, SourceKind kind) {
    std::string err;
    if (!ctx.audio->setSource(kind, err))
        notify(ctx, NoticeLevel::Error,
               kind == SourceKind::Mic ? "Could not open the microphone: " + err : err);
    if (kind == SourceKind::Mic)
        ctx.audio->engine().setMonitorMic(false); // monitoring is off by default
}

// ---- icon buttons -----------------------------------------------------------------------------------------
// Standard Unicode symbols from Segoe UI Symbol (merged into the UI font). If the glyph is missing in the
// font, the button falls back to its text label, so nothing ever shows as a blank box. Every icon button has
// a tooltip.

struct Icon {
    const char* glyph;
    ImWchar codepoint;
    const char* fallback;
};

constexpr Icon kPlay{"\u25B6", 0x25B6, "Play"};
constexpr Icon kPause{"\u23F8", 0x23F8, "Pause"};
constexpr Icon kStop{"\u23F9", 0x23F9, "Stop"};
constexpr Icon kLoop{"\U0001F501", 0x1F501, "Loop"};
constexpr Icon kOpen{"\U0001F4C2", 0x1F4C2, "Open"};
constexpr Icon kMuteOff{"\U0001F50A", 0x1F50A, "Mute"};
constexpr Icon kMuteOn{"\U0001F507", 0x1F507, "Muted"};

// Returns true when clicked. `active` draws it as a pressed toggle, `danger` in red.
bool iconButton(const char* id, const Icon& icon, const char* tooltip, bool active = false,
                bool danger = false) {
    const float s = dpiScale();
    const bool haveGlyph = ImGui::GetFont()->IsGlyphInFont(icon.codepoint);
    const std::string label = std::string(haveGlyph ? icon.glyph : icon.fallback) + "##" + id;
    int pushed = 0;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
        ++pushed;
    }
    if (danger) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
        pushed += 2;
    }
    const float h = ImGui::GetFrameHeight();
    const bool clicked = ImGui::Button(label.c_str(), haveGlyph ? ImVec2(h * 1.35f, h) : ImVec2(0.0f, h));
    if (pushed)
        ImGui::PopStyleColor(pushed);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    (void)s;
    return clicked;
}

// ---- waveform overview ----------------------------------------------------------------------------------

void drawWaveform(ShellContext& ctx, ImVec2 size) {
    AudioSession& a = *ctx.audio;
    Engine& e = a.engine();
    const float s = dpiScale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##waveform", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(p0, p1, IM_COL32(20, 22, 26, 255), 4.0f * s);
    const auto file = a.file();
    const double dur = a.durationSeconds();
    if (!file || dur <= 0.0) {
        const char* msg =
            a.loading() ? "Decoding..." : "No file loaded. Use Open, or File > Open Audio File.";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f),
                    IM_COL32(130, 135, 145, 255), msg);
        return;
    }

    auto secToX = [&](double sec) { return p0.x + static_cast<float>(sec / dur) * size.x; };
    auto xToSec = [&](float x) {
        return std::clamp(static_cast<double>((x - p0.x) / size.x), 0.0, 1.0) * dur;
    };

    // Loop region (behind the waveform).
    const bool loopOn = a.loopEnabled();
    const double loopA = a.loopStartSeconds(), loopB = a.loopEndSeconds();
    const float xa = secToX(loopA), xb = secToX(loopB);
    if (loopOn)
        dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p1.y), IM_COL32(70, 120, 200, 55));

    // Min/max per pixel column, merged from the precomputed overview.
    const WaveformOverview& ov = file->overview;
    const float mid = p0.y + size.y * 0.5f;
    const float half = size.y * 0.5f - 2.0f * s;
    dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), IM_COL32(60, 64, 72, 255));
    const int columns = std::max(1, static_cast<int>(size.x));
    const size_t n = ov.minimum.size();
    for (int x = 0; x < columns && n > 0; ++x) {
        const size_t c0 =
            std::min(n - 1, static_cast<size_t>(static_cast<double>(x) / columns * static_cast<double>(n)));
        const size_t c1 =
            std::min(n, std::max(c0 + 1, static_cast<size_t>(static_cast<double>(x + 1) / columns *
                                                             static_cast<double>(n))));
        float lo = 1e9f, hi = -1e9f;
        for (size_t c = c0; c < c1; ++c) {
            lo = std::min(lo, ov.minimum[c]);
            hi = std::max(hi, ov.maximum[c]);
        }
        const float px = p0.x + static_cast<float>(x) + 0.5f;
        dl->AddLine(ImVec2(px, mid - std::clamp(hi, -1.0f, 1.0f) * half),
                    ImVec2(px, mid - std::clamp(lo, -1.0f, 1.0f) * half - 1.0f), IM_COL32(95, 165, 240, 255));
    }

    // Loop handles and playhead.
    if (loopOn) {
        dl->AddRectFilled(ImVec2(xa - 1.5f * s, p0.y), ImVec2(xa + 1.5f * s, p1.y),
                          IM_COL32(120, 175, 255, 255));
        dl->AddRectFilled(ImVec2(xb - 1.5f * s, p0.y), ImVec2(xb + 1.5f * s, p1.y),
                          IM_COL32(120, 175, 255, 255));
    }
    const float xp = secToX(ctx.waveformDrag == 1 ? ctx.scrubSeconds : a.positionSeconds());
    dl->AddLine(ImVec2(xp, p0.y), ImVec2(xp, p1.y), IM_COL32(255, 255, 255, 230), 1.5f);

    // Interaction: click or drag = seek; with Loop on, the two handles are draggable.
    const float grab = 7.0f * s;
    const float mx = ImGui::GetIO().MousePos.x;
    const bool nearA = loopOn && std::fabs(mx - xa) <= grab;
    const bool nearB = loopOn && std::fabs(mx - xb) <= grab;
    if (hovered && (nearA || nearB))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) {
        ctx.waveformDrag = nearA ? 2 : (nearB ? 3 : 1);
        if (ctx.waveformDrag == 1) {
            // Scrubbing: hold the audio (it fades out in 5 ms) and move only the playhead. Re-seeking the
            // live stream on every UI frame would fade out and in again every few milliseconds, which is
            // heard as a buzz. The real seek happens once, on release.
            ctx.scrubSeconds = xToSec(mx);
            ctx.resumeAfterScrub = e.playing();
            if (ctx.resumeAfterScrub)
                e.pause();
        }
    }
    if (ImGui::IsItemActive()) {
        const double t = xToSec(mx);
        constexpr double kMinLoop = 0.05;
        if (ctx.waveformDrag == 1)
            ctx.scrubSeconds = t;
        else if (ctx.waveformDrag == 2)
            a.setLoopSeconds(true, std::min(t, loopB - kMinLoop), loopB);
        else if (ctx.waveformDrag == 3)
            a.setLoopSeconds(true, loopA, std::max(t, loopA + kMinLoop));
    } else {
        if (ctx.waveformDrag == 1) { // released
            a.seekSeconds(ctx.scrubSeconds);
            if (ctx.resumeAfterScrub)
                e.play();
            ctx.resumeAfterScrub = false;
        }
        ctx.waveformDrag = 0;
    }
}

// ---- peak meter ----------------------------------------------------------------------------------------

// A level bar from floorDb to 0 dB. `markerDb` (if > floorDb) draws a vertical line, e.g. the noise gate
// threshold.
void drawMeterBar(float db, ImVec2 size, float floorDb = -60.0f, float markerDb = -1000.0f) {
    const float s = dpiScale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(28, 30, 35, 255), 2.0f * s);
    const float frac = std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
    const ImU32 col = db > -3.0f ? IM_COL32(235, 90, 80, 255)
                                 : (db > -12.0f ? IM_COL32(235, 200, 80, 255) : IM_COL32(90, 200, 120, 255));
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x * frac, p0.y + size.y), col, 2.0f * s);
    if (markerDb > floorDb) {
        const float mx = p0.x + size.x * std::clamp((markerDb - floorDb) / -floorDb, 0.0f, 1.0f);
        dl->AddLine(ImVec2(mx, p0.y - 1.0f), ImVec2(mx, p0.y + size.y + 1.0f), IM_COL32(255, 255, 255, 220),
                    1.5f * s);
    }
}

} // namespace

void applyAudioSettingsFromConfig(ShellContext& ctx) {
    const AudioConfig& c = ctx.audio->config();
    ctx.settings->audio.backend = backendName(c.backend);
    ctx.settings->audio.inputDeviceId = c.inputDeviceId;
    ctx.settings->audio.outputDeviceId = c.outputDeviceId;
    ctx.settings->audio.sampleRate = static_cast<int>(c.sampleRate);
    ctx.settings->audio.bufferFrames = static_cast<int>(c.bufferFrames);
}

// ---------------------------------------------------------------------------------------------------------
// Transport panel
// ---------------------------------------------------------------------------------------------------------

void drawTransportPanel(ShellContext& ctx) {
    if (!beginPanel(ctx, kTransport))
        return;
    AudioSession& a = *ctx.audio;
    Engine& e = a.engine();
    const float s = dpiScale();
    const bool isMic = a.source() == SourceKind::Mic;

    // -- source and file/mic specific controls --
    ImGui::TextUnformatted("Source");
    ImGui::SameLine();
    if (ImGui::RadioButton("File", !isMic) && isMic)
        switchSource(ctx, SourceKind::File);
    ImGui::SameLine();
    if (ImGui::RadioButton("Mic", isMic) && !isMic)
        switchSource(ctx, SourceKind::Mic);
    ImGui::SameLine();
    if (isMic) {
        ImGui::TextDisabled(e.captureActive() ? "Live input" : "Input not available");
    } else {
        if (iconButton("open", kOpen, "Open an audio file (Ctrl+O)"))
            ctx.openFileRequested = true;
        ImGui::SameLine();
        if (a.loading()) {
            ImGui::ProgressBar(a.loadProgress(), ImVec2(160.0f * s, 0.0f));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", a.loadingName().c_str());
        } else if (a.file()) {
            const std::string name = toUtf8(fromUtf8(a.file()->path).filename());
            ImGui::TextUnformatted(name.c_str());
        } else {
            ImGui::TextDisabled("No file loaded");
        }
    }

    const bool fileMode = !isMic;
    if (fileMode) {
        // -- transport buttons --
        ImGui::BeginDisabled(!e.hasFile());
        if (iconButton("play", e.playing() ? kPause : kPlay, e.playing() ? "Pause (Space)" : "Play (Space)"))
            togglePlay(ctx);
        ImGui::SameLine();
        if (iconButton("stop", kStop, "Stop and return to the start (or the loop start)"))
            e.stopTransport();
        ImGui::SameLine();
        const bool loop = a.loopEnabled();
        if (iconButton("loop", kLoop,
                       loop ? "Loop is on: drag the handles on the waveform to set the region"
                            : "Loop the whole file or a region",
                       loop))
            a.setLoopSeconds(!loop, a.loopStartSeconds(), a.loopEndSeconds());
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (e.hasFile()) {
            const double shown = ctx.waveformDrag == 1 ? ctx.scrubSeconds : a.positionSeconds();
            ImGui::Text("%s / %s", formatTime(shown).c_str(), formatTime(a.durationSeconds()).c_str());
        } else {
            ImGui::TextDisabled("--:--.--- / --:--.---");
        }
    } else {
        // -- microphone controls: monitoring and the noise gate --
        bool monitor = e.monitorMic();
        if (ImGui::Checkbox("Monitor", &monitor)) {
            e.setMonitorMic(monitor);
            if (monitor && !ctx.settings->micMonitorWarningShown) { // one-time warning
                notify(ctx, NoticeLevel::Warning, "Use headphones to avoid feedback.");
                ctx.settings->micMonitorWarningShown = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hear the microphone through the output. Off by default. Use headphones.");
        ImGui::SameLine();
        bool gateOn = e.micGateEnabled();
        float thr = e.micGateThresholdDb();
        bool gateChanged = ImGui::Checkbox("Noise gate", &gateOn);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Silence the microphone while it is quieter than the threshold (background noise).");
        if (gateOn) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(80.0f * s, ImGui::GetContentRegionAvail().x - 70.0f * s));
            gateChanged |= ImGui::SliderFloat("##gatethr", &thr, -80.0f, -20.0f, "%.0f dB");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Gate threshold. Set it just above the noise you see on the meter while you are quiet.");
            ImGui::SameLine();
            if (e.micGateOpen())
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.5f, 1.0f), "open");
            else
                ImGui::TextDisabled("closed");
        }
        if (gateChanged) {
            e.setMicGate(gateOn, thr);
            ctx.settings->micGateEnabled = gateOn;
            ctx.settings->micGateThresholdDb = e.micGateThresholdDb();
        }
    }

    // -- waveform overview, fills the space above the footer --
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 waveSize(avail.x, std::max(48.0f * s, avail.y - footer));
    if (fileMode) {
        drawWaveform(ctx, waveSize);
    } else {
        const float startY = ImGui::GetCursorPosY();
        ImGui::Dummy(ImVec2(0, 4.0f * s));
        ImGui::TextDisabled("Microphone level (before the gate)");
        const float marker = e.micGateEnabled() ? e.micGateThresholdDb() : -1000.0f;
        drawMeterBar(toDb(e.micPeakLeft()), ImVec2(waveSize.x, 10.0f * s), -80.0f, marker);
        drawMeterBar(toDb(e.micPeakRight()), ImVec2(waveSize.x, 10.0f * s), -80.0f, marker);
        const float used = ImGui::GetCursorPosY() - startY;
        ImGui::Dummy(ImVec2(0, std::max(0.0f, waveSize.y - used)));
    }

    // -- footer: master gain, mute, clip, peak meters --
    float db = e.masterGainDb();
    ImGui::SetNextItemWidth(std::max(90.0f * s, avail.x * 0.30f));
    if (ImGui::SliderFloat("##master", &db, -60.0f, 12.0f, "Master %.1f dB"))
        e.setMasterGainDb(db);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        e.setMasterGainDb(0.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Double-click to reset to 0 dB");
    ImGui::SameLine();
    const bool muted = e.muted();
    if (iconButton("mute", muted ? kMuteOn : kMuteOff,
                   muted ? "Output is muted. Click to unmute (Esc)" : "Mute the output (Esc)", false, muted))
        e.setMute(!muted);
    ImGui::SameLine();
    if (muted)
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "MUTED");
    else
        ImGui::TextDisabled("sound on");
    ImGui::SameLine();
    if (e.clipLatched()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
        if (ImGui::SmallButton("CLIP"))
            e.clearClip();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The output hit 0 dBFS. Click to clear.");
    } else {
        ImGui::TextDisabled("clip");
    }
    ImGui::SameLine();
    const float barW = std::max(40.0f * s, ImGui::GetContentRegionAvail().x - 6.0f * s);
    ImGui::BeginGroup();
    drawMeterBar(toDb(e.peakLeft()), ImVec2(barW, 5.0f * s));
    drawMeterBar(toDb(e.peakRight()), ImVec2(barW, 5.0f * s));
    ImGui::EndGroup();

    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------
// Audio Settings
// ---------------------------------------------------------------------------------------------------------

namespace {

bool deviceCombo(const char* label, std::string& id, const std::vector<DeviceInfo>& devices, bool input,
                 const char* defaultText = "System default") {
    std::string preview = defaultText;
    if (!id.empty()) {
        preview = "(unavailable device)";
        for (const auto& d : devices)
            if (d.isInput == input && d.id == id)
                preview = d.name;
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (ImGui::Selectable(defaultText, id.empty())) {
            id.clear();
            changed = true;
        }
        int i = 0;
        for (const auto& d : devices) {
            if (d.isInput != input)
                continue;
            ImGui::PushID(i++);
            const std::string text = d.name + (d.isDefault ? "  (default)" : "");
            if (ImGui::Selectable(text.c_str(), d.id == id)) {
                id = d.id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// The GPU runtime (docs/GPU_RUNTIME.md section 6.5). Device and sync mode are saved in the settings and apply
// after a restart, because the CUDA context is created once per process.
void drawGpuSection(ShellContext& ctx) {
    const GpuRuntimeLoader& rt = GpuRuntimeLoader::instance();
    const GpuRuntimeState& st = rt.state();
    Settings& set = *ctx.settings;
    ImGui::Separator();
    ImGui::TextUnformatted("GPU");
    if (st.ok()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f), "Runtime OK");
        ImGui::SameLine();
        ImGui::TextDisabled("(for modules that run neural networks)");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Runtime not available");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f * dpiScale());
        ImGui::TextWrapped("%s", st.statusText.c_str());
        ImGui::PopTextWrapPos();
    }
    if (st.ok()) {
        const std::vector<GpuDevice> devices = rt.devices();
        std::string current = "Device " + std::to_string(set.gpu.device);
        for (const GpuDevice& d : devices)
            if (d.index == set.gpu.device)
                current = d.name;
        if (ImGui::BeginCombo("GPU device", current.c_str())) {
            for (const GpuDevice& d : devices) {
                const std::string label = d.name + "  (cc " + std::to_string(d.ccMajor) + "." + std::to_string(d.ccMinor) + ")";
                if (ImGui::Selectable(label.c_str(), d.index == set.gpu.device))
                    set.gpu.device = d.index;
            }
            ImGui::EndCombo();
        }
        for (const GpuDevice& d : devices)
            if (d.index == st.selectedDevice)
                ImGui::TextDisabled("VRAM: %.1f GB total, %.1f GB free", static_cast<double>(d.vramTotal) / 1073741824.0,
                                    static_cast<double>(d.vramFree) / 1073741824.0);
        ImGui::TextDisabled("Driver CUDA %d.%d, runtime CUDA %d.%d, cuDNN %d.%d", st.driverCuda / 1000,
                            st.driverCuda % 1000 / 10, st.runtimeCuda / 1000, st.runtimeCuda % 1000 / 10,
                            st.cudnn / 10000, st.cudnn % 10000 / 100);
        ImGui::TextDisabled("ONNX Runtime %s, LibTorch %s", st.ortVersion.c_str(), st.torchVersion.c_str());
    }
    if (st.ok() || st.dllFound) {
        const bool blocking = set.gpu.sync != "spin";
        if (ImGui::BeginCombo("GPU wait mode", blocking ? "Blocking (recommended)" : "Spin")) {
            if (ImGui::Selectable("Blocking (recommended)", blocking))
                set.gpu.sync = "blocking";
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The CPU sleeps while the GPU works. Costs nothing measurable and frees a CPU core.");
            if (ImGui::Selectable("Spin", !blocking))
                set.gpu.sync = "spin";
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The CPU busy-waits. Wakes up a little sooner but keeps a core at 100 %% while a GPU module runs.");
            ImGui::EndCombo();
        }
        const bool pending = st.ok() && ((set.gpu.sync == "spin") != (st.syncMode == DSPTB_GPU_SYNC_SPIN) ||
                                         set.gpu.device != st.selectedDevice);
        if (pending)
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Restart to apply.");
    }
    ImGui::TextDisabled("Runtime folder: %s", st.runtimeDir.c_str());
    if (ImGui::SmallButton("Open folder")) {
        std::error_code ec;
        const std::filesystem::path dir = fromUtf8(st.runtimeDir);
        if (std::filesystem::is_directory(dir, ec))
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else
            notify(ctx, NoticeLevel::Warning, "The GPU runtime folder does not exist.");
    }
}

} // namespace

void drawAudioSettingsWindow(ShellContext& ctx) {
    if (!ctx.showAudioSettings) {
        ctx.editConfigInit = false;
        return;
    }
    AudioSession& a = *ctx.audio;
    const float s = dpiScale();
    if (!ctx.editConfigInit) {
        ctx.editConfig = a.config();
        ctx.editConfigInit = true;
        std::string err;
        ctx.devices.clear();
        ctx.devicesBackend = ctx.editConfig.backend;
        ctx.devicesLoaded = enumerateDevices(ctx.devicesBackend, ctx.devices, err);
        if (!ctx.devicesLoaded)
            spdlog::warn("Could not list audio devices: {}", err);
    }
    AudioConfig& cfg = ctx.editConfig;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * s, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Audio Settings", &ctx.showAudioSettings,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
        // Backend. ASIO appears only in builds made with DSPTB_ENABLE_ASIO.
        std::vector<BackendKind> kinds = availableBackends();
        if (cfg.backend == BackendKind::Null) // Null only when it is already active
            kinds.push_back(BackendKind::Null);
        if (ImGui::BeginCombo("Backend", backendDisplayName(cfg.backend))) {
            for (BackendKind kind : kinds) {
                if (ImGui::Selectable(backendDisplayName(kind), cfg.backend == kind) && cfg.backend != kind) {
                    cfg.backend = kind;
                    // Device ids belong to one backend: the old choice means nothing to the new one.
                    cfg.outputDeviceId.clear();
                    cfg.inputDeviceId.clear();
                }
                if (kind == BackendKind::WasapiExclusive && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Lowest latency, but this app takes the device for itself:\n"
                                      "other programs (media players, browsers) cannot play on it\n"
                                      "while it is open.");
            }
            ImGui::EndCombo();
        }
        if (cfg.backend != ctx.devicesBackend) {
            // The edited backend changed (combo, Revert or a failed Apply): list its devices. The ids are
            // left alone here, so Revert gets the active config's devices back.
            std::string err;
            ctx.devices.clear();
            ctx.devicesBackend = cfg.backend;
            ctx.devicesLoaded = enumerateDevices(cfg.backend, ctx.devices, err);
        }
        if (cfg.backend == BackendKind::WasapiExclusive)
            ImGui::TextDisabled("Exclusive: other apps cannot use this device while it is open.");
        const bool isNull = cfg.backend == BackendKind::Null;
        ImGui::BeginDisabled(isNull);
#ifdef DSPTB_HAS_ASIO
        if (cfg.backend == BackendKind::Asio) {
            // One ASIO driver serves both directions.
            deviceCombo("ASIO driver", cfg.outputDeviceId, ctx.devices, false, "First installed driver");
            cfg.inputDeviceId = cfg.outputDeviceId;
            ImGui::TextDisabled(
                "Use the driver of your audio interface. Without one, FlexASIO is a free option:");
            ImGui::SameLine();
            if (ImGui::SmallButton("FlexASIO (GitHub)"))
                ShellExecuteW(nullptr, L"open", L"https://github.com/dechamps/FlexASIO/releases", nullptr,
                              nullptr, SW_SHOWNORMAL);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Opens the FlexASIO download page in your browser. It is a separate,\n"
                                  "MIT-licensed ASIO driver that sits on WASAPI and lets you pick the input\n"
                                  "and output devices. Install it, then press Refresh devices.");
        } else
#endif
        {
            deviceCombo("Output device", cfg.outputDeviceId, ctx.devices, false);
            deviceCombo("Input device", cfg.inputDeviceId, ctx.devices, true);
        }
        ImGui::EndDisabled();

        const bool shared = cfg.backend == BackendKind::WasapiShared;
        ImGui::BeginDisabled(shared);
        const std::string rateText = shared ? "Device mix rate" : std::to_string(cfg.sampleRate) + " Hz";
        if (ImGui::BeginCombo("Sample rate", rateText.c_str())) {
            for (uint32_t r : kRates)
                if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), cfg.sampleRate == r))
                    cfg.sampleRate = r;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (shared && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Shared mode runs at the rate set in Windows Sound settings.");

        if (ImGui::BeginCombo("Buffer size", (std::to_string(cfg.bufferFrames) + " frames").c_str())) {
            for (uint32_t b : kBuffers)
                if (ImGui::Selectable((std::to_string(b) + " frames").c_str(), cfg.bufferFrames == b))
                    cfg.bufferFrames = b;
            ImGui::EndCombo();
        }

        // What the open stream really uses (the request can be raised: shared mode cannot go below the device
        // period).
        if (a.engine().isOpen()) {
            const Engine& en = a.engine();
            const double periodMs = 1000.0 * en.bufferFrames() / std::max<uint32_t>(1, en.sampleRate());
            if (a.config().backend == BackendKind::WasapiShared &&
                en.bufferFrames() > a.config().bufferFrames)
                ImGui::TextWrapped("The device runs %u-frame periods (%.1f ms) in shared mode and cannot go "
                                   "smaller. Use WASAPI exclusive for a smaller buffer.",
                                   en.bufferFrames(), periodMs);
#ifdef DSPTB_HAS_ASIO
            else if (a.config().backend == BackendKind::Asio && en.bufferFrames() != a.config().bufferFrames)
                ImGui::TextWrapped(
                    "The driver runs %u-frame buffers (%.1f ms); you asked for %u. An ASIO driver "
                    "decides its own buffer size: change it in the driver's control panel.",
                    en.bufferFrames(), periodMs, a.config().bufferFrames);
#endif
            else
                ImGui::TextDisabled(
                    "Device period in use: %u frames (%.1f ms). Estimated delay through the app: %.1f ms.",
                    en.bufferFrames(), periodMs, en.latencyMs());
        }
        ImGui::Spacing();
        if (ImGui::Button("Apply", ImVec2(90.0f * s, 0.0f))) {
            std::string err;
            if (a.applyConfig(cfg, err)) {
                applyAudioSettingsFromConfig(ctx);
                notify(ctx, NoticeLevel::Info, "Audio settings applied.");
            } else {
                notify(ctx, NoticeLevel::Error,
                       "Could not apply audio settings: " + err + " The previous settings were restored.");
            }
            ctx.editConfig = a.config();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh devices")) {
            std::string err;
            ctx.devices.clear();
            ctx.devicesBackend = cfg.backend;
            ctx.devicesLoaded = enumerateDevices(cfg.backend, ctx.devices, err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            ctx.editConfig = a.config();
#ifdef DSPTB_HAS_ASIO
        if (a.config().backend == BackendKind::Asio && a.engine().isOpen()) {
            ImGui::SameLine();
            if (ImGui::Button("Driver control panel") && !a.engine().openDriverControlPanel())
                notify(ctx, NoticeLevel::Warning, "This ASIO driver has no control panel.");
        }
#endif

        ImGui::Separator();
        const Engine& e = a.engine();
        if (e.isOpen())
            ImGui::TextDisabled("Running: %s, %u Hz, %u frames, %.1f ms buffered",
                                backendDisplayName(a.config().backend), e.sampleRate(), e.bufferFrames(),
                                e.latencyMs());
        else
            ImGui::TextDisabled("No audio stream is open.");
        if (!ctx.devicesLoaded)
            ImGui::TextDisabled("The audio system could not be queried.");
        drawGpuSection(ctx);
#ifdef DSPTB_HAS_ASIO
        // This dialog is where ASIO gets switched on, so Steinberg's guidelines put the logo here in every
        // state.
        ImGui::Spacing();
        if (ctx.asioLogo) {
            const float logoWidth = 56.0f * s;
            ImGui::Image(ctx.asioLogo,
                         ImVec2(logoWidth, logoWidth * ctx.asioLogoSize.y / ctx.asioLogoSize.x));
            ImGui::SameLine();
        }
        ImGui::TextDisabled("ASIO is a registered trademark of\nSteinberg Media Technologies GmbH.");
#endif
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------
// Status bar fields
// ---------------------------------------------------------------------------------------------------------

void drawAudioStatusFields(ShellContext& ctx) {
    const AudioSession& a = *ctx.audio;
    if (ctx.session && ctx.session->recorder().active()) {
        // record indicator and elapsed time, with the overrun count if the writer fell behind.
        const Recorder& rec = ctx.session->recorder();
        const int total = static_cast<int>(rec.elapsedSeconds());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
        ImGui::Text("%s REC %02d:%02d", "\xE2\x97\x8F", total / 60, total % 60);
        ImGui::PopStyleColor();
        if (rec.overrunFrames() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(overruns: %llu frames)",
                                static_cast<unsigned long long>(rec.overrunFrames()));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    }
    const Engine& e = a.engine();
    if (a.deviceLost()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
        ImGui::TextUnformatted("Device lost");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("|  retrying every 2 s");
        return;
    }
    if (!e.isOpen()) {
        ImGui::TextUnformatted("No audio");
        return;
    }
    ImGui::Text("%s", backendDisplayName(a.config().backend));
    ImGui::SameLine();
    ImGui::TextDisabled("|  %u Hz  |  %u frames  |  %.1f ms  |  DSP %.1f%%  |  underruns %llu",
                        e.sampleRate(), e.bufferFrames(), e.latencyMs(),
                        static_cast<double>(e.dspLoad()) * 100.0,
                        static_cast<unsigned long long>(e.underruns()));
    if (e.clipLatched()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
        ImGui::TextUnformatted(" CLIP");
        ImGui::PopStyleColor();
    }
    if (e.muted()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
        ImGui::TextUnformatted(" MUTED");
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------------------------------------------------
// Shortcuts
// ---------------------------------------------------------------------------------------------------------

void handleAudioShortcuts(ShellContext& ctx) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return;
    const bool popupOpen =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    Engine& e = ctx.audio->engine();
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        ctx.openFileRequested = true;
    if (popupOpen)
        return;
    // Space plays/pauses unless keyboard navigation is driving a focused widget (Space would also click it).
    if (!io.NavVisible && ImGui::IsKeyPressed(ImGuiKey_Space, false) && e.hasFile() &&
        ctx.audio->source() == SourceKind::File)
        togglePlay(ctx);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        e.setMute(!e.muted()); // Esc = Mute (toggle)
}

} // namespace dsptb::ui
