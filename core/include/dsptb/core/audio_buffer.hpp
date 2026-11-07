#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsptb {

// Interleaved float samples with a native channel count. Used by file I/O, the generators and the analyzer.
struct PcmAudio {
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    std::vector<float> samples; // interleaved: frame0ch0, frame0ch1, ...

    size_t frames() const noexcept { return channels ? samples.size() / channels : 0; }
    float at(size_t frame, uint32_t channel) const noexcept { return samples[frame * channels + channel]; }
};

// The engine's internal bus format: stereo, float32, planar.
struct AudioBuffer {
    uint32_t sampleRate = 0;
    std::array<std::vector<float>, 2> ch;

    size_t frames() const noexcept { return ch[0].size(); }
    void resize(size_t frames) {
        ch[0].assign(frames, 0.0f);
        ch[1].assign(frames, 0.0f);
    }
};

// Min/max per column of the mono mix (L+R)/2, computed once when a file is loaded. The UI merges
// columns down to its pixel width.
struct WaveformOverview {
    std::vector<float> minimum;
    std::vector<float> maximum;
    size_t framesPerColumn = 1;
};

WaveformOverview computeOverview(const AudioBuffer& audio, size_t columns = 4096);

} // namespace dsptb
