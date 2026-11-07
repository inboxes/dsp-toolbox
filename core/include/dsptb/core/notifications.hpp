#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsptb {

enum class NoticeLevel { Info, Warning, Error };

struct Notice {
    uint64_t id = 0;
    NoticeLevel level = NoticeLevel::Info;
    std::string text;
    double expiresAt = 0.0; // seconds on the caller's clock
};

// Non-modal banners: they auto-dismiss after kLifetimeSeconds or when clicked.
// UI-thread only; time is passed in so the logic is deterministic and unit-testable.
class NotificationCenter {
public:
    static constexpr double kLifetimeSeconds = 8.0;
    static constexpr size_t kMaxVisible = 4;

    uint64_t push(NoticeLevel level, std::string text, double now);
    void dismiss(uint64_t id);
    // Drops expired notices. Call once per frame.
    void update(double now);
    const std::vector<Notice>& active() const noexcept { return notices_; }
    bool empty() const noexcept { return notices_.empty(); }

private:
    std::vector<Notice> notices_;
    uint64_t nextId_ = 1;
};

} // namespace dsptb
