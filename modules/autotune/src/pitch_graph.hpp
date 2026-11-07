#pragma once

// The scrolling pitch graph: what was sung against what came out, over a lane per note.

#include <dsptb/canvas.hpp>

#include "target_picker.hpp"

#include <cstdint>
#include <vector>

namespace dsptb::autotune {

// One analysis hop, from the audio thread to the UI.
struct PitchFrame {
    uint64_t clock = 0;
    float sungMidi = 0.0f;
    float correctedMidi = 0.0f;
    int16_t target = -1;
    uint8_t voiced = 0;
    uint8_t pad = 0;
};

class PitchGraph {
public:
    void push(const PitchFrame& frame);
    void clear();
    void draw(Canvas& canvas, const Key& key, double sampleRate);

private:
    void drawLanes(Canvas& canvas, const Key& key, float width, float height, float bottomMidi) const;
    void drawLine(Canvas& canvas, bool corrected, uint32_t colour, float thickness, float width, float height,
                  float bottomMidi, double samples) const;

    std::vector<PitchFrame> history_; // oldest first, about the last 5 seconds
    float centreMidi_ = 57.0f;        // A3 until someone sings
    std::vector<dsptb_vec2> points_;
};

} // namespace dsptb::autotune
