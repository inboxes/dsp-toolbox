#pragma once

// Pitch (org.dsptb.pitch): the pitch of a single voice or instrument over time, as frequency, note and cents.
// Autotune's tests measure their output with its frame dump, so that format is a contract.

#include "common.hpp"

#include <dsptb/dsp_common/pitch.hpp>
#include <dsptb/dsp_common/tuning.hpp>

#include <memory>

namespace dsptb::analysis {

class PitchModule final : public Analyzer<PitchModule> {
    enum Param : uint32_t { kMinHz, kMaxHz, kThreshold, kChannel, kA4, kHistory, kFreeze };

    struct Record {
        double t = 0.0; // centre of the frame, seconds
        float hz = 0.0f;
        float confidence = 0.0f;
        bool voiced = false;
    };

public:
    static constexpr ModuleInfo info{.id = "org.dsptb.pitch",
                                     .name = "Pitch",
                                     .version = "0.1.0",
                                     .vendor = "DSP Toolbox",
                                     .description = "The pitch of one voice or instrument, as note and cents.",
                                     .caps = 0,
                                     .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::params(
        Float("min_hz", "Min frequency", "Hz", 50.f, 1000.f, 50.f).log(),
        Float("max_hz", "Max frequency", "Hz", 50.f, 1000.f, 1000.f).log(),
        Float("threshold", "YIN threshold", "", 0.10f, 0.30f, 0.15f),
        Enum("channel", "Channel", kChannelLabels, 0),
        Float("a4_hz", "Reference A4", "Hz", 415.f, 466.f, 440.f),
        Float("history_s", "History", "s", 2.f, 60.f, 10.f),
        Bool("freeze", "Freeze", false));

    PitchModule() : history_(8192) {}

    bool prepare(const PrepareInfo& p) override {
        sampleRate_ = p.sampleRate;
        offline_ = p.offline;
        yin_.reset();
        dump_.clear();
        dropped_ = 0;
        return true;
    }

    void analyze(AnalyzeBlock& b) override {
        const double lo = b.param(kMinHz);
        const double hi = std::max(static_cast<double>(b.param(kMaxHz)), lo + 1.0);
        const double threshold = b.param(kThreshold);
        if (!yin_ || lo != lo_ || hi != hi_ || threshold != threshold_)
            rebuild(lo, hi, threshold);
        if (!yin_)
            return;
        if (b.droppedFrames() > 0) {
            dropped_ += b.droppedFrames();
            yin_->reset();
            frameBase_ = b.firstFrameClock();
        }
        const uint32_t n = b.frames();
        mixClean(b.param(kChannel), b.channel(0), b.channel(1), n, mono_);
        const bool frozen = b.param(kFreeze) >= 0.5f;
        yin_->push(mono_.data(), n, [&](const dc::PitchResult& r) {
            Record rec;
            rec.t = (static_cast<double>(frameBase_) + static_cast<double>(r.frameIndex * config_.hop) +
                     static_cast<double>(config_.frameSize) * 0.5) /
                    sampleRate_;
            rec.hz = r.hz;
            rec.confidence = r.confidence;
            rec.voiced = r.voiced;
            if (offline_)
                dump_.push_back(rec);
            else if (!frozen)
                history_.push(rec);
        });
    }

    std::string dumpFrames() override {
        std::string out = "{\"module\":\"org.dsptb.pitch\",\"sampleRate\":" + jsonNumber(sampleRate_, "%.1f") +
                          ",\"hop\":" + std::to_string(config_.hop) + ",\"frames\":[";
        char buf[160];
        for (size_t i = 0; i < dump_.size(); ++i) {
            const Record& r = dump_[i];
            std::snprintf(buf, sizeof(buf), "%s{\"index\":%zu,\"timeS\":%.6f,\"hz\":%.4f,\"confidence\":%.4f,\"voiced\":%s}",
                          i ? "," : "", i, r.t, static_cast<double>(r.hz), static_cast<double>(r.confidence),
                          r.voiced ? "true" : "false");
            out += buf;
        }
        return out + "]}";
    }

    void draw(Canvas& c) override {
        const double history = host().valid() ? host().getParam(kHistory) : 10.0;
        history_.update(history);
        const auto& items = history_.items();
        const dc::Tuning tuning = readTuning();

        if (c.button("Export CSV"))
            exportCsv();
        c.sameLine();
        c.label(exportText_.c_str());

        const float s = c.uiScale();
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 260.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 200.0f * s, 900.0f * s);
        auto region = c.region("pitch_view", w, h);
        if (!region)
            return;

        // The latest voiced frame, large enough to read while singing.
        char readout[64] = "--";
        if (!items.empty() && items.back().voiced) {
            char note[24];
            dc::noteName(items.back().hz, tuning, note, sizeof(note));
            std::snprintf(readout, sizeof(readout), "%.1f Hz   %s   %.0f %%", static_cast<double>(items.back().hz), note,
                          static_cast<double>(items.back().confidence) * 100.0);
        }
        c.text({0.0f, 0.0f}, rgba(255, 255, 255), readout);

        const Plot p{48.0f * s, 22.0f * s, std::max(40.0f, w - 56.0f * s), std::max(40.0f, h - 46.0f * s)};
        drawPlotFrame(c, p);
        const double loHz = host().valid() ? host().getParam(kMinHz) : 50.0;
        const double hiHz = std::max(host().valid() ? static_cast<double>(host().getParam(kMaxHz)) : 1000.0, loHz + 1.0);
        const auto yOf = [&](double hz) {
            const double f = std::log(hz / loHz) / std::log(hiHz / loHz);
            return p.y0 + p.h * static_cast<float>(1.0 - f);
        };
        drawNoteLines(c, p, s, loHz, hiHz, tuning, yOf);
        {
            auto clip = c.clip({p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h});
            if (!items.empty()) {
                const double newest = items.back().t;
                const size_t step = std::max<size_t>(1, items.size() / static_cast<size_t>(p.w * 2.0f));
                bool hasPrev = false;
                dsptb_vec2 prev{};
                for (size_t i = 0; i < items.size(); i += step) {
                    const Record& r = items[i];
                    if (!r.voiced || r.hz <= 0.0f) {
                        hasPrev = false; // an unvoiced frame leaves a gap
                        continue;
                    }
                    const dsptb_vec2 cur{p.xOf(r.t, newest, history), yOf(r.hz)};
                    if (hasPrev) {
                        const auto alpha = static_cast<uint8_t>(90.0f + 165.0f * std::clamp(r.confidence, 0.0f, 1.0f));
                        c.line(prev, cur, rgba(120, 200, 255, alpha), 2.0f);
                    }
                    prev = cur;
                    hasPrev = true;
                }
            }
        }
        drawPlotBorder(c, p);
        drawTimeAxis(c, p, s, history);
    }

private:
    template <class YOf>
    void drawNoteLines(Canvas& c, const Plot& p, float s, double loHz, double hiHz, const dc::Tuning& tuning,
                       const YOf& yOf) const {
        const int first = static_cast<int>(std::ceil(dc::hzToMidi(loHz, tuning)));
        const int last = static_cast<int>(std::floor(dc::hzToMidi(hiHz, tuning)));
        for (int midi = first; midi <= last; ++midi) {
            const double hz = dc::midiToHz(static_cast<double>(midi), tuning);
            const float y = yOf(hz);
            const dc::NoteInfo note = dc::nearestNote(hz, tuning);
            const bool labelled = note.pitchClass == 0 || (last - first < 24 && note.pitchClass == 9);
            c.line({p.x0, y}, {p.x0 + p.w, y}, rgba(255, 255, 255, labelled ? 60 : 18), 1.0f);
            if (labelled) {
                char label[12];
                std::snprintf(label, sizeof(label), "%s%d", dc::pitchClassName(note.pitchClass), note.octave);
                c.text({p.x0 - 34.0f * s, y - 7.0f * s}, rgba(200, 204, 214), label);
            }
        }
    }

    dc::Tuning readTuning() const {
        const float a4 = host().valid() ? host().getParam(kA4) : 440.0f;
        return dc::Tuning::fromOffsetCents(1200.0 * std::log2(static_cast<double>(a4) / 440.0));
    }

    // A window that lasts the same time at any rate. The longest period searched must fit half a frame, so
    // a rate too low for that (the constructor throws) simply produces nothing.
    void rebuild(double lo, double hi, double threshold) {
        lo_ = lo;
        hi_ = hi;
        threshold_ = threshold;
        config_.frameSize = dc::fftSizeForRate(sampleRate_, 2048);
        config_.hop = dc::fftSizeForRate(sampleRate_, 256);
        config_.minHz = lo;
        config_.maxHz = std::min(hi, sampleRate_ * 0.5);
        config_.threshold = threshold;
        try {
            yin_ = std::make_unique<dc::YinPitch>(sampleRate_, config_);
        } catch (const std::exception&) {
            yin_.reset();
        }
        frameBase_ = 0;
    }

    void exportCsv() {
        std::string text = "time_s,hz,confidence,voiced\n";
        char line[96];
        for (const Record& r : history_.items()) {
            std::snprintf(line, sizeof(line), "%.6f,%.4f,%.4f,%d\n", r.t, static_cast<double>(r.hz),
                          static_cast<double>(r.confidence), r.voiced ? 1 : 0);
            text += line;
        }
        const std::string path = writeCsv("pitch", text);
        exportText_ = path.empty() ? "Couldn't write the file." : "Saved " + path;
    }

    TimeHistory<Record> history_;
    std::unique_ptr<dc::YinPitch> yin_;
    dc::PitchConfig config_;
    std::vector<float> mono_;
    std::vector<Record> dump_;
    std::string exportText_ = "the last history window";
    double sampleRate_ = 48000.0;
    double lo_ = 0.0, hi_ = 0.0, threshold_ = 0.0;
    uint64_t frameBase_ = 0;
    uint64_t dropped_ = 0;
    bool offline_ = false;
};

} // namespace dsptb::analysis
