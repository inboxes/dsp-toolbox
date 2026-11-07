// Level / Loudness Meter (org.dsptb.levelmeter).
//
// All measurement maths is dsptb::dsp_common (LoudnessMeter = libebur128, LevelMeter). This file feeds blocks
// from analyze(), publishes readings at about 10 Hz through a TripleBuffer, keeps the short-term loudness
// history, and draws the meters.
//
// Threads: analyze() on the analysis thread owns the two meters. draw() on the UI thread reads the published
// readings and the history ring. "Reset" is a flag the analysis thread picks up (LoudnessMeter::reset
// allocates, so it must not run on the UI thread while analyze() is using the same object).

#include <dsptb/dsp_common/loudness.hpp>
#include <dsptb/rt/spsc_ring.hpp>
#include <dsptb/rt/triple_buffer.hpp>
#include <dsptb/sdk.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

DSPTB_DEFINE_RT_GUARD();

namespace {

namespace dc = dsptb::dsp;

enum Param : uint32_t { kRmsWindow, kPeakHold, kTruePeak, kTarget, kHistory };

constexpr double kPublishPeriodS = 0.1; // readings and history points at 10 Hz
constexpr float kNoSignal = -200.0f;    // stands in for -inf in plain structs and JSON

float finite(double v) {
    return std::isfinite(v) ? static_cast<float>(v) : kNoSignal;
}

struct Readings {
    float peakDb[2] = {kNoSignal, kNoSignal}; // with hold + fall
    float rmsDb[2] = {kNoSignal, kNoSignal};
    float samplePeakDb[2] = {kNoSignal, kNoSignal}; // max since reset
    float truePeakDb[2] = {kNoSignal, kNoSignal};   // max since reset
    uint8_t clip[2] = {0, 0};
    float momentary = kNoSignal, shortTerm = kNoSignal, integrated = kNoSignal, range = 0.0f;
    uint32_t resetCount = 0; // bumps when the analysis thread has applied a reset
};

} // namespace

class LevelMeterModule final : public dsptb::Analyzer<LevelMeterModule> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "org.dsptb.levelmeter",
                                            .name = "Level / Loudness Meter",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox",
                                            .description = "Peak, RMS, true peak and EBU R128 loudness.",
                                            .caps = 0,
                                            .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params =
        dsptb::params(dsptb::Float("rms_window_ms", "RMS window", "ms", 50.f, 3000.f, 300.f).log(),
                      dsptb::Float("peak_hold_s", "Peak hold", "s", 0.f, 10.f, 2.f),
                      dsptb::Bool("true_peak", "True peak (4x oversampled)", true),
                      dsptb::Float("target_lufs", "Target", "LUFS", -40.f, 0.f, -23.f),
                      dsptb::Float("history_s", "History", "s", 10.f, 600.f, 60.f));

    LevelMeterModule() : history_(8192), readings_(Readings{}) {}

    bool prepare(const dsptb::PrepareInfo& p) override {
        try {
            sampleRate_ = p.sampleRate;
            loudness_ = std::make_unique<dc::LoudnessMeter>(p.sampleRate, 2, 4096);
            level_ = std::make_unique<dc::LevelMeter>(p.sampleRate, 2, 300.0, 2.0);
            appliedRms_ = 300.0;
            sinceLastPublish_ = 0;
            return true;
        } catch (...) {
            return false;
        }
    }

    // ------------------------------------------------------------------------------------------------
    // analysis thread
    // ------------------------------------------------------------------------------------------------
    void analyze(dsptb::AnalyzeBlock& b) override {
        if (!loudness_ || !level_)
            return;
        const uint32_t resetWanted = resetRequests_.load(std::memory_order_acquire);
        if (resetWanted != resetDone_) {
            loudness_->reset();
            level_->clearClip();
            resetDone_ = resetWanted;
            resetApplied_ = true;
        }
        const double rmsMs = b.param(kRmsWindow);
        if (std::fabs(rmsMs - appliedRms_) > 0.5) {
            level_->setRmsWindowMs(rmsMs);
            appliedRms_ = rmsMs;
        }
        level_->setPeakHoldS(b.param(kPeakHold));

        // NaN/Inf would stay in the filter and running sums for good, so they are replaced by silence.
        const uint32_t n = b.frames();
        for (int c = 0; c < 2; ++c) {
            clean_[c].resize(n);
            const float* src = b.channel(static_cast<uint32_t>(c));
            for (uint32_t i = 0; i < n; ++i)
                clean_[c][i] = std::isfinite(src[i]) ? src[i] : 0.0f;
        }
        const float* ch[2] = {clean_[0].data(), clean_[1].data()};
        loudness_->process(ch, n);
        level_->process(ch, n);

        sinceLastPublish_ += static_cast<double>(b.frames()) / b.sampleRate();
        if (sinceLastPublish_ >= kPublishPeriodS) {
            sinceLastPublish_ = std::fmod(sinceLastPublish_, kPublishPeriodS);
            publish();
        }
    }

    std::string dumpFrames() override {
        if (!loudness_ || !level_)
            return {};
        publish(); // final readings, whatever the 10 Hz timer says
        const Readings& r = last_;
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
                      "{\"module\":\"org.dsptb.levelmeter\",\"sampleRate\":%.1f,"
                      "\"momentaryLufs\":%.4f,\"shortTermLufs\":%.4f,\"integratedLufs\":%.4f,"
                      "\"loudnessRangeLu\":%.4f,"
                      "\"truePeakDbtp\":[%.4f,%.4f],\"samplePeakDbfs\":[%.4f,%.4f],"
                      "\"peakDbfs\":[%.4f,%.4f],\"rmsDbfs\":[%.4f,%.4f],\"clip\":[%d,%d],"
                      "\"shortTermHistory\":[",
                      sampleRate_, double(r.momentary), double(r.shortTerm), double(r.integrated),
                      double(r.range), double(r.truePeakDb[0]), double(r.truePeakDb[1]),
                      double(r.samplePeakDb[0]), double(r.samplePeakDb[1]), double(r.peakDb[0]),
                      double(r.peakDb[1]), double(r.rmsDb[0]), double(r.rmsDb[1]), r.clip[0], r.clip[1]);
        std::string out = buf;
        for (size_t i = 0; i < dumpHistory_.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s%.3f", i ? "," : "", double(dumpHistory_[i]));
            out += buf;
        }
        out += "]}";
        return out;
    }

    // ------------------------------------------------------------------------------------------------
    // UI thread
    // ------------------------------------------------------------------------------------------------
    void draw(dsptb::Canvas& c) override {
        readings_.update();
        const Readings& r = readings_.read();
        float v;
        while (history_.tryPop(v))
            shortTermUi_.push_back(v);
        const float histLimit = host().valid() ? host().getParam(kHistory) : 60.0f;
        const size_t maxPoints = static_cast<size_t>(std::max(10.0f, histLimit) / kPublishPeriodS);
        if (shortTermUi_.size() > maxPoints)
            shortTermUi_.erase(shortTermUi_.begin(),
                               shortTermUi_.begin() +
                                   static_cast<std::ptrdiff_t>(shortTermUi_.size() - maxPoints));
        if (r.resetCount != seenReset_) { // the integrated measurement restarted: so does the plot
            seenReset_ = r.resetCount;
            shortTermUi_.clear();
        }

        const float s = c.uiScale();
        if (c.button("Reset")) {
            resetRequests_.fetch_add(1, std::memory_order_release);
        }
        c.sameLine();
        c.label("clears integrated loudness, range, maxima and clip lamps");

        const bool showTp = host().valid() ? host().getParam(kTruePeak) >= 0.5f : true;
        const float target = host().valid() ? host().getParam(kTarget) : -23.0f;
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 300.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 260.0f * s, 700.0f * s);
        auto region = c.region("level_view", w, h);
        if (!region)
            return;

        const float barsW = 150.0f * s;
        drawBars(c, s, r, 4.0f * s, 4.0f * s, barsW, h - 8.0f * s);
        drawReadouts(c, s, r, showTp, barsW + 18.0f * s, 4.0f * s, w - barsW - 22.0f * s);
        drawHistory(c, s, target, barsW + 18.0f * s, 4.0f * s + 118.0f * s, w - barsW - 22.0f * s,
                    std::max(60.0f * s, h - 8.0f * s - 118.0f * s));
    }

    std::string saveState() override { return "{}"; }

private:
    void publish() {
        Readings& r = readings_.writeSlot();
        for (unsigned ch = 0; ch < 2; ++ch) {
            r.peakDb[ch] = finite(level_->peakDbfs(ch));
            r.rmsDb[ch] = finite(level_->rmsDbfs(ch));
            r.clip[ch] = level_->clipped(ch) ? 1 : 0;
            r.samplePeakDb[ch] = finite(loudness_->samplePeakDbfs(ch));
            r.truePeakDb[ch] = finite(loudness_->truePeakDbtp(ch));
        }
        r.momentary = finite(loudness_->momentaryLufs());
        r.shortTerm = finite(loudness_->shortTermLufs());
        r.integrated = finite(loudness_->integratedLufs());
        r.range = static_cast<float>(loudness_->loudnessRangeLu());
        if (resetApplied_) {
            ++resetCountPub_;
            resetApplied_ = false;
            dumpHistory_.clear();
        }
        r.resetCount = resetCountPub_;
        last_ = r;
        readings_.publish();
        history_.tryPush(r.shortTerm);
        if (dumpHistory_.size() < 1u << 20)
            dumpHistory_.push_back(r.shortTerm);
    }

    static float dbToFrac(float db, float lo, float hi) {
        return std::clamp((db - lo) / (hi - lo), 0.0f, 1.0f);
    }

    void drawBars(dsptb::Canvas& c, float s, const Readings& r, float x0, float y0, float w, float h) const {
        const float lo = -60.0f, hi = 3.0f;
        const float labelH = 16.0f * s;
        const float top = y0 + 4.0f * s, bottom = y0 + h - labelH;
        const float barW = 38.0f * s;
        const float xs[2] = {x0 + 40.0f * s, x0 + 40.0f * s + barW + 18.0f * s};
        c.fillRect({x0, y0}, {x0 + w, y0 + h}, dsptb::rgba(18, 19, 24));
        // scale
        for (int db = -60; db <= 0; db += 10) {
            const float y = bottom - (bottom - top) * dbToFrac(static_cast<float>(db), lo, hi);
            c.line({x0 + 34.0f * s, y}, {x0 + w - 4.0f * s, y}, dsptb::rgba(255, 255, 255, 40), 1.0f);
            char t[8];
            std::snprintf(t, sizeof(t), "%d", db);
            c.text({x0 + 4.0f * s, y - 7.0f * s}, dsptb::rgba(160, 164, 174), t);
        }
        static constexpr const char* names[2] = {"L", "R"};
        for (int ch = 0; ch < 2; ++ch) {
            const float x = xs[ch];
            c.fillRect({x, top}, {x + barW, bottom}, dsptb::rgba(30, 32, 38));
            const float rmsY = bottom - (bottom - top) * dbToFrac(r.rmsDb[ch], lo, hi);
            const float peakY = bottom - (bottom - top) * dbToFrac(r.peakDb[ch], lo, hi);
            c.fillRect({x, rmsY}, {x + barW, bottom}, dsptb::rgba(60, 170, 110));
            c.fillRect({x + barW * 0.25f, rmsY}, {x + barW * 0.75f, bottom}, dsptb::rgba(90, 210, 140));
            c.line({x, peakY}, {x + barW, peakY}, dsptb::rgba(250, 250, 250), 2.0f);
            c.rect({x, top}, {x + barW, bottom}, dsptb::rgba(90, 94, 104), 1.0f);
            // clip lamp
            const uint32_t lamp = r.clip[ch] ? dsptb::rgba(235, 50, 50) : dsptb::rgba(70, 40, 40);
            c.fillRect({x, top - 4.0f * s}, {x + barW, top}, lamp);
            c.text({x + barW * 0.5f - 3.0f * s, bottom + 2.0f * s}, dsptb::rgba(200, 204, 214), names[ch]);
        }
    }

    static void fmtLevel(char* out, size_t cap, float v, const char* unit) {
        if (v <= kNoSignal + 1.0f)
            std::snprintf(out, cap, "-inf %s", unit);
        else
            std::snprintf(out, cap, "%.1f %s", static_cast<double>(v), unit);
    }

    void drawReadouts(dsptb::Canvas& c, float s, const Readings& r, bool showTp, float x0, float y0,
                      float w) const {
        const float cell = w / 4.0f;
        struct Item {
            const char* name;
            float value;
            const char* unit;
        } items[4] = {{"MOMENTARY", r.momentary, "LUFS"},
                      {"SHORT-TERM", r.shortTerm, "LUFS"},
                      {"INTEGRATED", r.integrated, "LUFS"},
                      {"RANGE", r.range, "LU"}};
        for (int i = 0; i < 4; ++i) {
            const float x = x0 + cell * static_cast<float>(i);
            c.text({x, y0}, dsptb::rgba(150, 154, 164), items[i].name);
            char t[32];
            if (i == 3)
                std::snprintf(t, sizeof(t), "%.1f %s", static_cast<double>(items[i].value), items[i].unit);
            else
                fmtLevel(t, sizeof(t), items[i].value, items[i].unit);
            c.text({x, y0 + 18.0f * s}, dsptb::rgba(240, 240, 240), t);
        }
        if (showTp) {
            const float tpMax = std::max(r.truePeakDb[0], r.truePeakDb[1]);
            char t[48];
            fmtLevel(t, sizeof(t), tpMax, "dBTP");
            c.text({x0, y0 + 50.0f * s}, dsptb::rgba(150, 154, 164), "TRUE PEAK MAX");
            c.text({x0, y0 + 68.0f * s},
                   tpMax > -1.0f ? dsptb::rgba(240, 90, 80) : dsptb::rgba(240, 240, 240), t);
        }
        const float spMax = std::max(r.samplePeakDb[0], r.samplePeakDb[1]);
        char t[48];
        fmtLevel(t, sizeof(t), spMax, "dBFS");
        c.text({x0 + cell * 2.0f, y0 + 50.0f * s}, dsptb::rgba(150, 154, 164), "SAMPLE PEAK MAX");
        c.text({x0 + cell * 2.0f, y0 + 68.0f * s}, dsptb::rgba(240, 240, 240), t);
    }

    void drawHistory(dsptb::Canvas& c, float s, float target, float x0, float y0, float w, float h) const {
        const float lo = -60.0f, hi = 0.0f;
        c.fillRect({x0, y0}, {x0 + w, y0 + h}, dsptb::rgba(18, 19, 24));
        for (int db = -60; db <= 0; db += 10) {
            const float y = y0 + h * (1.0f - dbToFrac(static_cast<float>(db), lo, hi));
            c.line({x0, y}, {x0 + w, y}, dsptb::rgba(255, 255, 255, 30), 1.0f);
            char t[8];
            std::snprintf(t, sizeof(t), "%d", db);
            c.text({x0 + 3.0f * s, y - 14.0f * s}, dsptb::rgba(130, 134, 144), t);
        }
        const float ty = y0 + h * (1.0f - dbToFrac(target, lo, hi));
        c.line({x0, ty}, {x0 + w, ty}, dsptb::rgba(235, 190, 70), 1.5f);
        char t[32];
        std::snprintf(t, sizeof(t), "target %.0f LUFS", static_cast<double>(target));
        c.text({x0 + w - 120.0f * s, ty - 15.0f * s}, dsptb::rgba(235, 190, 70), t);

        const float histS = host().valid() ? host().getParam(kHistory) : 60.0f;
        const size_t maxPoints = static_cast<size_t>(std::max(10.0f, histS) / kPublishPeriodS);
        auto clip = c.clip({x0, y0}, {x0 + w, y0 + h});
        pts_.clear();
        const size_t n = shortTermUi_.size();
        for (size_t i = 0; i < n; ++i) {
            if (shortTermUi_[i] <= kNoSignal + 1.0f)
                continue;
            // newest point at the right edge
            const float x = x0 + w - w * static_cast<float>(n - 1 - i) / static_cast<float>(maxPoints);
            pts_.push_back({x, y0 + h * (1.0f - dbToFrac(shortTermUi_[i], lo, hi))});
        }
        if (pts_.size() >= 2)
            c.polyline(pts_.data(), static_cast<uint32_t>(pts_.size()), dsptb::rgba(110, 180, 250), 1.5f);
        c.rect({x0, y0}, {x0 + w, y0 + h}, dsptb::rgba(90, 94, 104), 1.0f);
        c.text({x0 + 6.0f * s, y0 + 3.0f * s}, dsptb::rgba(150, 154, 164), "SHORT-TERM LOUDNESS (LUFS)");
    }

    // analysis side
    double sampleRate_ = 48000.0;
    std::unique_ptr<dc::LoudnessMeter> loudness_;
    std::unique_ptr<dc::LevelMeter> level_;
    double appliedRms_ = 300.0;
    double sinceLastPublish_ = 0.0;
    uint32_t resetDone_ = 0, resetCountPub_ = 0;
    bool resetApplied_ = false;
    Readings last_;
    std::vector<float> dumpHistory_;
    std::vector<float> clean_[2];

    // shared
    std::atomic<uint32_t> resetRequests_{0};
    dsptb::rt::SpscRing<float> history_;
    dsptb::rt::TripleBuffer<Readings> readings_;

    // UI side
    std::vector<float> shortTermUi_;
    uint32_t seenReset_ = 0;
    mutable std::vector<dsptb_vec2> pts_;
};

DSPTB_REGISTER_MODULES(LevelMeterModule);
