#pragma once

#include "chain.hpp"
#include "module_instance.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dsptb {

// Runs analyzer modules off the audio thread.
//
// Each attached analyzer owns a TapRing that the audio thread fills (PRE or POST tap). One shared thread
// wakes at about 200 Hz and drains every ring into `analyze()` in chunks of at most kChunk frames. An
// analyzer with DSPTB_CAP_OWN_THREAD gets a thread of its own instead. Frames the audio thread could not fit
// into a full ring are reported to the module as `dropped_frames` on its next call.
//
// Thread model: attach/detach/markGap and the queries are UI-thread calls. A runner is only ever consumed by
// one thread, which is what keeps the ring single-consumer. The host holds the instance (shared_ptr) while it
// runs, so detach() on the UI thread is also where the last reference to a removed analyzer is dropped.
class AnalysisHost {
public:
    static constexpr uint32_t kChunk = 4096;   // largest block analyze() receives
    static constexpr double kTapSeconds = 1.0; // ring capacity

    // `startThread` false: nothing runs until pumpOnce() is called (deterministic tests).
    explicit AnalysisHost(bool startThread = true);
    ~AnalysisHost();
    AnalysisHost(const AnalysisHost&) = delete;
    AnalysisHost& operator=(const AnalysisHost&) = delete;

    // Creates the instance's tap ring (at least one second at `sampleRate`) and starts feeding it to the
    // module. The caller puts the returned ring into the chain snapshot. An already attached instance is
    // replaced (new ring), which is what a sample-rate change needs.
    std::shared_ptr<TapRing> attach(const std::shared_ptr<ModuleInstance>& inst, dsptb_tap tap,
                                    uint32_t sampleRate);
    void detach(InstanceId id);
    bool attached(InstanceId id) const;
    std::shared_ptr<TapRing> ring(InstanceId id) const;

    void setTap(InstanceId id, dsptb_tap tap);
    // The next analyze() call for this instance reports one lost frame, so modules draw a gap where the
    // signal was not delivered (the analyzer was disabled for a while, or the source was swapped).
    void markGap(InstanceId id);

    // Frames the audio thread dropped for this instance so far (ring was full).
    uint64_t droppedFrames(InstanceId id) const;
    // Frames handed to analyze() so far.
    uint64_t analyzedFrames(InstanceId id) const;

    // Drains every shared-thread runner once, on the calling thread. Used when constructed without a thread.
    void pumpOnce();

private:
    struct Runner {
        std::shared_ptr<ModuleInstance> inst;
        std::shared_ptr<TapRing> ring;
        std::atomic<int> tap{DSPTB_TAP_POST};
        std::atomic<uint32_t> gap{0};
        std::atomic<uint64_t> analyzed{0};
        uint64_t clock = 0;       // sample clock of the next frame handed out
        uint64_t lastDropped = 0; // ring->droppedFrames when we last looked
        std::vector<float> inter, l, r;
        bool ownThread = false;
        std::thread thread;
        std::atomic<bool> stop{false};
    };

    static void drain(Runner& r) noexcept;
    void sharedLoop();
    Runner* findLocked(InstanceId id) const;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Runner>> runners_;
    std::thread shared_;
    std::atomic<bool> stopShared_{false};
    bool autoStart_;
};

} // namespace dsptb
