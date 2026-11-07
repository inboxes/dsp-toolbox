#pragma once

// Shared by the Audio Analysis modules: scrolling history, plot layout, input cleanup and CSV export.

#include "csv_export.hpp"

#include <dsptb/dsp_common/stft.hpp>
#include <dsptb/rt/spsc_ring.hpp>
#include <dsptb/sdk.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace dsptb::analysis {

namespace dc = dsptb::dsp;

constexpr const char* kChannelLabels[] = {"Mid (L+R)/2", "Left", "Right", "Side (L-R)/2"};

inline int roundInt(float v) {
    return static_cast<int>(v + (v < 0 ? -0.5f : 0.5f));
}

// Mid, Left, Right or Side as one clean channel. A NaN or Inf in the stream would poison every frame that
// overlaps it.
inline void mixClean(float channel, const float* l, const float* r, uint32_t n, std::vector<float>& mono) {
    if (mono.size() < n)
        mono.resize(n);
    const auto mix = static_cast<dc::ChannelMix>(std::clamp(roundInt(channel), 0, 3));
    dc::mixToMono(l, r, n, mix, mono.data());
    for (uint32_t i = 0; i < n; ++i)
        if (!std::isfinite(mono[i]))
            mono[i] = 0.0f;
}

// Records travel from the analysis thread to the UI through a ring. The UI thread keeps the part inside the
// history window. T needs a `double t` in seconds.
template <class T> class TimeHistory {
public:
    explicit TimeHistory(size_t ringCapacity) : ring_(ringCapacity) {}

    void push(const T& record) noexcept { ring_.tryPush(record); } // analysis thread; drops when full

    // UI thread: takes what arrived and forgets what's older than the window.
    void update(double historySeconds) {
        T record;
        while (ring_.tryPop(record))
            items_.push_back(record);
        while (!items_.empty() && items_.back().t - items_.front().t > historySeconds)
            items_.pop_front();
    }
    const std::deque<T>& items() const { return items_; }
    void clear() { items_.clear(); }

private:
    dsptb::rt::SpscRing<T> ring_;
    std::deque<T> items_;
};

struct Plot {
    float x0, y0, w, h;

    float xOf(double t, double newest, double history) const {
        return x0 + w * static_cast<float>(1.0 - (newest - t) / history);
    }
    double timeAt(float x, double newest, double history) const {
        return newest - history * (1.0 - static_cast<double>((x - x0) / w));
    }
    bool contains(dsptb_vec2 pos) const { return pos.x >= x0 && pos.x <= x0 + w && pos.y >= y0 && pos.y <= y0 + h; }
};

// How far from a reading the mouse can be, in time, and still count as being on it. Longer than any hop, and short
// enough that a gap in the data reads as a gap.
constexpr double kHoverToleranceSeconds = 0.05;

// The record closest to `t`, or null if none is within `tolerance`.
template <class Items>
const typename Items::value_type* nearestInTime(const Items& items, double t, double tolerance = kHoverToleranceSeconds) {
    using Record = typename Items::value_type;
    const auto after = std::lower_bound(items.begin(), items.end(), t, [](const Record& r, double v) { return r.t < v; });
    const Record* best = nullptr;
    double bestGap = tolerance;
    const auto consider = [&](const Record& r) {
        const double gap = std::abs(r.t - t);
        if (gap <= bestGap) {
            best = &r;
            bestGap = gap;
        }
    };
    if (after != items.end())
        consider(*after);
    if (after != items.begin())
        consider(*(after - 1));
    return best;
}

inline void drawHoverLine(Canvas& c, const Plot& p, float x) {
    c.line({x, p.y0}, {x, p.y0 + p.h}, rgba(255, 255, 255, 110), 1.0f);
}

// A label by the mouse, kept inside the plot and flipped to the left near its right edge.
inline void drawHoverTag(Canvas& c, const Plot& p, float s, dsptb_vec2 at, const char* text) {
    const float width = 7.0f * s * static_cast<float>(std::strlen(text)); // the canvas can't measure text
    const float height = 18.0f * s;
    float x = at.x + 10.0f * s;
    if (x + width > p.x0 + p.w)
        x = at.x - 10.0f * s - width;
    x = std::max(x, p.x0);
    const float y = std::max(p.y0, std::min(at.y + 12.0f * s, p.y0 + p.h - height));
    c.fillRect({x, y}, {x + width, y + height}, rgba(0, 0, 0, 200));
    c.text({x + 3.0f * s, y + 2.0f * s}, rgba(255, 255, 255), text);
}

// The time axis all four analyzers share, matching the Spectrogram's: 0 at the right edge.
inline void drawTimeAxis(Canvas& c, const Plot& p, float s, double history) {
    constexpr double kSteps[] = {1, 2, 5, 10, 15, 30, 60};
    double step = 60.0;
    for (double st : kSteps)
        if (history / st <= 8.0) {
            step = st;
            break;
        }
    for (double t = 0.0; t <= history + 1e-3; t += step) {
        const float x = p.x0 + p.w * static_cast<float>(1.0 - t / history);
        c.line({x, p.y0 + p.h}, {x, p.y0 + p.h + 4.0f * s}, rgba(170, 174, 184), 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), t > 0.0 ? "-%g s" : "0 s", t);
        c.text({x - 12.0f * s, p.y0 + p.h + 5.0f * s}, rgba(200, 204, 214), label);
    }
}

inline void drawPlotFrame(Canvas& c, const Plot& p) {
    c.fillRect({p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h}, rgba(8, 8, 12));
}
inline void drawPlotBorder(Canvas& c, const Plot& p) {
    c.rect({p.x0, p.y0}, {p.x0 + p.w, p.y0 + p.h}, rgba(90, 94, 104), 1.0f);
}

// A number for JSON, where infinities and NaN don't exist.
inline std::string jsonNumber(double v, const char* format = "%.6f") {
    if (!std::isfinite(v))
        return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, v);
    return buf;
}

} // namespace dsptb::analysis
