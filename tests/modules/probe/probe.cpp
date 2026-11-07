// Test fixtures: analyzers that record exactly what analyze() was given, and report it through dumpFrames().
// `test.probe` runs on the shared analysis thread; `test.probe-thread` asks for a thread of its own.
#include <dsptb/sdk.hpp>

#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

DSPTB_DEFINE_RT_GUARD();

class Probe : public dsptb::Analyzer<Probe> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.probe",
                                            .name = "Probe (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description = "Test fixture: records analyze() calls.",
                                            .caps = 0,
                                            .defaultTap = DSPTB_TAP_POST};
    static constexpr auto params = dsptb::noParams();

    void analyze(dsptb::AnalyzeBlock& b) override {
        ++calls_;
        frames_ += b.frames();
        dropped_ += b.droppedFrames();
        if (b.frames() > maxChunk_)
            maxChunk_ = b.frames();
        if (calls_ > 1 && b.firstFrameClock() != expectedClock_ + b.droppedFrames())
            ++clockJumps_; // the clock must continue where the previous chunk ended (plus anything dropped)
        expectedClock_ = b.firstFrameClock() + b.frames();
        lastTap_ = static_cast<int>(b.tap());
        threadId_ = std::hash<std::thread::id>{}(std::this_thread::get_id());
        for (uint32_t i = 0; i < b.frames(); ++i) {
            sumL_ += static_cast<double>(b.channel(0)[i]) * b.channel(0)[i];
            sumR_ += static_cast<double>(b.channel(1)[i]) * b.channel(1)[i];
        }
    }

    std::string dumpFrames() override {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"calls\":%llu,\"frames\":%llu,\"dropped\":%llu,\"maxChunk\":%u,\"clockJumps\":%llu,"
                      "\"tap\":%d,\"threadId\":%llu,\"sumSqL\":%.9g,\"sumSqR\":%.9g}",
                      static_cast<unsigned long long>(calls_), static_cast<unsigned long long>(frames_),
                      static_cast<unsigned long long>(dropped_), maxChunk_,
                      static_cast<unsigned long long>(clockJumps_), lastTap_,
                      static_cast<unsigned long long>(threadId_), sumL_, sumR_);
        return buf;
    }

private:
    // Written by the analysis thread only; dumpFrames() is called after that thread has been stopped or
    // while the test drives the analysis by hand (pumpOnce), so none of this needs to be atomic.
    uint64_t calls_ = 0, frames_ = 0, dropped_ = 0, clockJumps_ = 0, expectedClock_ = 0, threadId_ = 0;
    uint32_t maxChunk_ = 0;
    int lastTap_ = -1;
    double sumL_ = 0.0, sumR_ = 0.0;
};

class ProbeThread final : public Probe {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.probe-thread",
                                            .name = "Probe with own thread (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description =
                                                "Test fixture: records analyze() calls on its own thread.",
                                            .caps = DSPTB_CAP_OWN_THREAD,
                                            .defaultTap = DSPTB_TAP_POST};
};

// Counts reset() calls, so a test can see whether the host reset the chain.
class ResetCounter final : public dsptb::Processor<ResetCounter> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.reset-counter",
                                            .name = "Reset counter (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description = "Test fixture: counts reset() calls.",
                                            .caps = DSPTB_CAP_REALTIME};
    static constexpr auto params = dsptb::noParams();

    bool prepare(const dsptb::PrepareInfo&) override { return true; }
    void process(dsptb::ProcessBlock&) override {}
    void reset() override { resets_.fetch_add(1, std::memory_order_relaxed); }
    std::string dumpFrames() override {
        return "{\"resets\":" + std::to_string(resets_.load(std::memory_order_relaxed)) + "}";
    }

private:
    std::atomic<uint64_t> resets_{0};
};

DSPTB_REGISTER_MODULES(Probe, ProbeThread, ResetCounter);
