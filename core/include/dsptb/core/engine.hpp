#pragma once

#include "audio_buffer.hpp"
#include "backend.hpp"
#include "chain.hpp"
#include "chain_processor.hpp"
#include "noise_gate.hpp"
#include "null_backend.hpp"

#include <dsptb/rt/rt_swap.hpp>
#include <dsptb/rt/smoothed_value.hpp>
#include <dsptb/rt/spsc_ring.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dsptb {

// A decoded working file plus its overview. Immutable once published to the engine.
struct LoadedFile {
    std::string path;
    AudioBuffer audio; // planar stereo at the engine's sample rate
    WaveformOverview overview;
    double seconds = 0.0;
};

enum class SourceKind : int { File = 0, Mic = 1 };

// Where an input goes. Exactly one input is routed to the chain; an Output route skips the chain and the taps
// and joins the signal after the POST tap.
enum class InputRoute : int { Off = 0, Chain = 1, Output = 2 };

// Audio thread -> UI notifications, 16-byte POD.
// ModuleFaulted: value = instance id, extra = FaultReason.
enum class EngineEventType : uint32_t { Underrun = 1, Clip, NonFinite, EndOfFile, TapOverrun, ModuleFaulted };
struct EngineEvent {
    EngineEventType type = EngineEventType::Underrun;
    uint32_t value = 0;
    uint64_t extra = 0;
};
static_assert(sizeof(EngineEvent) == 16, "EngineEvent must stay a 16-byte POD");

struct EngineOptions {
    double declickMs = 5.0;     // play/pause/seek gate ramp; 0 = instant (bit-exact tests)
    double sourceFadeMs = 20.0; // File <-> Mic crossfade
    double muteRampMs = 5.0;
    double masterRampMs = 10.0; // master gain smoothing
    double chainDipMs = 10.0;   // reorder dip: chain output fade
    double outputRouteRampMs = 10.0; // volume of an input routed to the output
};

// The real-time engine. Owns one backend. All `onAudio` work happens on the audio thread
// under an RtScope and never allocates, locks or logs. Everything else is called from the UI thread, except
// the atomic getters, which are safe from any thread.
class Engine final : public IAudioCallback {
public:
    explicit Engine(EngineOptions options = {});
    ~Engine() override;

    // ---- stream control (UI thread) ----
    bool open(const AudioConfig& config, std::string& error); // closes any previous stream first
    bool start(std::string& error);
    void stop();
    void close();
    bool isOpen() const { return backend_ != nullptr; }
    bool isRunning() const { return running_; }
    // The driver's own settings window (ASIO). False when the backend has none or no stream is open.
    bool openDriverControlPanel() { return backend_ && backend_->openControlPanel(); }
    const AudioConfig& config() const { return config_; }
    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    // Largest block the engine hands to the chain (processors are prepared with this).
    uint32_t maxBlockFrames() const { return maxBlock_; }
    // Called on the UI thread after a stream opened and the engine state is prepared, before it starts. The
    // SessionModel uses it to re-prepare its instances for the new sample rate and block size.
    void setOnOpened(std::function<void()> fn) { onOpened_ = std::move(fn); }
    uint32_t latencyFrames() const { return backend_ ? backend_->latencyFrames() : latencyFrames_; }
    double latencyMs() const { return sampleRate_ ? 1000.0 * latencyFrames() / sampleRate_ : 0.0; }
    bool deviceLost() const { return backend_ && backend_->deviceLost(); }
    std::string describeBackend() const { return backend_ ? backend_->describe() : std::string(); }
    uint32_t minPeriodFrames() const { return backend_ ? backend_->minPeriodFrames() : 0; }
    uint32_t cushionFrames() const { return backend_ ? backend_->cushionFrames() : 0; }
    bool captureActive() const { return isOpen() && config_.capture; }
    // Only non-null for BackendKind::Null; lets tests and the CLI drive and capture it.
    NullBackend* nullBackend() {
        return config_.backend == BackendKind::Null ? static_cast<NullBackend*>(backend_.get()) : nullptr;
    }

    // ---- file source and transport ----
    // Publishes a file (or none) to the audio thread. Stops playback and rewinds.
    void setFile(std::shared_ptr<const LoadedFile> file);
    bool hasFile() const { return lengthFrames_.load(std::memory_order_relaxed) > 0; }
    uint64_t lengthFrames() const { return lengthFrames_.load(std::memory_order_relaxed); }
    uint64_t positionFrames() const { return position_.load(std::memory_order_relaxed); }
    bool playing() const { return playing_.load(std::memory_order_relaxed); }
    void play();
    void pause() { playing_.store(false, std::memory_order_relaxed); }
    void stopTransport(); // pause and return to the start (or the loop start)
    void seekFrames(uint64_t frame) {
        seekRequest_.store(static_cast<int64_t>(frame), std::memory_order_release);
    }
    // The region is [startFrame, endFrame). Looping is ignored while the region is empty or past the file
    // end.
    void setLoop(bool enabled, uint64_t startFrame, uint64_t endFrame);
    bool loopEnabled() const { return loopEnabled_.load(std::memory_order_relaxed); }
    uint64_t loopStart() const { return loopStart_.load(std::memory_order_relaxed); }
    uint64_t loopEnd() const { return loopEnd_.load(std::memory_order_relaxed); }

    // ---- input routes ----
    // The source is the input that feeds the chain. Selecting Mic only has an effect when the stream was opened
    // with capture = true.
    void setSource(SourceKind source) { source_.store(static_cast<int>(source), std::memory_order_relaxed); }
    SourceKind source() const { return static_cast<SourceKind>(source_.load(std::memory_order_relaxed)); }
    void setMonitorMic(bool monitor) { monitor_.store(monitor, std::memory_order_relaxed); }
    bool monitorMic() const { return monitor_.load(std::memory_order_relaxed); }
    // The input that is not the source can also be heard, mixed in after the POST tap at its own volume. A file
    // routed this way keeps its normal transport. A mic routed this way still obeys monitoring.
    void setOutputRoute(SourceKind input, bool toOutput) {
        toOutput_[static_cast<int>(input)].store(toOutput, std::memory_order_relaxed);
    }
    void setOutputRouteGainDb(SourceKind input, float db) {
        outputDb_[static_cast<int>(input)].store(std::clamp(db, -60.0f, 6.0f), std::memory_order_relaxed);
    }
    float outputRouteGainDb(SourceKind input) const {
        return outputDb_[static_cast<int>(input)].load(std::memory_order_relaxed);
    }
    InputRoute route(SourceKind input) const {
        if (source() == input)
            return InputRoute::Chain;
        return toOutput_[static_cast<int>(input)].load(std::memory_order_relaxed) ? InputRoute::Output
                                                                                  : InputRoute::Off;
    }

    // ---- microphone noise gate and input meter ----
    // The gate acts on the microphone before it is mixed, so it affects the monitor output and the analyzers
    // alike.
    void setMicGate(bool enabled, float thresholdDb) {
        gateThresholdDb_.store(std::clamp(thresholdDb, -90.0f, -10.0f), std::memory_order_relaxed);
        gateEnabled_.store(enabled, std::memory_order_relaxed);
    }
    bool micGateEnabled() const { return gateEnabled_.load(std::memory_order_relaxed); }
    float micGateThresholdDb() const { return gateThresholdDb_.load(std::memory_order_relaxed); }
    bool micGateOpen() const { return gateOpen_.load(std::memory_order_relaxed); }
    // Microphone level BEFORE the gate (decaying linear peak), valid whether or not the mic is monitored.
    float micPeakLeft() const { return micPeakL_.load(std::memory_order_relaxed); }
    float micPeakRight() const { return micPeakR_.load(std::memory_order_relaxed); }

    // ---- output safety ----
    void setMasterGainDb(float db);
    float masterGainDb() const { return masterDb_.load(std::memory_order_relaxed); }
    void setMute(bool muted) { mute_.store(muted, std::memory_order_relaxed); }
    bool muted() const { return mute_.load(std::memory_order_relaxed); }

    // ---- meters and statistics ----
    float peakLeft() const { return peakL_.load(std::memory_order_relaxed); } // linear, decaying
    float peakRight() const { return peakR_.load(std::memory_order_relaxed); }
    bool clipLatched() const { return clip_.load(std::memory_order_relaxed); }
    void clearClip() { clip_.store(false, std::memory_order_relaxed); }
    float dspLoad() const {
        return dspLoad_.load(std::memory_order_relaxed);
    } // smoothed, 1.0 = whole block period
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    uint64_t nonFiniteBlocks() const { return nonFinite_.load(std::memory_order_relaxed); }
    uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }

    // ---- taps / chain snapshot ----
    void publishChain(std::unique_ptr<ChainSnapshot> snapshot) { chain_.publish(std::move(snapshot)); }
    // Frees snapshots and files the audio thread has retired. Call regularly from the UI thread.
    void collectGarbage();

    // Drains one event (UI thread). Returns false when the queue is empty.
    bool popEvent(EngineEvent& out) { return events_.tryPop(out); }

    // ---- IAudioCallback (audio thread) ----
    void onAudio(const float* in, float* out, uint32_t frames) noexcept override;

private:
    struct FileHandle {
        std::shared_ptr<const LoadedFile> file;
    };

    void processChunk(const float* in, float* out, uint32_t n) noexcept;
    void pushEvent(EngineEventType type, uint32_t value = 0, uint64_t extra = 0) noexcept;
    void pushTap(TapRing& tap, const float* l, const float* r, uint32_t n) noexcept;
    void prepareState();
    float fileOutputTarget(bool micSelected) const noexcept;
    float micOutputTarget(bool micSelected) const noexcept;

    EngineOptions options_;
    AudioConfig config_;
    std::unique_ptr<IAudioBackend> backend_;
    bool running_ = false;
    std::function<void()> onOpened_;
    uint32_t sampleRate_ = 0;
    uint32_t bufferFrames_ = 0;
    uint32_t latencyFrames_ = 0;
    uint32_t maxBlock_ = 0;

    // cross-thread
    rt::RtSwap<FileHandle> file_{16};
    rt::RtSwap<ChainSnapshot> chain_{16};
    rt::SpscRing<EngineEvent> events_{256};
    std::atomic<uint64_t> lengthFrames_{0};
    std::atomic<uint64_t> position_{0};
    std::atomic<bool> playing_{false};
    std::atomic<int64_t> seekRequest_{-1};
    std::atomic<bool> loopEnabled_{false};
    std::atomic<uint64_t> loopStart_{0};
    std::atomic<uint64_t> loopEnd_{0};
    std::atomic<int> source_{static_cast<int>(SourceKind::File)};
    std::atomic<bool> monitor_{false};
    std::atomic<bool> toOutput_[2] = {false, false};  // by SourceKind
    std::atomic<float> outputDb_[2] = {0.0f, 0.0f}; // by SourceKind
    std::atomic<float> masterDb_{0.0f};
    std::atomic<bool> mute_{false};
    std::atomic<bool> gateEnabled_{false};
    std::atomic<float> gateThresholdDb_{NoiseGate::kDefaultThresholdDb};
    std::atomic<bool> gateOpen_{false};
    std::atomic<float> micPeakL_{0.0f};
    std::atomic<float> micPeakR_{0.0f};
    std::atomic<float> peakL_{0.0f};
    std::atomic<float> peakR_{0.0f};
    std::atomic<bool> clip_{false};
    std::atomic<float> dspLoad_{0.0f};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> nonFinite_{0};
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<uint64_t> eventsDropped_{0};

    // audio-thread state
    const LoadedFile* curFile_ = nullptr;
    uint64_t pos_ = 0;
    float gate_ = 0.0f;
    float gateStep_ = 1.0f;
    bool seekPending_ = false;
    uint64_t seekTarget_ = 0;
    bool atEnd_ = false;
    rt::SmoothedValue fileGain_, micGain_, monitorGate_, master_, muteGate_, chainFade_;
    // One per input rather than one for "the other input", so swapping the routes fades both instead of
    // jumping from one signal to the other.
    rt::SmoothedValue fileOutputGain_, micOutputGain_;
    uint64_t sampleClock_ = 0;
    bool seekedFlag_ = false; // a seek or loop wrap happened in the previous chunk
    bool wasMic_ = false;
    uint64_t micStartClock_ = 0;
    std::vector<float> busL_, busR_;
    std::vector<float> micL_, micR_;
    std::vector<float> outputRouteL_, outputRouteR_;
    NoiseGate micGate_;
    float gateThresholdApplied_ = 1000.0f;
    bool gateWasEnabled_ = false;
    float peakDecay_ = 0.0f;
    double qpcToSeconds_ = 0.0;
    float dspEma_ = 0.0f;
    bool clipWasLatched_ = false;
};

} // namespace dsptb
