#pragma once

// Onset (org.dsptb.onset): marks where new sounds start, and shows the novelty curve behind each decision.

#include "common.hpp"
#include "tempo.hpp"

#include <dsptb/dsp_common/onset.hpp>

#include <atomic>
#include <memory>

namespace dsptb::analysis {

class OnsetModule final : public Analyzer<OnsetModule> {
    enum Param : uint32_t { kSensitivity, kMinInterval, kChannel, kHistory, kFreeze };

    struct NoveltyRecord {
        double t = 0.0;
        float novelty = 0.0f;
    };
    struct OnsetRecord {
        double t = 0.0;
        float strength = 0.0f;
    };

public:
    static constexpr ModuleInfo info{.id = "org.dsptb.onset",
                                     .name = "Onset",
                                     .version = "0.1.0",
                                     .vendor = "DSP Toolbox",
                                     .description = "Marks where new sounds start, such as drum hits.",
                                     .caps = 0,
                                     .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::params(
        Float("sensitivity", "Sensitivity", "", 0.f, 1.f, 0.5f),
        Float("min_interval_ms", "Minimum gap", "ms", 10.f, 500.f, 50.f),
        Enum("channel", "Channel", kChannelLabels, 0),
        Float("history_s", "History", "s", 2.f, 60.f, 10.f),
        Bool("freeze", "Freeze", false));

    OnsetModule() : novelty_(8192), onsets_(1024) {}

    bool prepare(const PrepareInfo& p) override {
        sampleRate_ = p.sampleRate;
        offline_ = p.offline;
        detector_.reset();
        dumpNovelty_.clear();
        dumpOnsets_.clear();
        return true;
    }

    void analyze(AnalyzeBlock& b) override {
        const double interval = b.param(kMinInterval);
        if (!detector_ || interval != interval_)
            rebuild(interval);
        if (!detector_)
            return;
        // Sensitivity 0.5 is the library's default threshold; higher finds quieter onsets.
        const float sensitivity = std::clamp(b.param(kSensitivity), 0.0f, 1.0f);
        detector_->setSensitivity(1.0f + (1.0f - sensitivity), 0.04f * (1.0f - sensitivity));
        if (b.droppedFrames() > 0) {
            detector_->reset();
            frameBase_ = b.firstFrameClock();
        }
        const uint32_t n = b.frames();
        mixClean(b.param(kChannel), b.channel(0), b.channel(1), n, mono_);
        const bool frozen = b.param(kFreeze) >= 0.5f;
        const double base = static_cast<double>(frameBase_) / sampleRate_;
        detector_->push(
            mono_.data(), n,
            [&](float value, uint64_t frame) {
                const NoveltyRecord rec{base + (static_cast<double>(frame * config_.hop) +
                                                static_cast<double>(config_.fftSize) * 0.5) /
                                                   sampleRate_,
                                        value};
                if (offline_)
                    dumpNovelty_.push_back(rec);
                else if (!frozen)
                    novelty_.push(rec);
            },
            [&](const dc::OnsetEvent& e) {
                const OnsetRecord rec{base + e.timeSeconds, e.strength};
                if (offline_)
                    dumpOnsets_.push_back(rec);
                else if (!frozen)
                    onsets_.push(rec);
            });
    }

    std::string dumpFrames() override {
        std::string out = "{\"module\":\"org.dsptb.onset\",\"sampleRate\":" + jsonNumber(sampleRate_, "%.1f") +
                          ",\"hop\":" + std::to_string(config_.hop) +
                          ",\"approxBpm\":" + jsonNumber(estimateTempoBpm(dumpOnsets_), "%.1f") + ",\"onsets\":[";
        char buf[120];
        for (size_t i = 0; i < dumpOnsets_.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s{\"timeS\":%.6f,\"strength\":%.4f}", i ? "," : "", dumpOnsets_[i].t,
                          static_cast<double>(dumpOnsets_[i].strength));
            out += buf;
        }
        out += "],\"frames\":[";
        for (size_t i = 0; i < dumpNovelty_.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s{\"index\":%zu,\"timeS\":%.6f,\"novelty\":%.5f}", i ? "," : "", i,
                          dumpNovelty_[i].t, static_cast<double>(dumpNovelty_[i].novelty));
            out += buf;
        }
        return out + "]}";
    }

    void draw(Canvas& c) override {
        const double history = host().valid() ? host().getParam(kHistory) : 10.0;
        novelty_.update(history);
        const size_t before = onsets_.items().size();
        onsets_.update(1e9); // the count and tempo use every onset since the last Reset
        total_ += onsets_.items().size() - std::min(before, onsets_.items().size());

        if (c.button("Reset")) {
            onsets_.clear();
            total_ = 0;
            bpmAtTotal_ = 0;
            bpm_ = 0.0;
        }
        c.sameLine();
        if (c.button("Export CSV"))
            exportCsv();
        c.sameLine();
        char summary[96];
        if (bpmAtTotal_ != total_) { // only a new onset can change it
            bpm_ = estimateTempoBpm(onsets_.items());
            bpmAtTotal_ = total_;
        }
        if (bpm_ > 0.0)
            std::snprintf(summary, sizeof(summary), "%zu onsets   approx. %.0f BPM", total_, bpm_);
        else
            std::snprintf(summary, sizeof(summary), "%zu onsets", total_);
        c.label(summary);
        if (!exportText_.empty())
            c.label(exportText_.c_str());

        const float s = c.uiScale();
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 260.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 160.0f * s, 900.0f * s);
        auto region = c.region("onset_view", w, h);
        if (!region)
            return;
        const Plot p{10.0f * s, 4.0f * s, std::max(40.0f, w - 20.0f * s), std::max(40.0f, h - 28.0f * s)};
        drawPlotFrame(c, p);
        const auto& items = novelty_.items();
        {
            auto clip = c.clip({p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h});
            if (!items.empty()) {
                const double newest = items.back().t;
                float top = 1e-3f;
                for (const auto& r : items)
                    top = std::max(top, r.novelty);
                pts_.clear();
                const size_t step = std::max<size_t>(1, items.size() / static_cast<size_t>(p.w * 2.0f));
                for (size_t i = 0; i < items.size(); i += step)
                    pts_.push_back({p.xOf(items[i].t, newest, history), p.y0 + p.h * (1.0f - items[i].novelty / top)});
                c.polyline(pts_.data(), static_cast<uint32_t>(pts_.size()), rgba(120, 200, 255), 1.0f);
                for (const auto& o : onsets_.items()) {
                    const float x = p.xOf(o.t, newest, history);
                    if (x >= p.x0)
                        c.line({x, p.y0}, {x, p.y0 + p.h}, rgba(255, 180, 70, 220), 1.5f);
                }
            }
        }
        drawHover(c, p, s, history);
        drawPlotBorder(c, p);
        drawTimeAxis(c, p, s, history);
    }

private:
    // An onset within a few pixels of the mouse if there is one, otherwise the novelty curve under it.
    void drawHover(Canvas& c, const Plot& p, float s, double history) const {
        const dsptb_mouse m = c.mouse();
        const auto& items = novelty_.items();
        if (!m.hovered || !p.contains(m.pos) || items.empty())
            return;
        const double newest = items.back().t;
        const double t = p.timeAt(m.pos.x, newest, history);
        drawHoverLine(c, p, m.pos.x);
        char text[96];
        if (const OnsetRecord* onset = nearestInTime(onsets_.items(), t, history * static_cast<double>(4.0f * s / p.w))) {
            std::snprintf(text, sizeof(text), "Onset   strength %.2f   -%.2f s", static_cast<double>(onset->strength),
                          newest - onset->t);
        } else if (const NoveltyRecord* value = nearestInTime(items, t)) {
            std::snprintf(text, sizeof(text), "Novelty %.3f   -%.2f s", static_cast<double>(value->novelty), newest - value->t);
        } else {
            std::snprintf(text, sizeof(text), "No data   -%.2f s", newest - t);
        }
        drawHoverTag(c, p, s, m.pos, text);
    }

    void rebuild(double interval) {
        interval_ = interval;
        config_.fftSize = dc::fftSizeForRate(sampleRate_, 1024);
        config_.hop = dc::fftSizeForRate(sampleRate_, 256);
        config_.minIntervalMs = interval;
        try {
            detector_ = std::make_unique<dc::SpectralFluxOnset>(sampleRate_, config_);
        } catch (const std::exception&) {
            detector_.reset();
        }
        frameBase_ = 0;
    }

    void exportCsv() {
        std::string text = "time_s,strength\n";
        char line[64];
        for (const auto& o : onsets_.items()) {
            std::snprintf(line, sizeof(line), "%.6f,%.5f\n", o.t, static_cast<double>(o.strength));
            text += line;
        }
        const std::string path = writeCsv("onset", text);
        exportText_ = path.empty() ? "Couldn't write the file." : "Saved " + path;
    }

    TimeHistory<NoveltyRecord> novelty_;
    TimeHistory<OnsetRecord> onsets_;
    std::unique_ptr<dc::SpectralFluxOnset> detector_;
    dc::OnsetConfig config_;
    std::vector<float> mono_;
    std::vector<NoveltyRecord> dumpNovelty_;
    std::vector<OnsetRecord> dumpOnsets_;
    std::vector<dsptb_vec2> pts_;
    std::string exportText_;
    double sampleRate_ = 48000.0;
    double interval_ = 0.0;
    uint64_t frameBase_ = 0;
    size_t total_ = 0;
    size_t bpmAtTotal_ = 0;
    double bpm_ = 0.0;
    bool offline_ = false;
};

} // namespace dsptb::analysis
