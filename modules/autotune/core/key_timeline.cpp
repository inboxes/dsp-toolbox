#include "key_timeline.hpp"

#include <algorithm>
#include <cmath>

namespace dsptb::autotune {

int sameNotesId(int root, Scale scale) noexcept {
    return scale == Scale::Minor ? (root + 3) % 12 : root % 12; // the relative major
}

namespace {

DetectedKey toKey(const KeyWindow& w) {
    return {true, true, w.root, w.scale};
}

} // namespace

std::vector<DetectedKey> resolveKeyTimeline(const std::vector<KeyWindow>& windows) {
    std::vector<DetectedKey> out(windows.size());
    const auto first = std::find_if(windows.begin(), windows.end(), [](const KeyWindow& w) { return w.confident; });
    if (first == windows.end())
        return out;

    DetectedKey current = toKey(*first);
    int currentId = sameNotesId(first->root, first->scale);
    for (size_t i = 0; i < windows.size(); ++i) {
        const KeyWindow& w = windows[i];
        if (w.confident) {
            const int id = sameNotesId(w.root, w.scale);
            const bool confirmed = i > 0 && windows[i - 1].confident &&
                                   sameNotesId(windows[i - 1].root, windows[i - 1].scale) == id;
            if (id != currentId && confirmed) {
                current = toKey(windows[i - 1]);
                currentId = id;
                out[i - 1] = current;
            } else if (id == currentId) {
                current = toKey(w); // same notes: show the name this window found (E minor rather than G major)
            }
        }
        out[i] = current;
    }
    return out;
}

size_t keyWindowAt(double seconds, double lengthSeconds, double hopSeconds, size_t count) noexcept {
    if (count == 0)
        return 0;
    const double index = std::round((seconds - 0.5 * lengthSeconds) / hopSeconds);
    return static_cast<size_t>(std::clamp(index, 0.0, static_cast<double>(count - 1)));
}

} // namespace dsptb::autotune
