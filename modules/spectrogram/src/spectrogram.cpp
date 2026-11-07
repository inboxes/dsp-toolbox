// Spectrogram (org.dsptb.spectrogram).
//
// The maths (STFT, bin-to-row mapping, colormaps, note names) is dsptb::dsp_common; this file wires it up:
//   analysis thread: analyze() -> mixToMono -> StftAnalyzer -> BinToRowMap -> dbToRgba -> column ring
//   UI thread:       draw() drains the column ring into a circular GPU texture and draws it with axes
//
// Thread contract (MODULE_SDK): analyze() runs on the analysis thread, draw()/saveState() on the UI thread.
// They only share the column ring (SPSC), the spectrum profile (relaxed atomics) and a few atomics.

#include <dsptb/dsp_common/colormap.hpp>
#include <dsptb/dsp_common/freq_map.hpp>
#include <dsptb/dsp_common/stft.hpp>
#include <dsptb/rt/spsc_ring.hpp>
#include <dsptb/sdk.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

DSPTB_DEFINE_RT_GUARD();

namespace {

namespace dc = dsptb::dsp;

constexpr uint32_t kRows = 512;           // H: output rows (spec: H = 512)
constexpr uint32_t kMaxTexColumns = 4096; // texture width cap (spec)
constexpr size_t kRingColumns = 4096;
constexpr size_t kMaxSummaries = 65536;
constexpr float kProfileDecayS = 0.3f;

constexpr std::array<size_t, 5> kFftSizes{512, 1024, 2048, 4096, 8192};
constexpr std::array<double, 3> kOverlaps{0.5, 0.75, 0.875};

enum Param : uint32_t {
    kFftSize,
    kOverlap,
    kWindow,
    kChannel,
    kFreqScale,
    kMinDb,
    kMaxDb,
    kMinHz,
    kMaxHz,
    kHistory,
    kColormap,
    kShowSpectrum,
    kFreeze,
};

constexpr const char* kFftLabels[] = {"512", "1024", "2048", "4096", "8192"};
constexpr const char* kOverlapLabels[] = {"50 %", "75 %", "87.5 %"};
constexpr const char* kWindowLabels[] = {"Hann", "Blackman-Harris"};
constexpr const char* kChannelLabels[] = {"Mid (L+R)/2", "Left", "Right", "Side (L-R)/2"};
constexpr const char* kScaleLabels[] = {"Linear", "Log", "Mel"};
constexpr const char* kMapLabels[] = {"Magma", "Viridis", "Inferno", "Grey"};

// What the picture depends on. A change in any field of `Axis` starts a new texture generation.
struct Axis {
    float sampleRate = 48000.0f;
    float minHz = 20.0f, maxHz = 20000.0f;
    float minDb = -100.0f, maxDb = 0.0f;
    float historyS = 10.0f;
    uint8_t scale = 1, colormap = 0;
    uint8_t pad[2] = {0, 0};
};

enum ColumnFlags : uint32_t { kGapColumn = 1 };

// One image column, top row first, as handed from the analysis thread to the UI thread.
struct Column {
    uint32_t gen = 0;  // texture generation; the UI starts over when it changes
    uint32_t texW = 0; // texture width for this generation
    uint32_t flags = 0;
    Axis axis;
    uint32_t px[kRows];
};

struct Summary {
    uint64_t frame;
    float peakHz, peakDb, binDb;
};

// Everything that makes the analysis thread rebuild its STFT / frequency map.
struct Config {
    size_t fftSize = 4096;
    size_t hop = 1024;
    dc::Window window = dc::Window::Hann;
    dc::ChannelMix mix = dc::ChannelMix::Mid;
    dc::FreqScale scale = dc::FreqScale::Log;
    double sampleRate = 48000.0;
    double minHz = 20.0, maxHz = 20000.0;
    double historyS = 10.0;
    bool operator==(const Config&) const = default;
};

int roundInt(float v) {
    return static_cast<int>(v + (v < 0 ? -0.5f : 0.5f));
}

} // namespace

class Spectrogram final : public dsptb::Analyzer<Spectrogram> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "org.dsptb.spectrogram",
                                            .name = "Spectrogram",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox",
                                            .description = "Live scrolling spectrogram.",
                                            .caps = 0,
                                            .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::params(
        dsptb::Enum("fft_size", "FFT size", kFftLabels, 3),
        dsptb::Enum("overlap", "Overlap", kOverlapLabels, 1),
        dsptb::Enum("window", "Window", kWindowLabels, 0),
        dsptb::Enum("channel", "Channel", kChannelLabels, 0),
        dsptb::Enum("freq_scale", "Frequency axis", kScaleLabels, 1),
        dsptb::Float("min_db", "Min level", "dB", -140.f, 0.f, -100.f),
        dsptb::Float("max_db", "Max level", "dB", -140.f, 0.f, 0.f),
        dsptb::Float("min_hz", "Min frequency", "Hz", 10.f, 24000.f, 20.f).log(),
        dsptb::Float("max_hz", "Max frequency", "Hz", 10.f, 24000.f, 20000.f).log(),
        dsptb::Float("history_s", "History", "s", 2.f, 60.f, 10.f),
        dsptb::Enum("colormap", "Colormap", kMapLabels, 0),
        dsptb::Bool("show_spectrum", "Spectrum line", true), dsptb::Bool("freeze", "Freeze", false));

    Spectrogram() : ring_(kRingColumns) {
        for (auto& v : latest_)
            v.store(-200.0f, std::memory_order_relaxed);
    }

    bool prepare(const dsptb::PrepareInfo& p) override {
        sampleRate_ = p.sampleRate;
        offline_ = p.offline;
        configured_ = false; // rebuilt at the first analyze() for the new rate
        summaries_.clear();
        if (offline_)
            summaries_.reserve(4096);
        return true;
    }

    // ------------------------------------------------------------------------------------------------
    // analysis thread
    // ------------------------------------------------------------------------------------------------
    void analyze(dsptb::AnalyzeBlock& b) override {
        const Config cfg = readConfig(b);
        if (!configured_ || !(cfg == cfg_))
            reconfigure(cfg);
        minDb_ = b.param(kMinDb);
        maxDb_ = std::max(b.param(kMaxDb), minDb_ + 1.0f);
        colormap_ = static_cast<dc::Colormap>(std::clamp(roundInt(b.param(kColormap)), 0, 3));
        axis_.minDb = minDb_;
        axis_.maxDb = maxDb_;
        axis_.colormap = static_cast<uint8_t>(colormap_);
        const bool frozen = b.param(kFreeze) >= 0.5f;

        if (b.droppedFrames() > 0) {
            // Samples are missing: start the frame stream over and show it.
            stft_->reset();
            accCount_ = 0;
            if (!frozen)
                pushGap();
        }
        if (frozen != wasFrozen_) {
            wasFrozen_ = frozen;
            stft_->reset();
            accCount_ = 0;
        }

        const uint32_t n = b.frames();
        if (mono_.size() < n)
            mono_.resize(n);
        dc::mixToMono(b.channel(0), b.channel(1), n, cfg.mix, mono_.data());
        for (uint32_t i = 0; i < n; ++i) // a NaN/Inf in the stream would poison every frame that overlaps it
            if (!std::isfinite(mono_[i]))
                mono_[i] = 0.0f;
        stft_->push(mono_.data(), n,
                    [&](const float* db, size_t bins, uint64_t frame) { onFrame(db, bins, frame, frozen); });
    }

    // ------------------------------------------------------------------------------------------------
    // optional dump for `dsptb-cli analyze --dump-frames`
    // ------------------------------------------------------------------------------------------------
    std::string dumpFrames() override {
        std::string out;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"module\":\"org.dsptb.spectrogram\",\"sampleRate\":%.1f,\"fftSize\":%zu,\"hop\":%zu,"
                      "\"binHz\":%.6f,\"frames\":[",
                      cfg_.sampleRate, cfg_.fftSize, cfg_.hop,
                      cfg_.sampleRate / static_cast<double>(cfg_.fftSize));
        out += buf;
        for (size_t i = 0; i < summaries_.size(); ++i) {
            const Summary& s = summaries_[i];
            std::snprintf(
                buf, sizeof(buf),
                "%s{\"index\":%llu,\"timeS\":%.6f,\"peakHz\":%.4f,\"peakDb\":%.4f,\"peakBinDb\":%.4f}",
                i ? "," : "", static_cast<unsigned long long>(s.frame),
                static_cast<double>(s.frame) * static_cast<double>(cfg_.hop) / cfg_.sampleRate,
                static_cast<double>(s.peakHz), static_cast<double>(s.peakDb), static_cast<double>(s.binDb));
            out += buf;
        }
        out += "]}";
        return out;
    }

    // ------------------------------------------------------------------------------------------------
    // UI thread
    // ------------------------------------------------------------------------------------------------
    void draw(dsptb::Canvas& c) override {
        drainColumns(c);

        const float s = c.uiScale();
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 260.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 220.0f * s, 900.0f * s);
        auto region = c.region("spectrogram_view", w, h);
        if (!region)
            return;

        const float left = 56.0f * s, right = 64.0f * s, bottom = 22.0f * s;
        const float px0 = left, py0 = 4.0f * s;
        const float pw = std::max(40.0f, w - left - right);
        const float ph = std::max(40.0f, h - bottom - py0);

        c.fillRect({px0, py0}, {px0 + pw, py0 + ph}, dsptb::rgba(8, 8, 12));
        {
            auto clip = c.clip({px0, py0}, {px0 + pw, py0 + ph});
            drawImage(c, px0, py0, pw, ph);
            if (host().valid() && host().getParam(kShowSpectrum) >= 0.5f)
                drawProfile(c, px0, py0, pw, ph);
        }
        c.rect({px0, py0}, {px0 + pw, py0 + ph}, dsptb::rgba(90, 94, 104), 1.0f);

        drawFreqAxis(c, s, px0, py0, ph);
        drawTimeAxis(c, s, px0, py0 + ph, pw);
        drawColorBar(c, s, px0 + pw + 10.0f * s, py0, ph);
        drawHover(c, s, px0, py0, pw, ph);
    }

    std::string saveState() override { return "{}"; }

private:
    // ---- analysis-thread helpers ----------------------------------------------------------------
    Config readConfig(const dsptb::AnalyzeBlock& b) const {
        Config cfg;
        cfg.fftSize = kFftSizes[static_cast<size_t>(std::clamp(roundInt(b.param(kFftSize)), 0, 4))];
        const double overlap = kOverlaps[static_cast<size_t>(std::clamp(roundInt(b.param(kOverlap)), 0, 2))];
        cfg.hop = std::max<size_t>(
            1, static_cast<size_t>(std::lround(static_cast<double>(cfg.fftSize) * (1.0 - overlap))));
        cfg.window = roundInt(b.param(kWindow)) == 1 ? dc::Window::BlackmanHarris : dc::Window::Hann;
        cfg.mix = static_cast<dc::ChannelMix>(std::clamp(roundInt(b.param(kChannel)), 0, 3));
        cfg.scale = static_cast<dc::FreqScale>(std::clamp(roundInt(b.param(kFreqScale)), 0, 2));
        cfg.sampleRate = b.sampleRate();
        const double nyquist = 0.5 * cfg.sampleRate;
        double lo = b.param(kMinHz), hi = b.param(kMaxHz);
        hi = std::min(hi, nyquist * 0.999);
        if (hi <= lo * 1.05)
            lo = hi / 1.5; // keep a usable axis whatever the two sliders say
        cfg.minHz = std::max(lo, 1.0);
        cfg.maxHz = hi;
        cfg.historyS = b.param(kHistory);
        return cfg;
    }

    void reconfigure(const Config& cfg) {
        if (!stft_)
            stft_ = std::make_unique<dc::StftAnalyzer>(dc::StftConfig{cfg.fftSize, cfg.hop, cfg.window});
        else
            stft_->configure(dc::StftConfig{cfg.fftSize, cfg.hop, cfg.window});
        map_ = std::make_unique<dc::BinToRowMap>(cfg.fftSize, cfg.sampleRate, kRows, cfg.scale, cfg.minHz,
                                                 cfg.maxHz);
        rowDb_.assign(kRows, -200.0f);
        acc_.assign(kRows, -200.0f);
        accCount_ = 0;

        // Texture geometry: one column per `combine` frames so the history fits the width cap.
        const double framesPerSecond = cfg.sampleRate / static_cast<double>(cfg.hop);
        const double totalFrames = std::max(1.0, cfg.historyS * framesPerSecond);
        combine_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(totalFrames / kMaxTexColumns)));
        texW_ = std::max<uint32_t>(16, static_cast<uint32_t>(std::ceil(totalFrames / combine_)));

        const bool axisChanged = !configured_ || cfg.scale != cfg_.scale || cfg.minHz != cfg_.minHz ||
                                 cfg.maxHz != cfg_.maxHz || cfg.historyS != cfg_.historyS ||
                                 cfg.sampleRate != cfg_.sampleRate || cfg.fftSize != cfg_.fftSize ||
                                 cfg.hop != cfg_.hop;
        if (axisChanged)
            ++gen_;
        cfg_ = cfg;
        configured_ = true;
        axis_.sampleRate = static_cast<float>(cfg.sampleRate);
        axis_.minHz = static_cast<float>(cfg.minHz);
        axis_.maxHz = static_cast<float>(cfg.maxHz);
        axis_.historyS = static_cast<float>(cfg.historyS);
        axis_.scale = static_cast<uint8_t>(cfg.scale);
    }

    void onFrame(const float* db, size_t bins, uint64_t frame, bool frozen) {
        if (offline_ && summaries_.size() < kMaxSummaries)
            summaries_.push_back(peakOf(db, bins, frame));
        if (frozen)
            return;
        map_->apply(db, rowDb_.data());
        for (uint32_t r = 0; r < kRows; ++r)
            latest_[r].store(rowDb_[r], std::memory_order_relaxed);
        if (accCount_ == 0) {
            acc_ = rowDb_;
        } else {
            for (uint32_t r = 0; r < kRows; ++r)
                acc_[r] = std::max(acc_[r], rowDb_[r]); // columns merged by max when the history is long
        }
        if (++accCount_ >= combine_)
            pushColumn();
    }

    void pushColumn() {
        accCount_ = 0;
        Column col;
        col.gen = gen_;
        col.texW = texW_;
        col.axis = axis_;
        std::array<uint32_t, kRows> bottomUp{};
        dc::dbToRgba(acc_.data(), kRows, minDb_, maxDb_, colormap_, bottomUp.data());
        for (uint32_t r = 0; r < kRows; ++r)
            col.px[kRows - 1 - r] = bottomUp[r]; // texture row 0 is the top
        ring_.tryPush(col);
    }

    void pushGap() {
        Column col;
        col.gen = gen_;
        col.texW = texW_;
        col.axis = axis_;
        col.flags = kGapColumn;
        const uint32_t dark = 0xFF000000u; // R,G,B,A with A = 255
        for (uint32_t r = 0; r < kRows; ++r)
            col.px[r] = dark;
        for (uint32_t r = 0; r < kRows / 32; ++r)
            col.px[r] = dsptb::rgba(190, 20, 20); // dark red tick at the top
        ring_.tryPush(col);
    }

    Summary peakOf(const float* db, size_t bins, uint64_t frame) const {
        size_t k = 1;
        for (size_t i = 2; i + 1 < bins; ++i)
            if (db[i] > db[k])
                k = i;
        const double y1 = db[k - 1], y2 = db[k], y3 = db[k + 1];
        const double denom = y1 - 2.0 * y2 + y3;
        const double d = std::fabs(denom) > 1e-12 ? 0.5 * (y1 - y3) / denom : 0.0; // parabolic interpolation
        const double binHz = cfg_.sampleRate / static_cast<double>(cfg_.fftSize);
        Summary s;
        s.frame = frame;
        s.peakHz = static_cast<float>((static_cast<double>(k) + d) * binHz);
        s.peakDb = static_cast<float>(y2 - 0.25 * (y1 - y3) * d);
        s.binDb = static_cast<float>(y2);
        return s;
    }

    // ---- UI-thread helpers -----------------------------------------------------------------------
    void drainColumns(dsptb::Canvas& c) {
        // If the panel was hidden for a long time, only the newest `texW_` columns can matter.
        size_t pending = ring_.size();
        const size_t keep = texW_ ? texW_ : kMaxTexColumns;
        Column col;
        while (pending > keep) {
            ring_.tryPop(col);
            --pending;
        }
        while (ring_.tryPop(col)) {
            if (col.gen != gen_ui_ || tex_ == 0 || col.texW != texW_ui_)
                startGeneration(c, col);
            axisUi_ = col.axis;
            batch_.insert(batch_.end(), col.px, col.px + kRows);
            if (batch_.size() >= 64u * kRows)
                flushBatch(c);
        }
        flushBatch(c);
    }

    void startGeneration(dsptb::Canvas& c, const Column& col) {
        flushBatch(c);
        if (tex_)
            c.textureDestroy(tex_);
        gen_ui_ = col.gen;
        texW_ui_ = col.texW;
        wp_ = 0;
        hist_.assign(static_cast<size_t>(texW_ui_) * kRows, 0xFF000000u);
        tex_ = c.textureCreate(texW_ui_, kRows);
        if (tex_)
            c.textureUpdate(tex_, 0, 0, texW_ui_, kRows, hist_.data(), texW_ui_ * 4);
    }

    // Writes the queued columns into the CPU history and the texture (split where the ring wraps).
    void flushBatch(dsptb::Canvas& c) {
        const size_t total = batch_.size() / kRows;
        size_t done = 0;
        while (done < total && texW_ui_ > 0) {
            const uint32_t run = std::min<uint32_t>(static_cast<uint32_t>(total - done), texW_ui_ - wp_);
            staging_.resize(static_cast<size_t>(run) * kRows);
            for (uint32_t i = 0; i < run; ++i)
                for (uint32_t y = 0; y < kRows; ++y) {
                    const uint32_t v = batch_[(done + i) * kRows + y];
                    staging_[static_cast<size_t>(y) * run + i] = v;
                    hist_[static_cast<size_t>(y) * texW_ui_ + wp_ + i] = v;
                }
            if (tex_)
                c.textureUpdate(tex_, wp_, 0, run, kRows, staging_.data(), run * 4);
            wp_ = (wp_ + run) % texW_ui_;
            done += run;
        }
        batch_.clear();
    }

    void drawImage(dsptb::Canvas& c, float x0, float y0, float pw, float ph) const {
        if (!tex_ || texW_ui_ == 0)
            return;
        const float tw = static_cast<float>(texW_ui_);
        const float oldest = static_cast<float>(wp_) / tw;
        const float leftW = pw * (1.0f - oldest);
        if (wp_ < texW_ui_ && leftW > 0.5f)
            c.image(tex_, {x0, y0}, {x0 + leftW, y0 + ph}, {oldest, 0}, {1, 1});
        if (wp_ > 0)
            c.image(tex_, {x0 + leftW, y0}, {x0 + pw, y0 + ph}, {0, 0}, {oldest, 1});
    }

    // The newest spectrum as a profile along the right edge: frequency stays vertical so it lines up with
    // the axis. Smoothed with a 0.3 s peak decay over the whole level range.
    void drawProfile(dsptb::Canvas& c, float x0, float y0, float pw, float ph) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.25f, std::chrono::duration<float>(now - lastDraw_).count());
        lastDraw_ = now;
        const float range = std::max(1.0f, axisUi_.maxDb - axisUi_.minDb);
        const float fall = range / kProfileDecayS * dt;
        if (profile_.size() != kRows)
            profile_.assign(kRows, -200.0f);
        for (uint32_t r = 0; r < kRows; ++r)
            profile_[r] = std::max(latest_[r].load(std::memory_order_relaxed), profile_[r] - fall);
        const float widthMax = pw * 0.22f;
        pts_.resize(kRows);
        for (uint32_t r = 0; r < kRows; ++r) {
            const float f = std::clamp((profile_[r] - axisUi_.minDb) / range, 0.0f, 1.0f);
            pts_[r] = {x0 + pw - widthMax * f, y0 + ph * (1.0f - (static_cast<float>(r) + 0.5f) / kRows)};
        }
        c.polyline(pts_.data(), kRows, dsptb::rgba(255, 255, 255, 220), 1.5f);
    }

    std::unique_ptr<dc::BinToRowMap> uiMap() const {
        try {
            return std::make_unique<dc::BinToRowMap>(2048, axisUi_.sampleRate, kRows,
                                                     static_cast<dc::FreqScale>(axisUi_.scale), axisUi_.minHz,
                                                     axisUi_.maxHz);
        } catch (...) {
            return nullptr;
        }
    }

    void drawFreqAxis(dsptb::Canvas& c, float s, float x0, float y0, float ph) {
        if (!tex_)
            return;
        const auto map = uiMap();
        if (!map)
            return;
        static constexpr double kTicks[] = {20,   30,   50,   100,  200,   300,   500,  1000,
                                            2000, 3000, 5000, 8000, 10000, 15000, 20000};
        for (double hz : kTicks) {
            if (hz < axisUi_.minHz || hz > axisUi_.maxHz)
                continue;
            const float y = y0 + ph * (1.0f - static_cast<float>(map->rowToFraction(hz)));
            c.line({x0 - 4.0f * s, y}, {x0, y}, dsptb::rgba(170, 174, 184), 1.0f);
            c.line({x0, y}, {x0 + 6.0f * s, y}, dsptb::rgba(255, 255, 255, 90), 1.0f);
            char label[16];
            if (hz >= 1000.0)
                std::snprintf(label, sizeof(label), "%g k", hz / 1000.0);
            else
                std::snprintf(label, sizeof(label), "%g", hz);
            c.text({x0 - 8.0f * s - 7.0f * s * static_cast<float>(std::strlen(label)), y - 7.0f * s},
                   dsptb::rgba(200, 204, 214), label);
        }
    }

    void drawTimeAxis(dsptb::Canvas& c, float s, float x0, float y, float pw) const {
        if (!tex_)
            return;
        const float history = axisUi_.historyS;
        static constexpr float kSteps[] = {1, 2, 5, 10, 15, 30, 60};
        float step = kSteps[6];
        for (float st : kSteps)
            if (history / st <= 8.0f) {
                step = st;
                break;
            }
        for (float t = 0.0f; t <= history + 1e-3f; t += step) {
            const float x = x0 + pw * (1.0f - t / history);
            c.line({x, y}, {x, y + 4.0f * s}, dsptb::rgba(170, 174, 184), 1.0f);
            char label[16];
            std::snprintf(label, sizeof(label), t == 0.0f ? "0 s" : "-%g s", static_cast<double>(t));
            c.text({x - 10.0f * s, y + 6.0f * s}, dsptb::rgba(200, 204, 214), label);
        }
    }

    void drawColorBar(dsptb::Canvas& c, float s, float x0, float y0, float ph) const {
        const auto& lut = dc::colormapLut(static_cast<dc::Colormap>(axisUi_.colormap));
        constexpr int kSegments = 64;
        const float bw = 12.0f * s;
        for (int i = 0; i < kSegments; ++i) {
            const float t0 = static_cast<float>(i) / kSegments;
            const float y = y0 + ph * (1.0f - (t0 + 1.0f / kSegments));
            c.fillRect({x0, y}, {x0 + bw, y + ph / kSegments + 1.0f}, lut[static_cast<size_t>(t0 * 255.0f)]);
        }
        c.rect({x0, y0}, {x0 + bw, y0 + ph}, dsptb::rgba(90, 94, 104), 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f dB", static_cast<double>(axisUi_.maxDb));
        c.text({x0 + bw + 4.0f * s, y0 - 2.0f * s}, dsptb::rgba(200, 204, 214), label);
        std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(axisUi_.minDb));
        c.text({x0 + bw + 4.0f * s, y0 + ph - 14.0f * s}, dsptb::rgba(200, 204, 214), label);
    }

    // Crosshair with frequency (plus note name), time and level under the mouse.
    void drawHover(dsptb::Canvas& c, float s, float x0, float y0, float pw, float ph) {
        if (!tex_ || texW_ui_ == 0)
            return;
        const dsptb_mouse m = c.mouse();
        if (!m.hovered)
            return;
        const float mx = m.pos.x - x0, my = m.pos.y - y0;
        if (mx < 0 || my < 0 || mx >= pw || my >= ph)
            return;
        const auto map = uiMap();
        if (!map)
            return;
        const float frac = 1.0f - my / ph;
        const size_t row = std::min<size_t>(kRows - 1, static_cast<size_t>(frac * kRows));
        const double hz = map->rowCenterHz(row);
        const float ageS = axisUi_.historyS * (1.0f - mx / pw);

        // Level: read the colour back from the CPU history through the colormap.
        const float col = mx / pw;
        const uint32_t texX = (wp_ + static_cast<uint32_t>(col * static_cast<float>(texW_ui_))) % texW_ui_;
        const uint32_t texY = static_cast<uint32_t>(kRows - 1 - row);
        const uint32_t pixel = hist_[static_cast<size_t>(texY) * texW_ui_ + texX];
        const auto& lut = dc::colormapLut(static_cast<dc::Colormap>(axisUi_.colormap));
        size_t best = 0;
        int bestDist = 1 << 30;
        for (size_t i = 0; i < lut.size(); ++i) {
            const int dr = static_cast<int>(lut[i] & 0xFF) - static_cast<int>(pixel & 0xFF);
            const int dg = static_cast<int>((lut[i] >> 8) & 0xFF) - static_cast<int>((pixel >> 8) & 0xFF);
            const int db = static_cast<int>((lut[i] >> 16) & 0xFF) - static_cast<int>((pixel >> 16) & 0xFF);
            const int dist = dr * dr + dg * dg + db * db;
            if (dist < bestDist) {
                bestDist = dist;
                best = i;
            }
        }
        const float level =
            axisUi_.minDb + (axisUi_.maxDb - axisUi_.minDb) * static_cast<float>(best) / 255.0f;

        c.line({m.pos.x, y0}, {m.pos.x, y0 + ph}, dsptb::rgba(255, 255, 255, 110), 1.0f);
        c.line({x0, m.pos.y}, {x0 + pw, m.pos.y}, dsptb::rgba(255, 255, 255, 110), 1.0f);
        char note[24];
        dc::noteName(hz, note, sizeof(note));
        char text[96];
        std::snprintf(text, sizeof(text), "%.1f Hz  %s   -%.2f s   %.0f dB", hz, note,
                      static_cast<double>(ageS), static_cast<double>(level));
        const float tx = std::min(m.pos.x + 10.0f * s, x0 + pw - 260.0f * s);
        const float ty = std::min(m.pos.y + 12.0f * s, y0 + ph - 20.0f * s);
        c.fillRect({tx - 3.0f * s, ty - 2.0f * s}, {tx + 262.0f * s, ty + 16.0f * s},
                   dsptb::rgba(0, 0, 0, 190));
        c.text({tx, ty}, dsptb::rgba(255, 255, 255), text);
    }

    // ---- shared / analysis state ------------------------------------------------------------------
    double sampleRate_ = 48000.0;
    bool offline_ = false;
    bool configured_ = false;
    bool wasFrozen_ = false;
    Config cfg_;
    Axis axis_;
    float minDb_ = -100.0f, maxDb_ = 0.0f;
    dc::Colormap colormap_ = dc::Colormap::Magma;
    uint32_t gen_ = 0, texW_ = 0, combine_ = 1, accCount_ = 0;
    std::unique_ptr<dc::StftAnalyzer> stft_;
    std::unique_ptr<dc::BinToRowMap> map_;
    std::vector<float> mono_, rowDb_, acc_;
    std::vector<Summary> summaries_;

    dsptb::rt::SpscRing<Column> ring_;
    std::array<std::atomic<float>, kRows> latest_;

    // ---- UI state ---------------------------------------------------------------------------------
    dsptb_texture tex_ = 0;
    uint32_t gen_ui_ = 0, texW_ui_ = 0, wp_ = 0;
    Axis axisUi_;
    std::vector<uint32_t> hist_, staging_;
    std::vector<uint32_t> batch_; // queued columns, kRows values each
    std::vector<float> profile_;
    std::vector<dsptb_vec2> pts_;
    std::chrono::steady_clock::time_point lastDraw_ = std::chrono::steady_clock::now();
};

DSPTB_REGISTER_MODULES(Spectrogram);
