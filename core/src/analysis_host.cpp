#include "dsptb/core/analysis_host.hpp"

#include <algorithm>
#include <chrono>

namespace dsptb {

AnalysisHost::AnalysisHost(bool startThread) : autoStart_(startThread) {}

AnalysisHost::~AnalysisHost() {
    stopShared_.store(true);
    if (shared_.joinable())
        shared_.join();
    std::lock_guard lock(mutex_);
    for (auto& r : runners_) {
        r->stop.store(true);
        if (r->thread.joinable())
            r->thread.join();
    }
    runners_.clear();
}

AnalysisHost::Runner* AnalysisHost::findLocked(InstanceId id) const {
    for (const auto& r : runners_)
        if (r->inst->id() == id)
            return r.get();
    return nullptr;
}

void AnalysisHost::drain(Runner& r) noexcept {
    TapRing& tap = *r.ring;
    for (;;) {
        const size_t availFrames = tap.ring.size() / 2;
        if (availFrames == 0)
            return;
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(availFrames, kChunk));
        tap.ring.read(r.inter.data(), static_cast<size_t>(n) * 2);
        for (uint32_t i = 0; i < n; ++i) {
            r.l[i] = r.inter[2 * i];
            r.r[i] = r.inter[2 * i + 1];
        }
        const uint64_t droppedNow = tap.droppedFrames.load(std::memory_order_relaxed);
        const uint64_t lost = droppedNow - r.lastDropped;
        r.lastDropped = droppedNow;
        r.clock += lost; // the lost frames sat before this chunk on the sample clock
        const uint32_t gap = r.gap.exchange(0, std::memory_order_relaxed);
        const uint32_t reported = static_cast<uint32_t>(std::min<uint64_t>(lost + gap, 0xFFFFFFFFu));
        if (!r.inst->faulted()) {
            if (!r.inst->callAnalyze(r.l.data(), r.r.data(), n, r.clock, reported,
                                     static_cast<dsptb_tap>(r.tap.load(std::memory_order_relaxed))))
                r.inst->markFaulted(FaultReason::Exception);
        }
        r.clock += n;
        r.analyzed.fetch_add(n, std::memory_order_relaxed);
    }
}

void AnalysisHost::sharedLoop() {
    while (!stopShared_.load(std::memory_order_relaxed)) {
        {
            std::lock_guard lock(mutex_);
            for (auto& r : runners_)
                if (!r->ownThread)
                    drain(*r);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // ~200 Hz
    }
}

std::shared_ptr<TapRing> AnalysisHost::attach(const std::shared_ptr<ModuleInstance>& inst, dsptb_tap tap,
                                              uint32_t sampleRate) {
    detach(inst->id());
    const size_t capacity = std::max<size_t>(static_cast<size_t>(sampleRate * kTapSeconds), kChunk * 2);
    auto runner = std::make_unique<Runner>();
    runner->inst = inst;
    runner->ring = std::make_shared<TapRing>(capacity);
    runner->tap.store(tap);
    runner->inter.assign(static_cast<size_t>(kChunk) * 2, 0.0f);
    runner->l.assign(kChunk, 0.0f);
    runner->r.assign(kChunk, 0.0f);
    runner->ownThread = (inst->desc().caps & DSPTB_CAP_OWN_THREAD) != 0;
    std::shared_ptr<TapRing> ring = runner->ring;

    Runner* raw = runner.get();
    {
        std::lock_guard lock(mutex_);
        runners_.push_back(std::move(runner));
    }
    if (raw->ownThread && autoStart_) {
        raw->thread = std::thread([raw] {
            while (!raw->stop.load(std::memory_order_relaxed)) {
                drain(*raw);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    } else if (autoStart_ && !shared_.joinable()) {
        shared_ = std::thread([this] { sharedLoop(); });
    }
    return ring;
}

void AnalysisHost::detach(InstanceId id) {
    std::unique_ptr<Runner> gone;
    {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(runners_.begin(), runners_.end(),
                                     [id](const auto& r) { return r->inst->id() == id; });
        if (it == runners_.end())
            return;
        gone = std::move(*it);
        runners_.erase(it);
    }
    // Outside the lock: the shared thread may be waiting on it, and an own thread needs to finish its pass.
    gone->stop.store(true);
    if (gone->thread.joinable())
        gone->thread.join();
}

bool AnalysisHost::attached(InstanceId id) const {
    std::lock_guard lock(mutex_);
    return findLocked(id) != nullptr;
}

std::shared_ptr<TapRing> AnalysisHost::ring(InstanceId id) const {
    std::lock_guard lock(mutex_);
    const Runner* r = findLocked(id);
    return r ? r->ring : nullptr;
}

void AnalysisHost::setTap(InstanceId id, dsptb_tap tap) {
    std::lock_guard lock(mutex_);
    if (Runner* r = findLocked(id))
        r->tap.store(tap);
}

void AnalysisHost::markGap(InstanceId id) {
    std::lock_guard lock(mutex_);
    if (Runner* r = findLocked(id))
        r->gap.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AnalysisHost::droppedFrames(InstanceId id) const {
    std::lock_guard lock(mutex_);
    const Runner* r = findLocked(id);
    return r ? r->ring->droppedFrames.load(std::memory_order_relaxed) : 0;
}

uint64_t AnalysisHost::analyzedFrames(InstanceId id) const {
    std::lock_guard lock(mutex_);
    const Runner* r = findLocked(id);
    return r ? r->analyzed.load(std::memory_order_relaxed) : 0;
}

void AnalysisHost::pumpOnce() {
    std::lock_guard lock(mutex_);
    for (auto& r : runners_)
        if (!r->ownThread)
            drain(*r);
}

} // namespace dsptb
