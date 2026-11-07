#pragma once

// MFCC (org.dsptb.mfcc): the coefficients speech and music tools use to describe timbre, as a heatmap.

#include "common.hpp"

#include <dsptb/dsp_common/colormap.hpp>
#include <dsptb/dsp_common/mel.hpp>

#include <array>
#include <memory>

namespace dsptb::analysis {

class MfccModule final : public Analyzer<MfccModule> {
    enum Param : uint32_t { kNumMfcc, kNumMels, kFmin, kFmax, kChannel, kSkipC0, kHistory, kColormap, kFreeze };
    static constexpr size_t kMaxCoefficients = 40;
    static constexpr uint32_t kTextureWidth = 1024;

    struct Record {
        double t = 0.0;
        uint32_t count = 0;
        std::array<float, kMaxCoefficients> values{};
    };

public:
    static constexpr const char* kMapLabels[] = {"Magma", "Viridis", "Inferno", "Grey"};
    static constexpr ModuleInfo info{.id = "org.dsptb.mfcc",
                                     .name = "MFCC",
                                     .version = "0.1.0",
                                     .vendor = "DSP Toolbox",
                                     .description = "Timbre coefficients over time, as a heatmap.",
                                     .caps = 0,
                                     .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::params(
        Int("n_mfcc", "Coefficients", "", 13, 40, 13),
        Int("n_mels", "Mel bands", "", 20, 128, 40),
        Float("fmin", "Min frequency", "Hz", 0.f, 24000.f, 0.f),
        Float("fmax", "Max frequency", "Hz", 0.f, 24000.f, 24000.f),
        Enum("channel", "Channel", kChannelLabels, 0),
        Bool("skip_c0", "Hide coefficient 0", true),
        Float("history_s", "History", "s", 2.f, 60.f, 10.f),
        Enum("colormap", "Colormap", kMapLabels, 1),
        Bool("freeze", "Freeze", false));

    MfccModule() : history_(4096) {}

    bool prepare(const PrepareInfo& p) override {
        sampleRate_ = p.sampleRate;
        offline_ = p.offline;
        mfcc_.reset();
        dump_.clear();
        return true;
    }

    void analyze(AnalyzeBlock& b) override {
        const int nMfcc = std::clamp(roundInt(b.param(kNumMfcc)), 1, static_cast<int>(kMaxCoefficients));
        const int nMels = std::max(roundInt(b.param(kNumMels)), nMfcc); // the DCT can't give more than it's fed
        const double nyquist = sampleRate_ * 0.5;
        const double fmin = std::min(static_cast<double>(b.param(kFmin)), nyquist - 100.0);
        const double fmax = std::clamp(static_cast<double>(b.param(kFmax)), fmin + 100.0, nyquist);
        if (!mfcc_ || nMfcc != nMfcc_ || nMels != nMels_ || fmin != fmin_ || fmax != fmax_)
            rebuild(nMfcc, nMels, fmin, fmax);
        if (!mfcc_)
            return;
        if (b.droppedFrames() > 0) {
            stft_->reset();
            frameBase_ = b.firstFrameClock();
        }
        const uint32_t n = b.frames();
        mixClean(b.param(kChannel), b.channel(0), b.channel(1), n, mono_);
        const bool frozen = b.param(kFreeze) >= 0.5f;
        stft_->pushPower(mono_.data(), n, [&](const float* power, size_t, uint64_t frame) {
            Record rec;
            rec.t = (static_cast<double>(frameBase_) + static_cast<double>(frame * hop_) +
                     static_cast<double>(fftSize_) * 0.5) /
                    sampleRate_;
            rec.count = static_cast<uint32_t>(nMfcc_);
            mfcc_->compute(power, rec.values.data());
            if (offline_)
                dump_.push_back(rec);
            else if (!frozen)
                history_.push(rec);
        });
    }

    std::string dumpFrames() override {
        std::string out = "{\"module\":\"org.dsptb.mfcc\",\"sampleRate\":" + jsonNumber(sampleRate_, "%.1f") +
                          ",\"hop\":" + std::to_string(hop_) + ",\"nMfcc\":" + std::to_string(nMfcc_) + ",\"frames\":[";
        char buf[64];
        for (size_t i = 0; i < dump_.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s{\"index\":%zu,\"timeS\":%.6f,\"mfcc\":[", i ? "," : "", i, dump_[i].t);
            out += buf;
            for (uint32_t k = 0; k < dump_[i].count; ++k) {
                std::snprintf(buf, sizeof(buf), "%s%.6f", k ? "," : "", static_cast<double>(dump_[i].values[k]));
                out += buf;
            }
            out += "]}";
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

        const float s = c.uiScale();
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 260.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 180.0f * s, 900.0f * s);
        auto region = c.region("mfcc_view", w, h);
        if (!region)
            return;
        const Plot p{34.0f * s, 4.0f * s, std::max(40.0f, w - 44.0f * s), std::max(40.0f, h - 28.0f * s)};
        drawPlotFrame(c, p);
        const auto& items = history_.items();
        if (!items.empty()) {
            const bool skipC0 = !host().valid() || host().getParam(kSkipC0) >= 0.5f;
            const uint32_t count = items.back().count;
            const uint32_t first = skipC0 && count > 1 ? 1 : 0;
            const uint32_t rows = count - first;
            updateTexture(c, rows, first, history);
            if (tex_)
                c.image(tex_, {p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h});
            for (uint32_t r = 0; r < rows; r += rows > 20 ? 5 : 2) {
                const float y = p.y0 + p.h * (1.0f - (static_cast<float>(r) + 0.5f) / static_cast<float>(rows));
                char label[8];
                std::snprintf(label, sizeof(label), "%u", r + first);
                c.text({4.0f * s, y - 7.0f * s}, rgba(200, 204, 214), label);
            }
            drawHover(c, p, s, rows, first, history);
        }
        drawPlotBorder(c, p);
        drawTimeAxis(c, p, s, history);
    }

private:
    // Each coefficient is scaled by its own largest value in the window, so the picture stays readable when
    // one coefficient is much bigger than the others. The dump and the CSV hold the raw values.
    void updateTexture(Canvas& c, uint32_t rows, uint32_t first, double history) {
        const auto& items = history_.items();
        if (rows != texRows_ || !tex_) {
            if (tex_)
                c.textureDestroy(tex_);
            tex_ = c.textureCreate(kTextureWidth, rows);
            texRows_ = rows;
            pixels_.resize(static_cast<size_t>(kTextureWidth) * rows);
        }
        if (!tex_)
            return;
        std::array<float, kMaxCoefficients> top{};
        for (const Record& r : items)
            for (uint32_t k = first; k < r.count; ++k)
                top[k] = std::max(top[k], std::abs(r.values[k]));

        const auto map = static_cast<dc::Colormap>(
            std::clamp(host().valid() ? roundInt(host().getParam(kColormap)) : 1, 0, 3));
        const double newest = items.back().t;
        scaled_.resize(kTextureWidth);
        for (uint32_t row = 0; row < rows; ++row) {
            const uint32_t k = first + row;
            size_t cursor = 0;
            for (uint32_t x = 0; x < kTextureWidth; ++x) {
                const double t = newest - history * (1.0 - (static_cast<double>(x) + 0.5) / kTextureWidth);
                while (cursor + 1 < items.size() && items[cursor + 1].t <= t)
                    ++cursor;
                const bool empty = items[cursor].t > t + 0.5 || k >= items[cursor].count;
                scaled_[x] = empty || top[k] <= 0.0f ? -1.0f : items[cursor].values[k] / top[k];
            }
            // Row 0 of the texture is the top of the picture, where the highest coefficient goes.
            dc::dbToRgba(scaled_.data(), kTextureWidth, -1.0f, 1.0f, map,
                         pixels_.data() + static_cast<size_t>(rows - 1 - row) * kTextureWidth);
        }
        c.textureUpdate(tex_, 0, 0, kTextureWidth, rows, pixels_.data(), kTextureWidth * 4);
    }

    void drawHover(Canvas& c, const Plot& p, float s, uint32_t rows, uint32_t first, double history) const {
        const dsptb_mouse m = c.mouse();
        if (!m.hovered || m.pos.x < p.x0 || m.pos.x > p.x0 + p.w || m.pos.y < p.y0 || m.pos.y > p.y0 + p.h)
            return;
        const auto& items = history_.items();
        const double age = history * (1.0 - static_cast<double>((m.pos.x - p.x0) / p.w));
        const double t = items.back().t - age;
        const auto it = std::lower_bound(items.begin(), items.end(), t,
                                         [](const Record& r, double v) { return r.t < v; });
        if (it == items.end())
            return;
        const auto row = static_cast<uint32_t>(std::clamp((1.0f - (m.pos.y - p.y0) / p.h) * static_cast<float>(rows), 0.0f,
                                                          static_cast<float>(rows - 1)));
        char text[64];
        std::snprintf(text, sizeof(text), "c%u   -%.2f s   %.2f", row + first, age,
                      static_cast<double>(it->values[row + first]));
        const float tx = std::min(m.pos.x + 10.0f * s, p.x0 + p.w - 150.0f * s);
        c.fillRect({tx - 3.0f * s, m.pos.y + 10.0f * s}, {tx + 150.0f * s, m.pos.y + 28.0f * s}, rgba(0, 0, 0, 190));
        c.text({tx, m.pos.y + 12.0f * s}, rgba(255, 255, 255), text);
    }

    void rebuild(int nMfcc, int nMels, double fmin, double fmax) {
        nMfcc_ = nMfcc;
        nMels_ = nMels;
        fmin_ = fmin;
        fmax_ = fmax;
        fftSize_ = dc::fftSizeForRate(sampleRate_, 2048);
        hop_ = dc::fftSizeForRate(sampleRate_, 512);
        dc::MelConfig config;
        config.fftSize = fftSize_;
        config.sampleRate = sampleRate_;
        config.nMels = static_cast<size_t>(nMels);
        config.fmin = fmin;
        config.fmax = fmax;
        try {
            stft_ = std::make_unique<dc::StftAnalyzer>(dc::StftConfig{fftSize_, hop_, dc::Window::Hann});
            mfcc_ = std::make_unique<dc::Mfcc>(config, static_cast<size_t>(nMfcc));
        } catch (const std::exception&) {
            mfcc_.reset();
        }
        frameBase_ = 0;
    }

    void exportCsv() {
        std::string text = "time_s";
        const auto& items = history_.items();
        const uint32_t count = items.empty() ? 0 : items.back().count;
        for (uint32_t k = 0; k < count; ++k)
            text += ",c" + std::to_string(k);
        text += "\n";
        char cell[32];
        for (const Record& r : items) {
            std::snprintf(cell, sizeof(cell), "%.6f", r.t);
            text += cell;
            for (uint32_t k = 0; k < count; ++k) {
                std::snprintf(cell, sizeof(cell), ",%.6f", static_cast<double>(r.values[k]));
                text += cell;
            }
            text += "\n";
        }
        const std::string path = writeCsv("mfcc", text);
        exportText_ = path.empty() ? "Couldn't write the file." : "Saved " + path;
    }

    TimeHistory<Record> history_;
    std::unique_ptr<dc::StftAnalyzer> stft_;
    std::unique_ptr<dc::Mfcc> mfcc_;
    std::vector<float> mono_;
    std::vector<Record> dump_;
    std::vector<uint32_t> pixels_;
    std::vector<float> scaled_;
    std::string exportText_ = "the last history window";
    dsptb_texture tex_ = 0;
    uint32_t texRows_ = 0;
    double sampleRate_ = 48000.0;
    double fmin_ = 0.0, fmax_ = 0.0;
    size_t fftSize_ = 2048, hop_ = 512;
    int nMfcc_ = 0, nMels_ = 0;
    uint64_t frameBase_ = 0;
    bool offline_ = false;
};

} // namespace dsptb::analysis
