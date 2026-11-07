#include "dsptb/core/notifications.hpp"

#include <algorithm>

namespace dsptb {

uint64_t NotificationCenter::push(NoticeLevel level, std::string text, double now) {
    // An identical message that is already showing just has its timer refreshed.
    for (auto& n : notices_) {
        if (n.level == level && n.text == text) {
            n.expiresAt = now + kLifetimeSeconds;
            return n.id;
        }
    }
    notices_.push_back({nextId_++, level, std::move(text), now + kLifetimeSeconds});
    if (notices_.size() > kMaxVisible)
        notices_.erase(notices_.begin());
    return notices_.back().id;
}

void NotificationCenter::dismiss(uint64_t id) {
    notices_.erase(
        std::remove_if(notices_.begin(), notices_.end(), [id](const Notice& n) { return n.id == id; }),
        notices_.end());
}

void NotificationCenter::update(double now) {
    notices_.erase(std::remove_if(notices_.begin(), notices_.end(),
                                  [now](const Notice& n) { return now >= n.expiresAt; }),
                   notices_.end());
}

} // namespace dsptb
