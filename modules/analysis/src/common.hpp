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
};

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
