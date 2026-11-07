#include "pitch_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dsptb::autotune {

namespace {

constexpr double kWindowSeconds = 5.0;
constexpr size_t kMaxFrames = 2048; // about 188 frames a second, at every rate
constexpr float kHalfRange = 7.0f;  // semitones either side of the centre
constexpr float kLabelWidth = 34.0f;

constexpr uint32_t rgb(uint32_t value) {
    return rgba((value >> 16) & 255, (value >> 8) & 255, value & 255);
}
constexpr uint32_t kBackground = rgb(0x16181C);
constexpr uint32_t kInScaleLane = rgb(0x3A3F48);
constexpr uint32_t kOtherLane = rgb(0x25282E);
constexpr uint32_t kLaneLabel = rgb(0x9AA0A6);
constexpr uint32_t kSung = rgb(0x9AA0A6);
constexpr uint32_t kCorrected = rgb(0x4FC3F7);

constexpr const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

} // namespace

void PitchGraph::push(const PitchFrame& frame) {
    if (!history_.empty() && frame.clock < history_.back().clock)
        history_.clear(); // the host's clock restarted
    if (history_.size() >= kMaxFrames)
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(kMaxFrames / 4));
    history_.push_back(frame);
}

void PitchGraph::clear() {
    history_.clear();
}

void PitchGraph::drawLanes(Canvas& canvas, const Key& key, float width, float height, float bottomMidi) const {
    const float semitone = height / (2.0f * kHalfRange);
    const int first = static_cast<int>(std::ceil(bottomMidi));
    const int last = static_cast<int>(std::floor(bottomMidi + 2.0f * kHalfRange));
    for (int note = first; note <= last; ++note) {
        const float y = height - (static_cast<float>(note) - bottomMidi) * semitone;
        const bool bright = inKey(note, key);
        canvas.line({0.0f, y}, {width, y}, bright ? kInScaleLane : kOtherLane, 1.0f);
        const int pc = ((note % 12) + 12) % 12;
        // In Chromatic every lane counts as in the key, so label a few to keep the text apart.
        const bool label = key.scale == Scale::Chromatic ? (pc == 0 || pc == 4 || pc == 7 || pc == 9) : bright;
        if (label) {
            char text[16];
            std::snprintf(text, sizeof(text), "%s%d", kNoteNames[pc], note / 12 - 1);
            canvas.text({4.0f, y - 14.0f}, kLaneLabel, text);
        }
    }
}

void PitchGraph::drawLine(Canvas& canvas, bool corrected, uint32_t colour, float thickness, float width, float height,
                          float bottomMidi, double samples) const {
    if (history_.empty())
        return;
    const uint64_t newest = history_.back().clock;
    const float semitone = height / (2.0f * kHalfRange);
    const auto flush = [&] {
        if (points_.size() >= 2)
            canvas.polyline(points_.data(), static_cast<uint32_t>(points_.size()), colour, thickness);
    };
    auto& points = const_cast<std::vector<dsptb_vec2>&>(points_);
    points.clear();
    for (const PitchFrame& f : history_) {
        const double age = static_cast<double>(newest - f.clock) / samples;
        if (age > 1.0)
            continue;
        if (!f.voiced) { // a gap, never a line down to zero
            flush();
            points.clear();
            continue;
        }
        const float midi = corrected ? f.correctedMidi : f.sungMidi;
        const float x = kLabelWidth + (width - kLabelWidth) * static_cast<float>(1.0 - age);
        points.push_back({x, height - (midi - bottomMidi) * semitone});
    }
    flush();
}

void PitchGraph::draw(Canvas& canvas, const Key& key, double sampleRate) {
    const float s = canvas.uiScale();
    const float width = std::max(canvas.availableSize().x, 120.0f * s);
    const float height = 180.0f * s;

    // Follow the recent singing: the last second of voiced pitch, moved towards a little each frame.
    if (!history_.empty()) {
        const uint64_t newest = history_.back().clock;
        const auto oneSecond = static_cast<uint64_t>(sampleRate);
        double sum = 0.0;
        size_t count = 0;
        for (auto it = history_.rbegin(); it != history_.rend() && newest - it->clock <= oneSecond; ++it)
            if (it->voiced) {
                sum += static_cast<double>(it->sungMidi);
                ++count;
            }
        if (count > 0)
            centreMidi_ += 0.1f * (static_cast<float>(sum / static_cast<double>(count)) - centreMidi_);
    }

    auto region = canvas.region("pitch_graph", width, height);
    if (!region)
        return;
    canvas.fillRect({0.0f, 0.0f}, {width, height}, kBackground);
    auto clip = canvas.clip({0.0f, 0.0f}, {width, height});
    const float bottomMidi = centreMidi_ - kHalfRange;
    drawLanes(canvas, key, width, height, bottomMidi);
    const double samples = kWindowSeconds * sampleRate;
    drawLine(canvas, false, kSung, 1.0f, width, height, bottomMidi, samples);
    drawLine(canvas, true, kCorrected, 2.0f, width, height, bottomMidi, samples);
}

} // namespace dsptb::autotune
