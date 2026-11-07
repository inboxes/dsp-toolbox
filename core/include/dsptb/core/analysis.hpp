#pragma once

#include "audio_buffer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dsptb {

constexpr double kDbfsFloor = -200.0; // what silence reports (JSON cannot carry -inf)

double linearToDbfs(double linear);

struct ChannelStats {
    double peak = 0.0; // linear, max |x|
    double rms = 0.0;  // linear
    double dc = 0.0;   // mean
    double peakDbfs = kDbfsFloor;
    double rmsDbfs = kDbfsFloor;
    int64_t onsetSample = -1; // first frame with |x| > 1e-4, or -1 (counted from the analysed range start)
};

struct AnalysisOptions {
    double startSeconds = 0.0;
    double endSeconds = 0.0; // 0 = to the end
    double windowMs = 0.0;   // > 0: also compute RMS per window
};

struct AnalysisResult {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    size_t frames = 0; // frames analysed (after start/end)
    double seconds = 0.0;
    std::vector<ChannelStats> perChannel;
    double peakDbfs = kDbfsFloor; // max over channels
    double rmsDbfs = kDbfsFloor;  // RMS over all channels together
    double windowMs = 0.0;
    std::vector<std::vector<double>> windowRmsDbfs; // [window][channel]
};

AnalysisResult analyze(const PcmAudio& audio, const AnalysisOptions& options = {});

struct CompareResult {
    bool shapeMatches = false; // same channel count, sample rate and length
    double maxAbsDiff = 0.0;
    size_t firstDiffFrame = 0; // first frame where the difference is the maximum
    std::vector<std::string> notes;
};

CompareResult compareAudio(const PcmAudio& a, const PcmAudio& b);

} // namespace dsptb
