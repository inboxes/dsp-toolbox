#pragma once

// File Analysis (org.dsptb.fileanalysis): a report on the loaded file, with a whole-file spectrogram. It runs
// on a worker thread while the UI thread keeps drawing, so a finished report is handed over whole and never
// changes after that.

#include "common.hpp"

#include <dsptb/dsp_common/colormap.hpp>
#include <dsptb/dsp_common/freq_map.hpp>
#include <dsptb/dsp_common/loudness.hpp>
#include <dsptb/dsp_common/tuning.hpp>

#include <atomic>
#include <limits>
#include <memory>

namespace dsptb::analysis {

class FileAnalysisModule final : public OfflineJob<FileAnalysisModule> {
    enum Param : uint32_t { kFftSize, kColumns, kColormap };
    static constexpr uint32_t kRows = 512;
    static constexpr size_t kChunkFrames = 16384; // how often it checks for cancel and reports progress

    struct Report {
        uint64_t frames = 0;
        double sampleRate = 0.0;
        double seconds = 0.0;
        double integratedLufs = 0.0;
        double rangeLu = 0.0;
        double truePeakDbtp = 0.0;
        double samplePeakDbfs = 0.0;
        double dcOffset[2] = {0.0, 0.0};
        uint32_t columns = 0;
        double maxHz = 0.0;
        std::vector<float> db; // columns x kRows, by column
    };

public:
    static constexpr const char* kFftLabels[] = {"1024", "2048", "4096", "8192"};
    static constexpr const char* kColumnLabels[] = {"1024", "2048", "4096"};
    static constexpr const char* kMapLabels[] = {"Magma", "Viridis", "Inferno", "Grey"};
    static constexpr ModuleInfo info{.id = "org.dsptb.fileanalysis",
                                     .name = "File Analysis",
                                     .version = "0.1.0",
                                     .vendor = "DSP Toolbox",
                                     .description = "A report on the loaded file: loudness, peaks and a spectrogram.",
                                     .caps = 0,
                                     .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::params(
        Enum("fft_size", "Spectrogram FFT", kFftLabels, 2),
        Enum("columns", "Spectrogram width", kColumnLabels, 1),
        Enum("colormap", "Colormap", kMapLabels, 0));

    bool prepare(const PrepareInfo&) override { return true; }

    bool runOffline(OfflineBlock& job) override {
        auto report = std::make_shared<Report>();
        const uint64_t frames = job.frames();
        const double sampleRate = job.sampleRate();
        report->frames = frames;
        report->sampleRate = sampleRate;
        report->seconds = static_cast<double>(frames) / sampleRate;

        const uint32_t channels = std::min<uint32_t>(job.numChannels(), 2);
        const float* left = job.channel(0);
        const float* right = channels > 1 ? job.channel(1) : left;
        const float* planar[2] = {left, right};

        constexpr size_t kFftSizes[] = {1024, 2048, 4096, 8192};
        constexpr uint32_t kColumnCounts[] = {1024, 2048, 4096};
        const size_t fft = dc::fftSizeForRate(
            sampleRate, kFftSizes[static_cast<size_t>(std::clamp(roundInt(job.param(kFftSize)), 0, 3))]);
        // Enough frames to fill every column of a short file, without slowing a long one down.
        const size_t hop = static_cast<size_t>(std::clamp<uint64_t>(frames / kColumnCounts[columnIndex(job)], 32, fft / 2));
        report->columns = kColumnCounts[columnIndex(job)];
        report->maxHz = std::min(20000.0, sampleRate * 0.5);
        report->db.assign(static_cast<size_t>(report->columns) * kRows, -200.0f);

        dc::LoudnessMeter loudness(sampleRate, 2);
        dc::StftAnalyzer stft(dc::StftConfig{fft, hop, dc::Window::Hann});
        dc::BinToRowMap rows(fft, sampleRate, kRows, dc::FreqScale::Log, 20.0, report->maxHz);
        std::vector<float> mono(kChunkFrames);
        std::vector<float> rowDb(kRows);

        double peak = 0.0;
        double sum[2] = {0.0, 0.0};
        for (uint64_t pos = 0; pos < frames; pos += kChunkFrames) {
            if (job.cancelled())
                return false;
            const auto n = static_cast<size_t>(std::min<uint64_t>(kChunkFrames, frames - pos));
            const float* chunk[2] = {planar[0] + pos, planar[1] + pos};
            loudness.process(chunk, n);
            for (size_t i = 0; i < n; ++i) {
                const double l = static_cast<double>(chunk[0][i]);
                const double r = static_cast<double>(chunk[1][i]);
                sum[0] += l;
                sum[1] += r;
                peak = std::max({peak, std::abs(l), std::abs(r)});
            }
            dc::mixToMono(chunk[0], chunk[1], n, dc::ChannelMix::Mid, mono.data());
            for (size_t i = 0; i < n; ++i)
                if (!std::isfinite(mono[i]))
                    mono[i] = 0.0f;
            stft.push(mono.data(), n, [&](const float* magDb, size_t, uint64_t frame) {
                rows.apply(magDb, rowDb.data());
                // Each column keeps the loudest frame that lands on it.
                const double position = (static_cast<double>(frame * hop) + static_cast<double>(fft) * 0.5) /
                                        static_cast<double>(frames);
                const auto column = static_cast<size_t>(
                    std::min(position * report->columns, static_cast<double>(report->columns - 1)));
                float* out = report->db.data() + column * kRows;
                for (uint32_t y = 0; y < kRows; ++y)
                    out[y] = std::max(out[y], rowDb[y]);
            });
            job.reportProgress(static_cast<float>(static_cast<double>(pos + n) / static_cast<double>(frames)));
        }
        // A file shorter than one FFT per column still leaves gaps: repeat the column before.
        for (size_t x = 1; x < report->columns; ++x) {
            float* column = report->db.data() + x * kRows;
            if (std::all_of(column, column + kRows, [](float v) { return v <= -200.0f; }))
                std::copy_n(column - kRows, kRows, column);
        }
        report->integratedLufs = loudness.integratedLufs();
        report->rangeLu = loudness.loudnessRangeLu();
        report->truePeakDbtp = std::max(loudness.truePeakDbtp(0), loudness.truePeakDbtp(1));
        report->samplePeakDbfs = peak > 0.0 ? 20.0 * std::log10(peak) : -std::numeric_limits<double>::infinity();
        if (frames > 0)
            for (int ch = 0; ch < 2; ++ch)
                report->dcOffset[ch] = sum[ch] / static_cast<double>(frames);
        job.reportProgress(1.0f);
        report_.store(std::shared_ptr<const Report>(report), std::memory_order_release);
        return true;
    }

    std::string dumpFrames() override {
        const auto r = report_.load(std::memory_order_acquire);
        if (!r)
            return "{\"module\":\"org.dsptb.fileanalysis\"}";
        return "{\"module\":\"org.dsptb.fileanalysis\",\"sampleRate\":" + jsonNumber(r->sampleRate, "%.1f") +
               ",\"frames\":" + std::to_string(r->frames) + ",\"seconds\":" + jsonNumber(r->seconds) +
               ",\"integratedLufs\":" + jsonNumber(r->integratedLufs) + ",\"rangeLu\":" + jsonNumber(r->rangeLu) +
               ",\"truePeakDbtp\":" + jsonNumber(r->truePeakDbtp) + ",\"samplePeakDbfs\":" + jsonNumber(r->samplePeakDbfs) +
               ",\"dcOffset\":[" + jsonNumber(r->dcOffset[0], "%.9f") + "," + jsonNumber(r->dcOffset[1], "%.9f") + "]}";
    }

    void draw(Canvas& c) override {
        const auto report = report_.load(std::memory_order_acquire);
        if (!report) {
            c.label("Open an audio file and press Run.");
            return;
        }
        if (c.button("Export CSV"))
            exportCsv(*report);
        c.sameLine();
        c.label(exportText_.c_str());

        char line[128];
        std::snprintf(line, sizeof(line), "Duration %.2f s   at %.0f Hz", report->seconds, report->sampleRate);
        c.label(line);
        std::snprintf(line, sizeof(line), "Integrated %s LUFS   Range %s LU", number(report->integratedLufs).c_str(),
                      number(report->rangeLu).c_str());
        c.label(line);
        std::snprintf(line, sizeof(line), "True peak %s dBTP   Sample peak %s dBFS", number(report->truePeakDbtp).c_str(),
                      number(report->samplePeakDbfs).c_str());
        c.label(line);
        std::snprintf(line, sizeof(line), "DC offset  L %.6f   R %.6f", report->dcOffset[0], report->dcOffset[1]);
        c.label(line);

        const float s = c.uiScale();
        const dsptb_vec2 avail = c.availableSize();
        const float w = std::max(avail.x, 260.0f * s);
        const float h = std::clamp(avail.y - 6.0f * s, 160.0f * s, 900.0f * s);
        auto region = c.region("file_view", w, h);
        if (!region)
            return;
        const Plot p{48.0f * s, 4.0f * s, std::max(40.0f, w - 58.0f * s), std::max(40.0f, h - 28.0f * s)};
        drawPlotFrame(c, p);
        updateTexture(c, report);
        if (tex_)
            c.image(tex_, {p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h});
        drawHover(c, p, s, *report);
        drawPlotBorder(c, p);
        drawAxes(c, p, s, *report);
    }

private:
    void drawHover(Canvas& c, const Plot& p, float s, const Report& r) const {
        const dsptb_mouse m = c.mouse();
        if (!m.hovered || !p.contains(m.pos) || r.columns == 0)
            return;
        const float fx = (m.pos.x - p.x0) / p.w;
        const float fy = 1.0f - (m.pos.y - p.y0) / p.h;
        const auto column = std::min(r.columns - 1, static_cast<uint32_t>(fx * static_cast<float>(r.columns)));
        const auto row = std::min(kRows - 1, static_cast<uint32_t>(fy * static_cast<float>(kRows)));
        // The same log axis the spectrogram was mapped to, from 20 Hz up.
        const double hz = 20.0 * std::pow(r.maxHz / 20.0, (static_cast<double>(row) + 0.5) / kRows);
        const float db = r.db[static_cast<size_t>(column) * kRows + row];
        c.line({m.pos.x, p.y0}, {m.pos.x, p.y0 + p.h}, rgba(255, 255, 255, 110), 1.0f);
        c.line({p.x0, m.pos.y}, {p.x0 + p.w, m.pos.y}, rgba(255, 255, 255, 110), 1.0f);
        char note[24];
        dc::noteName(hz, dc::Tuning{}, note, sizeof(note));
        char level[24] = "no signal";
        if (db > -200.0f)
            std::snprintf(level, sizeof(level), "%.0f dB", static_cast<double>(db));
        char text[96];
        std::snprintf(text, sizeof(text), "%.1f Hz  %s   %.2f s   %s", hz, note, static_cast<double>(fx) * r.seconds, level);
        drawHoverTag(c, p, s, m.pos, text);
    }

    static size_t columnIndex(const OfflineBlock& job) {
        return static_cast<size_t>(std::clamp(roundInt(job.param(kColumns)), 0, 2));
    }

    static std::string number(double v) {
        if (!std::isfinite(v))
            return "--";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return buf;
    }

    void updateTexture(Canvas& c, const std::shared_ptr<const Report>& report) {
        const int map = host().valid() ? std::clamp(roundInt(host().getParam(kColormap)), 0, 3) : 0;
        if (shown_ == report && map == shownMap_ && tex_)
            return;
        if (tex_)
            c.textureDestroy(tex_);
        tex_ = c.textureCreate(report->columns, kRows);
        shown_ = report;
        shownMap_ = map;
        if (!tex_)
            return;
        pixels_.resize(static_cast<size_t>(report->columns) * kRows);
        row_.resize(report->columns);
        for (uint32_t y = 0; y < kRows; ++y) {
            for (uint32_t x = 0; x < report->columns; ++x)
                row_[x] = report->db[static_cast<size_t>(x) * kRows + y];
            // Row 0 of the data is the lowest frequency; row 0 of a texture is the top.
            dc::dbToRgba(row_.data(), report->columns, -100.0f, 0.0f, static_cast<dc::Colormap>(map),
                         pixels_.data() + static_cast<size_t>(kRows - 1 - y) * report->columns);
        }
        c.textureUpdate(tex_, 0, 0, report->columns, kRows, pixels_.data(), report->columns * 4);
    }

    void drawAxes(Canvas& c, const Plot& p, float s, const Report& r) const {
        for (const double hz : {100.0, 1000.0, 10000.0}) {
            if (hz > r.maxHz)
                continue;
            const auto f = static_cast<float>(std::log(hz / 20.0) / std::log(r.maxHz / 20.0));
            const float y = p.y0 + p.h * (1.0f - f);
            c.line({p.x0 - 4.0f * s, y}, {p.x0, y}, rgba(170, 174, 184), 1.0f);
            char label[16];
            std::snprintf(label, sizeof(label), hz >= 1000.0 ? "%g k" : "%g", hz >= 1000.0 ? hz / 1000.0 : hz);
            c.text({p.x0 - 40.0f * s, y - 7.0f * s}, rgba(200, 204, 214), label);
        }
        for (int i = 0; i <= 4; ++i) {
            const float x = p.x0 + p.w * static_cast<float>(i) / 4.0f;
            c.line({x, p.y0 + p.h}, {x, p.y0 + p.h + 4.0f * s}, rgba(170, 174, 184), 1.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%.1f s", r.seconds * i / 4.0);
            c.text({x - 14.0f * s, p.y0 + p.h + 5.0f * s}, rgba(200, 204, 214), label);
        }
    }

    void exportCsv(const Report& r) {
        std::string text = "measure,value,unit\n";
        const auto add = [&](const char* name, double value, const char* unit) {
            text += std::string(name) + "," + (std::isfinite(value) ? jsonNumber(value, "%.6f") : std::string()) + "," + unit + "\n";
        };
        add("duration", r.seconds, "s");
        add("sample_rate", r.sampleRate, "Hz");
        add("integrated_loudness", r.integratedLufs, "LUFS");
        add("loudness_range", r.rangeLu, "LU");
        add("true_peak", r.truePeakDbtp, "dBTP");
        add("sample_peak", r.samplePeakDbfs, "dBFS");
        add("dc_offset_left", r.dcOffset[0], "");
        add("dc_offset_right", r.dcOffset[1], "");
        const std::string path = writeCsv("file-analysis", text);
        exportText_ = path.empty() ? "Couldn't write the file." : "Saved " + path;
    }

    std::atomic<std::shared_ptr<const Report>> report_;
    std::shared_ptr<const Report> shown_; // UI thread: what the texture holds
    int shownMap_ = -1;
    dsptb_texture tex_ = 0;
    std::vector<uint32_t> pixels_;
    std::vector<float> row_;
    std::string exportText_;
};

} // namespace dsptb::analysis
