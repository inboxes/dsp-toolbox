#pragma once

// Spectral Features (org.dsptb.spectralfeatures): brightness, noisiness and level over time.

#include "common.hpp"

#include <dsptb/dsp_common/spectral_features.hpp>

#include <memory>

namespace dsptb::analysis {

class SpectralModule final : public Analyzer<SpectralModule> {
    enum Param : uint32_t {
        kShowCentroid,
        kShowSpread,
        kShowRolloff,
        kShowFlatness,
        kShowFlux,
        kShowZcr,
        kShowRms,
        kChannel,
        kHistory,
        kFreeze,
    };

    struct Record {
        double t = 0.0;
        dc::SpectralFeatures f{};
    };

    enum class Axis { LogHz, Unit, Db, Auto };
    struct Strip {
        Param show;
        const char* name;
        Axis axis;
        float (*value)(const dc::SpectralFeatures&);
    };

    static float rmsDb(const dc::SpectralFeatures& f) { return 20.0f * std::log10(std::max(f.rms, 1e-6f)); }
    // Music's flatness is around 0.0001, which is a dead line on a 0 to 1 scale. It's a power ratio, hence 10 log.
    static float flatnessDb(const dc::SpectralFeatures& f) { return 10.0f * std::log10(std::max(f.flatness, 1e-8f)); }

public:
    static constexpr ModuleInfo info{.id = "org.dsptb.spectralfeatures",
                                     .name = "Spectral Features",
                                     .version = "0.1.0",
                                     .vendor = "DSP Toolbox",
                                     .description = "Brightness, noisiness and level over time.",
                                     .caps = 0,
                                     .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::params(
        Bool("show_centroid", "Centroid", true),
        Bool("show_spread", "Spread", false),
        Bool("show_rolloff", "Rolloff (85 %)", true),
        Bool("show_flatness", "Flatness", true),
        Bool("show_flux", "Flux", false),
        Bool("show_zcr", "Zero-crossing rate", false),
        Bool("show_rms", "RMS", true),
        Enum("channel", "Channel", kChannelLabels, 0),
        Float("history_s", "History", "s", 2.f, 60.f, 10.f),
        Bool("freeze", "Freeze", false));

    SpectralModule() : history_(4096) {}

    bool prepare(const PrepareInfo& p) override {
        sampleRate_ = p.sampleRate;
        offline_ = p.offline;
        fftSize_ = dc::fftSizeForRate(p.sampleRate, 2048);
        hop_ = dc::fftSizeForRate(p.sampleRate, 512);
        extractor_ = std::make_unique<dc::SpectralFeatureExtractor>(p.sampleRate, fftSize_, hop_);
        dump_.clear();
        frameBase_ = 0;
        return true;
    }

    // All seven are always computed and exported: the checkboxes only choose what's plotted.
    void analyze(AnalyzeBlock& b) override {
        if (!extractor_)
            return;
        if (b.droppedFrames() > 0) {
            extractor_->reset();
            frameBase_ = b.firstFrameClock();
        }
        const uint32_t n = b.frames();
        mixClean(b.param(kChannel), b.channel(0), b.channel(1), n, mono_);
        const bool frozen = b.param(kFreeze) >= 0.5f;
        extractor_->push(mono_.data(), n, [&](const dc::SpectralFeatures& f, uint64_t frame) {
            Record rec;
            rec.t = (static_cast<double>(frameBase_) + static_cast<double>(frame * hop_) +
                     static_cast<double>(fftSize_) * 0.5) /
                    sampleRate_;
            rec.f = f;
            if (offline_)
                dump_.push_back(rec);
            else if (!frozen)
                history_.push(rec);
        });
    }

    std::string dumpFrames() override {
        std::string out = "{\"module\":\"org.dsptb.spectralfeatures\",\"sampleRate\":" +
                          jsonNumber(sampleRate_, "%.1f") + ",\"hop\":" + std::to_string(hop_) + ",\"frames\":[";
        char buf[256];
        for (size_t i = 0; i < dump_.size(); ++i) {
            const dc::SpectralFeatures& f = dump_[i].f;
            std::snprintf(buf, sizeof(buf),
                          "%s{\"index\":%zu,\"timeS\":%.6f,\"centroid\":%.5f,\"spread\":%.5f,\"rolloff\":%.5f,"
                          "\"flatness\":%.6f,\"flux\":%.6f,\"zcr\":%.6f,\"rmsDb\":%.5f}",
                          i ? "," : "", i, dump_[i].t, static_cast<double>(f.centroidHz),
                          static_cast<double>(f.spreadHz), static_cast<double>(f.rolloffHz),
                          static_cast<double>(f.flatness), static_cast<double>(f.flux), static_cast<double>(f.zcr),
                          static_cast<double>(rmsDb(f)));
            out += buf;
        }
        return out + "]}";
    }

    void draw(Canvas& c) override {
        const double history = host().valid() ? host().getParam(kHistory) : 10.0;
        history_.update(history);
        if (c.button("Export CSV"))
            exportCsv();
        c.sameLine();
        c.label(exportText_.c_str());

        static constexpr Strip kStrips[] = {
            {kShowCentroid, "Centroid", Axis::LogHz, [](const dc::SpectralFeatures& f) { return f.centroidHz; }},
            {kShowSpread, "Spread", Axis::LogHz, [](const dc::SpectralFeatures& f) { return f.spreadHz; }},
            {kShowRolloff, "Rolloff", Axis::LogHz, [](const dc::SpectralFeatures& f) { return f.rolloffHz; }},
            {kShowFlatness, "Flatness", Axis::Db, [](const dc::SpectralFeatures& f) { return flatnessDb(f); }},
            {kShowFlux, "Flux", Axis::Auto, [](const dc::SpectralFeatures& f) { return f.flux; }},
            {kShowZcr, "Zero crossings", Axis::Unit, [](const dc::SpectralFeatures& f) { return f.zcr; }},
            {kShowRms, "RMS", Axis::Db, [](const dc::SpectralFeatures& f) { return rmsDb(f); }},
        };
        int shown = 0;
        for (const Strip& strip : kStrips)
            shown += isShown(strip) ? 1 : 0;

        const float s = c.uiScale();
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 260.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 160.0f * s, 900.0f * s);
        auto region = c.region("spectral_view", w, h);
        if (!region || shown == 0)
            return;
        const float axisH = 24.0f * s;
        const float gap = 4.0f * s;
        const float stripH = std::max(24.0f * s, (h - axisH - gap * static_cast<float>(shown)) / static_cast<float>(shown));
        float y = 2.0f * s;
        Plot last{};
        for (const Strip& strip : kStrips) {
            if (!isShown(strip))
                continue;
            last = Plot{10.0f * s, y, std::max(40.0f, w - 20.0f * s), stripH};
            drawStrip(c, last, s, strip, history);
            y += stripH + gap;
        }
        drawTimeAxis(c, last, s, history);
    }

private:
    bool isShown(const Strip& strip) const { return !host().valid() || host().getParam(strip.show) >= 0.5f; }

    void drawStrip(Canvas& c, const Plot& p, float s, const Strip& strip, double history) {
        drawPlotFrame(c, p);
        const auto& items = history_.items();
        const double nyquist = sampleRate_ * 0.5;
        float top = 1e-6f;
        if (strip.axis == Axis::Auto)
            for (const Record& r : items)
                top = std::max(top, strip.value(r.f));
        const auto fraction = [&](float v) -> float {
            switch (strip.axis) {
            case Axis::LogHz:
                return static_cast<float>(std::log(std::max(static_cast<double>(v), 20.0) / 20.0) / std::log(nyquist / 20.0));
            case Axis::Unit:
                return v;
            case Axis::Db:
                return (v + 80.0f) / 80.0f;
            case Axis::Auto:
                break;
            }
            return v / top;
        };
        char text[64];
        std::snprintf(text, sizeof(text), "%s", strip.name);
        {
            auto clip = c.clip({p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h});
            if (!items.empty()) {
                const double newest = items.back().t;
                pts_.clear();
                const size_t step = std::max<size_t>(1, items.size() / static_cast<size_t>(p.w * 2.0f));
                for (size_t i = 0; i < items.size(); i += step) {
                    const float f = std::clamp(fraction(strip.value(items[i].f)), 0.0f, 1.0f);
                    pts_.push_back({p.xOf(items[i].t, newest, history), p.y0 + p.h * (1.0f - f)});
                }
                c.polyline(pts_.data(), static_cast<uint32_t>(pts_.size()), rgba(120, 200, 255), 1.5f);
                const float latest = strip.value(items.back().f);
                const char* unit = strip.axis == Axis::LogHz ? " Hz" : strip.axis == Axis::Db ? " dB" : "";
                const char* format = strip.axis == Axis::LogHz ? "%s   %.0f%s" : strip.axis == Axis::Db ? "%s   %.1f%s" : "%s   %.3f%s";
                std::snprintf(text, sizeof(text), format, strip.name, static_cast<double>(latest), unit);
            }
            c.text({p.x0 + 6.0f * s, p.y0 + 2.0f * s}, rgba(230, 232, 238), text);
        }
        drawHover(c, p, s, strip, history);
        drawPlotBorder(c, p);
    }

    // Every strip shows its value at the mouse's time, so the features can be read together.
    void drawHover(Canvas& c, const Plot& p, float s, const Strip& strip, double history) const {
        const dsptb_mouse m = c.mouse();
        const auto& items = history_.items();
        if (!m.hovered || m.pos.x < p.x0 || m.pos.x > p.x0 + p.w || items.empty())
            return;
        const double newest = items.back().t;
        const double t = p.timeAt(m.pos.x, newest, history);
        drawHoverLine(c, p, m.pos.x);
        char text[64];
        if (const Record* r = nearestInTime(items, t)) {
            const double value = static_cast<double>(strip.value(r->f));
            if (strip.axis == Axis::LogHz)
                std::snprintf(text, sizeof(text), "%.0f Hz   -%.2f s", value, newest - r->t);
            else if (strip.axis == Axis::Db)
                std::snprintf(text, sizeof(text), "%.1f dB   -%.2f s", value, newest - r->t);
            else
                std::snprintf(text, sizeof(text), "%.3f   -%.2f s", value, newest - r->t);
        } else {
            std::snprintf(text, sizeof(text), "No data   -%.2f s", newest - t);
        }
        drawHoverTag(c, p, s, {m.pos.x, p.y0}, text);
    }

    void exportCsv() {
        std::string text = "time_s,centroid_hz,spread_hz,rolloff_hz,flatness,flux,zcr,rms_db\n";
        char line[160];
        for (const Record& r : history_.items()) {
            const dc::SpectralFeatures& f = r.f;
            std::snprintf(line, sizeof(line), "%.6f,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f,%.4f\n", r.t,
                          static_cast<double>(f.centroidHz), static_cast<double>(f.spreadHz),
                          static_cast<double>(f.rolloffHz), static_cast<double>(f.flatness),
                          static_cast<double>(f.flux), static_cast<double>(f.zcr), static_cast<double>(rmsDb(f)));
            text += line;
        }
        const std::string path = writeCsv("spectral-features", text);
        exportText_ = path.empty() ? "Couldn't write the file." : "Saved " + path;
    }

    TimeHistory<Record> history_;
    std::unique_ptr<dc::SpectralFeatureExtractor> extractor_;
    std::vector<float> mono_;
    std::vector<Record> dump_;
    std::vector<dsptb_vec2> pts_;
    std::string exportText_ = "the last history window";
    double sampleRate_ = 48000.0;
    size_t fftSize_ = 2048, hop_ = 512;
    uint64_t frameBase_ = 0;
    bool offline_ = false;
};

} // namespace dsptb::analysis
