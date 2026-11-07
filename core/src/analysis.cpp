#include "dsptb/core/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace dsptb {

double linearToDbfs(double linear) {
    if (linear <= 0.0)
        return kDbfsFloor;
    return std::max(kDbfsFloor, 20.0 * std::log10(linear));
}

AnalysisResult analyze(const PcmAudio& audio, const AnalysisOptions& options) {
    AnalysisResult r;
    r.sampleRate = audio.sampleRate;
    r.channels = audio.channels;
    const size_t total = audio.frames();
    const double sr = static_cast<double>(std::max<uint32_t>(1, audio.sampleRate));
    size_t begin = static_cast<size_t>(std::max(0.0, options.startSeconds) * sr);
    size_t end = options.endSeconds > 0.0 ? static_cast<size_t>(options.endSeconds * sr) : total;
    begin = std::min(begin, total);
    end = std::clamp(end, begin, total);
    r.frames = end - begin;
    r.seconds = static_cast<double>(r.frames) / sr;
    r.perChannel.assign(audio.channels, ChannelStats{});
    if (audio.channels == 0 || r.frames == 0)
        return r;

    std::vector<double> sumSq(audio.channels, 0.0), sum(audio.channels, 0.0);
    for (size_t f = begin; f < end; ++f) {
        for (uint32_t c = 0; c < audio.channels; ++c) {
            const double x = audio.samples[f * audio.channels + c];
            ChannelStats& s = r.perChannel[c];
            const double a = std::fabs(x);
            if (a > s.peak)
                s.peak = a;
            sumSq[c] += x * x;
            sum[c] += x;
            if (s.onsetSample < 0 && a > 1e-4)
                s.onsetSample = static_cast<int64_t>(f - begin);
        }
    }
    double allSq = 0.0;
    const double n = static_cast<double>(r.frames);
    for (uint32_t c = 0; c < audio.channels; ++c) {
        ChannelStats& s = r.perChannel[c];
        s.rms = std::sqrt(sumSq[c] / n);
        s.dc = sum[c] / n;
        s.peakDbfs = linearToDbfs(s.peak);
        s.rmsDbfs = linearToDbfs(s.rms);
        r.peakDbfs = std::max(r.peakDbfs, s.peakDbfs);
        allSq += sumSq[c];
    }
    r.rmsDbfs = linearToDbfs(std::sqrt(allSq / (n * audio.channels)));

    if (options.windowMs > 0.0) {
        r.windowMs = options.windowMs;
        const size_t win = std::max<size_t>(1, static_cast<size_t>(options.windowMs * 0.001 * sr));
        for (size_t w = begin; w < end; w += win) {
            const size_t we = std::min(end, w + win);
            std::vector<double> row(audio.channels, 0.0);
            for (uint32_t c = 0; c < audio.channels; ++c) {
                double acc = 0.0;
                for (size_t f = w; f < we; ++f) {
                    const double x = audio.samples[f * audio.channels + c];
                    acc += x * x;
                }
                row[c] = linearToDbfs(std::sqrt(acc / static_cast<double>(we - w)));
            }
            r.windowRmsDbfs.push_back(std::move(row));
        }
    }
    return r;
}

CompareResult compareAudio(const PcmAudio& a, const PcmAudio& b) {
    CompareResult r;
    if (a.channels != b.channels)
        r.notes.push_back("channel counts differ: " + std::to_string(a.channels) + " vs " +
                          std::to_string(b.channels));
    if (a.sampleRate != b.sampleRate)
        r.notes.push_back("sample rates differ: " + std::to_string(a.sampleRate) + " vs " +
                          std::to_string(b.sampleRate));
    if (a.frames() != b.frames())
        r.notes.push_back("lengths differ: " + std::to_string(a.frames()) + " vs " +
                          std::to_string(b.frames()) + " frames");
    r.shapeMatches = r.notes.empty();
    if (a.channels != b.channels || a.channels == 0)
        return r;
    const size_t frames = std::min(a.frames(), b.frames());
    for (size_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < a.channels; ++c) {
            const double d = std::fabs(static_cast<double>(a.samples[f * a.channels + c]) -
                                       static_cast<double>(b.samples[f * b.channels + c]));
            if (d > r.maxAbsDiff) {
                r.maxAbsDiff = d;
                r.firstDiffFrame = f;
            }
        }
    }
    return r;
}

} // namespace dsptb
