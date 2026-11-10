// The latency pipeline between a module's process() and an inference worker thread (docs/GPU_RUNTIME.md
// section 7.1). Backend-free: it knows nothing of CUDA, ONNX Runtime or LibTorch. A Stage does the inference.
//
// Timeline. Input frame i is the i-th frame pushed since the last reset. The worker handles hop h = frames
// [h*hop, (h+1)*hop) with a window of `context` past frames, the hop, and `lookahead` future frames, so hop h
// can start once (h+1)*hop + lookahead frames have been pushed. Its output is due when the audio thread
// outputs frame h*hop + latency, with latency = hop + lookahead + budget. The budget is the time allowed for
// compute (plus the audio block granularity, see the doc), and the latency is constant: output frame j is the
// result for input frame j - latency, or the module's dry signal when that result is not ready.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "worker.hpp"

namespace dsptb::gpu {

struct PipelineConfig {
    double sampleRate = 48000.0;    // the rate the stages work at (the module resamples before and after)
    uint32_t hopFrames = 960;       // new input frames per inference, e.g. 20 ms
    uint32_t contextFrames = 0;     // past frames prepended to each window (left context)
    uint32_t lookaheadFrames = 0;   // future frames the model needs (right context)
    uint32_t budgetFrames = 0;      // time allowed for compute, in frames: the declared compute latency
    uint32_t maxBlockFrames = 1024; // the host's max_block_frames
};

// One stage of inference. Called only on the worker thread (or, offline, on the thread that calls push()).
class Stage {
public:
    virtual ~Stage() = default;
    // May allocate, load models, warm up and capture CUDA graphs. Called once, before the first run().
    virtual void prepare() = 0;
    // `window` holds context + hop + lookahead frames; write the `hopFrames` output frames for the hop
    // (the frames after the context) to `out`. Throwing puts the pipeline in the Failed state.
    virtual void run(const float* window, uint32_t windowFrames, float* out, uint32_t hopFrames) = 0;
    // Reported in Stats: whether this stage replays a CUDA graph.
    virtual bool graphed() const { return false; }
    // Releases models and CUDA objects. The pipeline calls it on the thread that called prepare() and run(),
    // when that thread is done: ORT and LibTorch keep per-thread state (handles, contexts) that leaks when a
    // session dies on another thread.
    virtual void teardown() {}
};

// Optional hooks for the process-wide capture lock (dsptb_gpu_lock_* in gpu_runtime.h). Workers hold it
// shared around each run(). A stage takes it exclusively around a CUDA graph capture, not around the whole
// prepare(), so a model that takes a while to load does not stall the other workers (runtime_api.hpp,
// ScopedCapture). Left empty in tests that have no GPU.
struct CaptureLock {
    void (*lockShared)() = nullptr;
    void (*unlockShared)() = nullptr;
    void (*lockExclusive)() = nullptr;
    void (*unlockExclusive)() = nullptr;
};

enum class State { Idle, Loading, Running, Failed };

struct Stats {
    uint64_t runs = 0;   // inferences completed
    uint64_t misses = 0; // hops whose result was not ready when due (dry output used)
    uint32_t consecutiveMisses = 0;
    uint32_t workerWakeLateMaxUs = 0; // worst delay of the worker waking from its 1 ms wait (OS scheduling)
    uint32_t computeAllTimeMaxUs =
        0;                      // worst single run since start (computeMaxUs covers only the latest runs)
    uint32_t hopTotalMaxUs = 0; // worst time of a whole hop on the worker: window copy, lock wait and run
    uint64_t lastMissFrame = 0; // output frame (since the last reset) of the latest miss, for diagnosis
    uint32_t computeP50Us = 0, computeP99Us = 0, computeMaxUs = 0; // rolling, the last 512 runs
    bool tooSlow = false;                                          // 10 or more consecutive misses
    bool graphed = false;
};

class Pipeline {
public:
    static constexpr uint32_t kTooSlowMisses = 10;
    static constexpr double kFadeSeconds = 0.005; // every switch between model and dry output
    static constexpr size_t kStatWindow = 512;

    Pipeline() = default;
    ~Pipeline() { stop(); }
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void setCaptureLock(const CaptureLock& lock) { lock_ = lock; }

    // UI thread (a module's prepare). Allocates every buffer; the pipeline must be stopped. With `offline`
    // there is no worker thread: push() runs the stage synchronously, so a render never misses and a live
    // session and its export line up.
    void configure(const PipelineConfig& cfg, std::unique_ptr<Stage> stage, bool offline = false) {
        stop();
        cfg_ = cfg;
        cfg_.hopFrames = std::max<uint32_t>(1, cfg.hopFrames);
        cfg_.maxBlockFrames = std::max<uint32_t>(1, cfg.maxBlockFrames);
        offline_ = offline;
        stage_ = std::move(stage);
        latency_ = cfg_.hopFrames + cfg_.lookaheadFrames + cfg_.budgetFrames;
        window_ = cfg_.contextFrames + cfg_.hopFrames + cfg_.lookaheadFrames;
        // The input ring holds the window, the latency and some slack for a late worker. A worker that falls
        // further behind skips hops that are already stale, so it never reads frames the audio thread has
        // overwritten.
        inMask_ = nextPow2(static_cast<uint64_t>(window_) + latency_ + 4ull * cfg_.maxBlockFrames +
                           4ull * cfg_.hopFrames) -
                  1;
        inRing_.assign(inMask_ + 1, 0.0f);
        nSlots_ = (latency_ + 4u * cfg_.maxBlockFrames) / cfg_.hopFrames + 4u;
        slots_ = std::make_unique<Slot[]>(nSlots_);
        for (uint32_t i = 0; i < nSlots_; ++i)
            slots_[i].data.assign(cfg_.hopFrames, 0.0f);
        windowBuf_.assign(window_, 0.0f);
        outBuf_.assign(cfg_.hopFrames, 0.0f);
        wetScratch_.assign(cfg_.hopFrames, 0.0f);
        computeUs_.assign(kStatWindow, 0u);
        sortScratch_.assign(kStatWindow, 0u);
        fadeStep_ = static_cast<float>(1.0 / std::max(1.0, kFadeSeconds * cfg_.sampleRate));
        resetState();
        state_.store(State::Idle, std::memory_order_release);
    }

    // UI thread. Offline: loads the stage here, on the calling thread, and returns when it is ready.
    void start() {
        if (!stage_ || running_)
            return;
        stop_.store(false, std::memory_order_relaxed);
        running_ = true;
        state_.store(State::Loading, std::memory_order_release);
        if (offline_) {
            if (prepareStage()) {
                nextHop_ = 0;
                state_.store(State::Running, std::memory_order_release);
            }
            return;
        }
        worker_ = std::thread([this] { workerMain(); });
    }

    // UI thread. Joins the worker, so nothing uses the stage afterwards (call before freeing models).
    void stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable())
            worker_.join();
        else if (offline_ && running_ && stage_)
            stage_->teardown(); // offline: the calling thread ran the stage
        if (running_) {
            running_ = false;
            const State s = state_.load(std::memory_order_acquire);
            if (s != State::Failed)
                state_.store(State::Idle, std::memory_order_release);
        }
    }

    // ---- AUDIO THREAD: no allocation, no locks, no waiting ----

    // Input samples at sampleRate.
    void push(const float* mono, uint32_t n) noexcept {
        for (uint32_t k = 0; k < n; ++k)
            inRing_[static_cast<size_t>(static_cast<uint64_t>(inPos_ + k) & inMask_)] = mono ? mono[k] : 0.0f;
        inPos_ += n;
        inCount_.store(inPos_, std::memory_order_release);
        if (offline_ && state_.load(std::memory_order_acquire) == State::Running)
            drainOffline();
    }

    // Output delayed by exactly latencyFrames(). `dry` (may be null) is the module's own signal for the same
    // output frames, used for any frame whose result is not ready (a miss) and while the model loads or has
    // failed.
    void pull(float* out, uint32_t n, const float* dry) noexcept {
        const State state = state_.load(std::memory_order_acquire);
        const bool modelUp = state == State::Running;
        const uint32_t epoch = epoch_.load(std::memory_order_relaxed);
        int64_t j = outPos_;
        uint32_t done = 0;
        while (done < n) {
            const int64_t i = j - static_cast<int64_t>(latency_);
            uint32_t seg;
            bool ok = false;
            const float* wet = nullptr; // null: silence (the first `latency` frames after a reset)
            int64_t hop = -1;
            if (i < 0) {
                seg = static_cast<uint32_t>(std::min<int64_t>(n - done, -i));
                ok = modelUp;
            } else {
                hop = i / cfg_.hopFrames;
                const uint32_t off = static_cast<uint32_t>(i - hop * cfg_.hopFrames);
                seg = std::min<uint32_t>(n - done, cfg_.hopFrames - off);
                if (modelUp)
                    ok = readSlot(hop, off, seg, epoch);
                if (ok)
                    wet = wetScratch_.data();
            }
            mix(out + done, seg, dry ? dry + done : nullptr, wet, ok);
            if (i >= 0 && modelUp && hop >= firstDueHop_.load(std::memory_order_relaxed))
                countMiss(hop, ok, j);
            j += seg;
            done += seg;
        }
        outPos_ = j;
        outPub_.store(j, std::memory_order_relaxed);
    }

    // Flushes the rings and restarts the timeline, so the next pushed frame is input frame 0.
    void reset() noexcept {
        inPos_ = 0;
        outPos_ = 0;
        if (state_.load(std::memory_order_acquire) == State::Running)
            fade_ = 1.0f;
        lastWet_ = 0.0f;
        lastMissHop_ = -1;
        consecutive_.store(0, std::memory_order_relaxed);
        inCount_.store(0, std::memory_order_relaxed);
        outPub_.store(0, std::memory_order_relaxed);
        firstDueHop_.store(0, std::memory_order_relaxed);
        epoch_.fetch_add(
            1, std::memory_order_release); // the worker restarts from hop 0 and drops a result in flight
        if (offline_)
            nextHop_ = 0;
    }

    uint32_t latencyFrames() const noexcept { return latency_; }
    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    Stats stats() const noexcept {
        Stats s;
        s.runs = runs_.load(std::memory_order_relaxed);
        s.misses = misses_.load(std::memory_order_relaxed);
        s.consecutiveMisses = consecutive_.load(std::memory_order_relaxed);
        s.lastMissFrame = lastMissFrame_.load(std::memory_order_relaxed);
        s.workerWakeLateMaxUs = wakeLateMax_.load(std::memory_order_relaxed);
        s.hopTotalMaxUs = hopTotalMax_.load(std::memory_order_relaxed);
        s.computeAllTimeMaxUs = computeAllMax_.load(std::memory_order_relaxed);
        s.computeP50Us = p50_.load(std::memory_order_relaxed);
        s.computeP99Us = p99_.load(std::memory_order_relaxed);
        s.computeMaxUs = max_.load(std::memory_order_relaxed);
        s.tooSlow = s.consecutiveMisses >= kTooSlowMisses;
        s.graphed = graphed_.load(std::memory_order_relaxed);
        return s;
    }
    // UI thread: why the pipeline is Failed (empty otherwise).
    std::string lastError() const {
        std::lock_guard<std::mutex> g(errorMutex_);
        return error_;
    }

private:
    struct Slot {
        std::atomic<int64_t> tag{-1}; // (epoch << 32) | hop index, or -1 while empty or being written
        std::vector<float> data;
    };

    static uint64_t nextPow2(uint64_t v) {
        uint64_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }
    static int64_t makeTag(uint32_t epoch, int64_t hop) {
        return (static_cast<int64_t>(epoch & 0x7fffffffu) << 32) | (hop & 0xffffffffll);
    }

    void resetState() {
        inPos_ = 0;
        outPos_ = 0;
        fade_ = 1.0f; // starts on the model output: the first `latency` frames are its silence, so no dry
                      // leaks in
        lastWet_ = 0.0f;
        lastMissHop_ = -1;
        nextHop_ = 0;
        inCount_.store(0);
        outPub_.store(0);
        firstDueHop_.store(0);
        consecutive_.store(0);
        misses_.store(0);
        runs_.store(0);
        wakeLateMax_.store(0);
        hopTotalMax_.store(0);
        computeAllMax_.store(0);
        p50_.store(0);
        p99_.store(0);
        max_.store(0);
        graphed_.store(false);
        statCount_ = 0;
        for (uint32_t i = 0; i < nSlots_; ++i)
            slots_[i].tag.store(-1);
        error_.clear();
    }

    // ---- audio side helpers ----

    // Copies `seg` frames of hop `hop` (from offset `off`) into the scratch buffer if the result is there.
    // The tag is read before and after, so a slot the worker is rewriting is never used.
    bool readSlot(int64_t hop, uint32_t off, uint32_t seg, uint32_t epoch) noexcept {
        Slot& s = slots_[static_cast<size_t>(hop % nSlots_)];
        const int64_t want = makeTag(epoch, hop);
        if (s.tag.load(std::memory_order_acquire) != want)
            return false;
        std::memcpy(wetScratch_.data(), s.data.data() + off, sizeof(float) * seg);
        return s.tag.load(std::memory_order_acquire) == want;
    }

    // out = model output when `ok`, else the dry signal; every switch is an equal-power fade of kFadeSeconds.
    // While a fade runs and the model result is missing, the last model sample is held, so there is no step.
    void mix(float* out, uint32_t n, const float* dry, const float* wet, bool ok) noexcept {
        for (uint32_t k = 0; k < n; ++k) {
            const float d = dry ? dry[k] : 0.0f;
            if (ok)
                fade_ = std::min(1.0f, fade_ + fadeStep_);
            else
                fade_ = std::max(0.0f, fade_ - fadeStep_);
            const float w = ok ? (wet ? wet[k] : 0.0f) : lastWet_;
            if (ok)
                lastWet_ = w;
            if (fade_ >= 1.0f)
                out[k] = w;
            else if (fade_ <= 0.0f)
                out[k] = d;
            else
                out[k] = std::sin(fade_ * 1.5707963f) * w + std::cos(fade_ * 1.5707963f) * d;
        }
    }

    void countMiss(int64_t hop, bool ok, int64_t frame) noexcept {
        if (!ok) {
            if (hop != lastMissHop_) {
                lastMissHop_ = hop;
                lastMissFrame_.store(static_cast<uint64_t>(frame), std::memory_order_relaxed);
                misses_.fetch_add(1, std::memory_order_relaxed);
                consecutive_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (hop > lastMissHop_ && consecutive_.load(std::memory_order_relaxed) != 0) {
            consecutive_.store(0, std::memory_order_relaxed);
        }
    }

    // ---- worker side ----

    bool prepareStage() {
        try {
            stage_->prepare();
            return true;
        } catch (const std::exception& e) {
            fail(std::string("loading failed: ") + e.what());
        } catch (...) {
            fail("loading failed: unknown error");
        }
        return false;
    }

    void fail(const std::string& why) {
        {
            std::lock_guard<std::mutex> g(errorMutex_);
            error_ = why;
        }
        state_.store(State::Failed, std::memory_order_release);
    }

    // Builds the window for hop `hop`, runs the stage and publishes the result. Returns false when the stage
    // threw.
    bool processHop(int64_t hop, uint32_t epoch) {
        const int64_t hopStart = nowMicros();
        const int64_t start = hop * cfg_.hopFrames - static_cast<int64_t>(cfg_.contextFrames);
        for (uint32_t k = 0; k < window_; ++k) {
            const int64_t f = start + k;
            windowBuf_[k] = f < 0 ? 0.0f : inRing_[static_cast<size_t>(f & static_cast<int64_t>(inMask_))];
        }
        // The audio thread may have lapped the ring (a worker that was stopped for a long time) or reset.
        if (inCount_.load(std::memory_order_acquire) - start > static_cast<int64_t>(inMask_ + 1) ||
            epoch_.load(std::memory_order_acquire) != epoch)
            return true;
        const int64_t t0 = nowMicros();
        try {
            if (lock_.lockShared)
                lock_.lockShared();
            try {
                stage_->run(windowBuf_.data(), window_, outBuf_.data(), cfg_.hopFrames);
            } catch (...) {
                if (lock_.unlockShared)
                    lock_.unlockShared();
                throw;
            }
            if (lock_.unlockShared)
                lock_.unlockShared();
        } catch (const std::exception& e) {
            fail(std::string("inference failed: ") + e.what());
            return false;
        } catch (...) {
            fail("inference failed: unknown error");
            return false;
        }
        const int64_t us = nowMicros() - t0;
        if (us > static_cast<int64_t>(computeAllMax_.load(std::memory_order_relaxed)))
            computeAllMax_.store(static_cast<uint32_t>(us), std::memory_order_relaxed);
        if (epoch_.load(std::memory_order_acquire) == epoch) {
            Slot& s = slots_[static_cast<size_t>(hop % nSlots_)];
            s.tag.store(-1, std::memory_order_release);
            std::memcpy(s.data.data(), outBuf_.data(), sizeof(float) * cfg_.hopFrames);
            s.tag.store(makeTag(epoch, hop), std::memory_order_release);
        }
        recordCompute(us);
        const int64_t total = nowMicros() - hopStart;
        if (total > static_cast<int64_t>(hopTotalMax_.load(std::memory_order_relaxed)))
            hopTotalMax_.store(static_cast<uint32_t>(total), std::memory_order_relaxed);
        return true;
    }

    void recordCompute(int64_t us) {
        computeUs_[statCount_ % kStatWindow] = static_cast<uint32_t>(std::max<int64_t>(0, us));
        ++statCount_;
        runs_.fetch_add(1, std::memory_order_relaxed);
        graphed_.store(stage_->graphed(), std::memory_order_relaxed);
        if (statCount_ % 8 == 0 || statCount_ < 8) {
            const size_t n = std::min<size_t>(statCount_, kStatWindow);
            std::copy(computeUs_.begin(), computeUs_.begin() + static_cast<std::ptrdiff_t>(n),
                      sortScratch_.begin());
            std::sort(sortScratch_.begin(), sortScratch_.begin() + static_cast<std::ptrdiff_t>(n));
            p50_.store(sortScratch_[n / 2], std::memory_order_relaxed);
            p99_.store(sortScratch_[std::min(n - 1, n * 99 / 100)], std::memory_order_relaxed);
            max_.store(sortScratch_[n - 1], std::memory_order_relaxed);
        }
    }

    // Offline: runs every hop that is complete. Called from push() on the calling thread.
    void drainOffline() noexcept {
        const uint32_t epoch = epoch_.load(std::memory_order_relaxed);
        for (;;) {
            const int64_t needed = (nextHop_ + 1) * cfg_.hopFrames + cfg_.lookaheadFrames;
            if (inPos_ < needed)
                return;
            if (!processHop(nextHop_, epoch))
                return;
            ++nextHop_;
        }
    }

    void workerMain() {
        runWorker();
        stage_->teardown(); // on this thread, see Stage::teardown
    }

    void runWorker() {
        MmcssScope mmcss;
        HighResTimer timer;
        if (!prepareStage())
            return;
        uint32_t localEpoch = epoch_.load(std::memory_order_acquire);
        // Loading can take seconds. Start at the first hop whose result is still due in the future: output
        // frame h * hop + latency must not be behind the audio thread yet. (Hops before it are dry, which is
        // what loading means; none of them is counted as a miss.)
        {
            const int64_t out = outPub_.load(std::memory_order_acquire);
            nextHopWorker_ = std::max<int64_t>(
                0, (out - static_cast<int64_t>(latency_) + cfg_.hopFrames - 1) / cfg_.hopFrames);
        }
        // The hop being output right now was dry because the model was still loading: not a miss.
        firstDueHop_.store(nextHopWorker_, std::memory_order_relaxed);
        state_.store(State::Running, std::memory_order_release);
        while (!stop_.load(std::memory_order_relaxed)) {
            const uint32_t e = epoch_.load(std::memory_order_acquire);
            if (e != localEpoch) {
                localEpoch = e;
                nextHopWorker_ = 0;
            }
            const int64_t in = inCount_.load(std::memory_order_acquire);
            const int64_t needed = (nextHopWorker_ + 1) * cfg_.hopFrames + cfg_.lookaheadFrames;
            if (in < needed) {
                const int64_t w0 = nowMicros();
                timer.wait(1000);
                const int64_t late = nowMicros() - w0 - 1000;
                if (late > 0 && late > static_cast<int64_t>(wakeLateMax_.load(std::memory_order_relaxed)))
                    wakeLateMax_.store(static_cast<uint32_t>(late), std::memory_order_relaxed);
                continue;
            }
            // A hop whose frames were all output already (a late worker) is dropped, not computed.
            const int64_t due = (nextHopWorker_ + 1) * static_cast<int64_t>(cfg_.hopFrames) + latency_;
            if (due <= outPub_.load(std::memory_order_relaxed)) {
                ++nextHopWorker_;
                continue;
            }
            if (!processHop(nextHopWorker_, localEpoch))
                return;
            ++nextHopWorker_;
        }
    }

    PipelineConfig cfg_;
    CaptureLock lock_;
    std::unique_ptr<Stage> stage_;
    bool offline_ = false;
    bool running_ = false;
    uint32_t latency_ = 0, window_ = 0, nSlots_ = 0;
    uint64_t inMask_ = 0;
    std::vector<float> inRing_, windowBuf_, outBuf_, wetScratch_;
    std::unique_ptr<Slot[]> slots_;
    float fadeStep_ = 0.01f;

    // Audio-thread state.
    int64_t inPos_ = 0, outPos_ = 0, lastMissHop_ = -1;
    float fade_ = 1.0f, lastWet_ = 0.0f;
    int64_t nextHop_ = 0; // offline only (audio thread)

    // Shared.
    std::atomic<int64_t> inCount_{0}, outPub_{0}, firstDueHop_{0};
    std::atomic<uint32_t> epoch_{0};
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> runs_{0}, misses_{0};
    std::atomic<uint32_t> consecutive_{0}, p50_{0}, p99_{0}, max_{0};
    std::atomic<uint64_t> lastMissFrame_{0};
    std::atomic<uint32_t> wakeLateMax_{0}, hopTotalMax_{0}, computeAllMax_{0};
    std::atomic<bool> graphed_{false};
    mutable std::mutex errorMutex_;
    std::string error_;

    // Worker-thread state.
    std::thread worker_;
    int64_t nextHopWorker_ = 0;
    std::vector<uint32_t> computeUs_, sortScratch_;
    size_t statCount_ = 0;
};

} // namespace dsptb::gpu
