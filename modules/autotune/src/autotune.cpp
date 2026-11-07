// Autotune (org.dsptb.autotune): detect what is being sung, decide which note it should be, and shift the voice by
// the difference. One Strength control sets how much and how fast.

#include "correction_smoother.hpp"
#include "key_timeline.hpp"
#include "pitch_graph.hpp"
#include "pitch_tracker.hpp"
#include "target_picker.hpp"

#include <dsptb/dsp_common/key.hpp>
#include <dsptb/dsp_common/pitch.hpp>
#include <dsptb/dsp_common/psola.hpp>
#include <dsptb/rt/spsc_ring.hpp>
#include <dsptb/sdk.hpp>
#include <dsptb/state_json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

DSPTB_DEFINE_RT_GUARD();

namespace dsptb::autotune {

namespace dsp = dsptb::dsp;

class Autotune final : public Processor<Autotune> {
    enum Param : uint32_t { kStrength, kKeySource, kKey, kScale, kRefA4, kNoteHold, kVoiceRange, kSilenceDb, kShifterMode };
    enum KeySource : int { kTrack, kManual };
    enum ShifterMode : int { kAuto, kLive, kQuality };

public:
    static constexpr const char* kKeySources[] = {"Track", "Manual"};
    static constexpr const char* kKeys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static constexpr const char* kScales[] = {"Major", "Minor", "Chromatic"};
    static constexpr const char* kVoiceRanges[] = {"Full", "Low", "Mid", "High"};
    static constexpr const char* kShifterModes[] = {"Auto", "Live", "Quality"};
    static constexpr ModuleInfo info{.id = "org.dsptb.autotune",
                                     .name = "Autotune",
                                     .version = "0.1.0",
                                     .vendor = "DSP Toolbox",
                                     .description = "Pitch-correct a singing voice, with one Strength control.",
                                     .caps = DSPTB_CAP_REALTIME | DSPTB_CAP_OFFLINE | DSPTB_CAP_HIDE_AUTO_PARAMS};
    static constexpr auto params = dsptb::params(
        Float("strength", "Strength", "%", 0.0f, 100.0f, 45.0f), Enum("key_source", "Key from", kKeySources, kTrack),
        Enum("key", "Key", kKeys, 0), Enum("scale", "Scale", kScales, 2),
        Float("ref_a4", "Reference A4", "Hz", 415.0f, 466.0f, 440.0f), Float("note_hold", "Note hold", "c", 0.0f, 50.0f, 30.0f),
        Enum("voice_range", "Voice range", kVoiceRanges, 0), Float("silence_db", "Ignore below", "dB", -70.0f, -30.0f, -50.0f),
        Enum("shifter_mode", "Shifter", kShifterModes, kAuto).hidden());

    ~Autotune() override { stopKeyJob(); }

    bool prepare(const PrepareInfo& p) override {
        stopKeyJob();
        sampleRate_ = p.sampleRate;
        const bool highRate = p.sampleRate > 60000.0;
        hop_ = highRate ? 512u : 256u;

        // A frame describes the sound about half its length ago, and a late correction makes a quick wobble deeper
        // instead of flatter. So shorter frames run alongside the long one, and a note high enough for one is
        // measured with it.
        const size_t longest = highRate ? 4096 : 2048;
        for (size_t k = 0; k < kDetectors; ++k) {
            dsp::PitchConfig pitch;
            pitch.frameSize = longest >> k;
            pitch.hop = hop_;
            // The longest period a frame can measure is half its length.
            pitch.minHz = k == 0 ? 80.0 : p.sampleRate / static_cast<double>(pitch.frameSize / 2) * 1.001;
            pitch.maxHz = 1000.0;
            yin_[k] = std::make_unique<dsp::YinPitch>(p.sampleRate, pitch);
            minHz_[k] = static_cast<float>(pitch.minHz);
        }

        // Latency must stay constant between prepares, so the mode is read only here.
        const int mode = host().valid() ? static_cast<int>(host().getParam(kShifterMode) + 0.5f) : kAuto;
        dsp::PsolaConfig psola;
        psola.minHz = 80.0;
        psola.maxHz = 1000.0;
        psola.maxBlock = p.maxBlockFrames;
        psola.mode = mode == kQuality || (mode == kAuto && p.offline) ? dsp::PsolaMode::Quality : dsp::PsolaMode::Live;
        shifter_ = std::make_unique<dsp::PsolaShifter>(p.sampleRate, psola);

        mono_.assign(p.maxBlockFrames, 0.0f);
        out_.assign(p.maxBlockFrames, 0.0f);
        smoother_.prepare(static_cast<float>(1000.0 * hop_ / p.sampleRate));
        resetState();

        // Live, the key is found on a thread of its own so the file can change at any time. A render must give
        // the same answer every time, so it finds the key before it starts.
        if (p.offline)
            detectKey(host().workingFileGeneration());
        else
            startKeyJob();
        return true;
    }

    uint32_t latencyFrames() const override { return shifter_ ? static_cast<uint32_t>(shifter_->latencyFrames()) : 0; }
    // Live mode's delay wanders by up to one cycle of the lowest note on top of its fixed part.
    uint32_t tailFrames() const override {
        return latencyFrames() + static_cast<uint32_t>(std::ceil(sampleRate_ / 80.0));
    }

    void reset() override { resetState(); }

    // Audio thread: no allocation, locks, I/O, logging or exceptions. Params via b.param(), the key via atomics, and
    // graph frames are dropped when the ring is full.
    void process(ProcessBlock& b) override {
        const uint32_t n = b.frames();
        const float* left = b.channel(0);
        const float* right = b.numChannels() > 1 ? b.channel(1) : left;
        for (uint32_t i = 0; i < n; ++i)
            mono_[i] = 0.5f * (left[i] + right[i]);

        if (timeline_.update()) // the key job finished: count from the new timeline
            ++timelineSeen_;
        blockPosition_ = b.transport().position_seconds;

        // Split at hop ends, so a new estimate takes effect at the sample it was made from.
        for (uint32_t done = 0; done < n;) {
            const uint32_t len = std::min(n - done, samplesUntilHop_);
            for (uint32_t i = 0; i < len; ++i)
                hopEnergy_ += static_cast<double>(mono_[done + i]) * mono_[done + i];
            samplesUntilHop_ -= len;
            const bool hopEnds = samplesUntilHop_ == 0;
            for (size_t k = 0; k < kDetectors; ++k)
                yin_[k]->push(mono_.data() + done, len, [&](const dsp::PitchResult& r) {
                    estimates_[k] = r;
                    fresh_[k] = true;
                });
            if (fresh_[0]) // every frame ends on a hop, so the long frame's result arrives with the others
                onEstimate(b, pickEstimate(), b.transport().sample_clock + done + len);
            if (hopEnds) {
                samplesUntilHop_ = hop_;
                hopEnergy_ = 0.0;
            }
            shifter_->process(mono_.data() + done, out_.data() + done, len, tracked_.voiced ? tracked_.f0Hz : 0.0f,
                              smoother_.ratio());
            done += len;
        }
        for (uint32_t c = 0; c < b.numChannels(); ++c)
            std::copy_n(out_.data(), n, b.channel(c));
    }

    std::string saveState() override {
        Json j;
        j["ui"]["advancedOpen"] = advancedOpen_;
        return dumpState(j);
    }

    bool loadState(std::string_view text) override {
        const Json j = parseState(text);
        if (j.contains("ui") && j["ui"].is_object())
            advancedOpen_ = j["ui"].value("advancedOpen", false);
        return true;
    }

    void draw(Canvas& c) override {
        PitchFrame frame;
        while (frames_.tryPop(frame)) {
            graph_.push(frame);
            latest_ = frame;
        }
        drawKeyRow(c);
        drawNoteReadout(c);
        graph_.draw(c, shownKey(), sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
        c.paramWidget(kStrength);
        c.label("Low = natural. High = robotic.");
        if (c.button(advancedOpen_ ? "v Advanced" : "> Advanced")) {
            advancedOpen_ = !advancedOpen_;
            host().markStateDirty("Show or hide Advanced");
        }
        if (advancedOpen_) {
            c.paramWidget(kRefA4);
            c.paramWidget(kNoteHold);
            c.paramWidget(kVoiceRange);
            c.paramWidget(kSilenceDb);
        }
    }

private:
    static constexpr float kRangeHz[4][2] = {{80.0f, 1000.0f}, {80.0f, 400.0f}, {130.0f, 700.0f}, {200.0f, 1000.0f}};

    void resetState() {
        for (size_t k = 0; k < kDetectors; ++k) {
            if (yin_[k])
                yin_[k]->reset();
            fresh_[k] = false;
        }
        if (shifter_)
            shifter_->reset();
        tracker_.reset();
        picker_.reset();
        smoother_.reset();
        tracked_ = {};
        samplesUntilHop_ = hop_;
        hopEnergy_ = 0.0;
    }

    DetectedKey detectedKey() const noexcept {
        const uint32_t packed = detected_.load(std::memory_order_acquire);
        DetectedKey d;
        d.valid = (packed & 1u) != 0;
        d.confident = (packed & 2u) != 0;
        d.root = static_cast<int>((packed >> 8) & 15u);
        d.scale = (packed & 4u) != 0 ? Scale::Minor : Scale::Major;
        return d;
    }

    // The shortest frame that can measure the note and agrees with the long one roughly; otherwise the long one.
    // "Roughly" is three semitones, so a quick note change still counts while a harmonic jump doesn't.
    dsp::PitchResult pickEstimate() noexcept {
        const dsp::PitchResult& longest = estimates_[0];
        const float reference = longest.voiced ? longest.hz : (tracked_.voiced ? tracked_.f0Hz : 0.0f);
        dsp::PitchResult chosen = longest;
        if (reference > 0.0f)
            for (size_t k = kDetectors - 1; k > 0; --k) {
                const dsp::PitchResult& r = estimates_[k];
                if (fresh_[k] && r.voiced && r.hz >= 1.25f * minHz_[k] &&
                    std::fabs(1200.0f * std::log2(r.hz / reference)) <= 300.0f) {
                    chosen = r;
                    break;
                }
            }
        for (size_t k = 0; k < kDetectors; ++k)
            fresh_[k] = false;
        return chosen;
    }

    void onEstimate(ProcessBlock& b, const dsp::PitchResult& result, uint64_t clock) noexcept {
        const int range = std::clamp(b.paramInt(kVoiceRange), 0, 3);
        PitchTrackerConfig config;
        config.minHz = kRangeHz[range][0];
        config.maxHz = kRangeHz[range][1];
        config.silenceDb = b.param(kSilenceDb);
        config.refA4 = b.param(kRefA4);
        const double power = hopEnergy_ / static_cast<double>(hop_);
        const float hopDb = power > 1e-20 ? static_cast<float>(10.0 * std::log10(power)) : -200.0f;
        tracked_ = tracker_.update(result.hz, result.voiced, hopDb, config);

        detected_.store(keyAt(blockPosition_ + static_cast<double>(clock - b.transport().sample_clock) / sampleRate_),
                        std::memory_order_relaxed);
        const Key key = effectiveKey(b.paramInt(kKeySource) == kManual, std::clamp(b.paramInt(kKey), 0, 11),
                                     static_cast<Scale>(std::clamp(b.paramInt(kScale), 0, 2)), detectedKey());
        const int target = picker_.pick(tracked_.midi, tracked_.voiced, key, b.param(kNoteHold));
        const float error = tracked_.voiced ? (static_cast<float>(target) - tracked_.midi) * 100.0f : 0.0f;
        const float cents = smoother_.update(tracked_.voiced, error, b.param(kStrength));

        PitchFrame frame;
        frame.clock = clock;
        frame.sungMidi = tracked_.midi;
        frame.correctedMidi = tracked_.midi + cents / 100.0f;
        frame.target = static_cast<int16_t>(target);
        frame.voiced = tracked_.voiced ? 1 : 0;
        frames_.tryPush(frame);
    }

    enum KeyStatus : int { kNoFile, kDetecting, kDone };
    static constexpr size_t kKeyChunk = 65536;
    // Songs change key, so the key is found for overlapping windows as well as for the whole file.
    static constexpr double kWindowSeconds = 10.0;
    static constexpr double kWindowHopSeconds = 5.0;
    static constexpr size_t kMaxWindows = 4096; // over five hours

    // One packed key per window: bit 0 valid, 1 confident, 2 minor, bits 8-11 the root. Fixed size, so handing it
    // to the audio thread never allocates.
    struct Timeline {
        uint32_t count = 0;
        uint16_t keys[kMaxWindows] = {};
    };

    static uint32_t pack(const DetectedKey& k) {
        return k.valid ? 1u | (k.confident ? 2u : 0u) | (k.scale == Scale::Minor ? 4u : 0u) |
                             (static_cast<uint32_t>(k.root) << 8)
                       : 0u;
    }

    // Audio thread: the key at a position in the file.
    uint32_t keyAt(double seconds) const noexcept {
        const Timeline& t = timeline_.read();
        if (t.count == 0)
            return 0;
        return t.keys[keyWindowAt(seconds, kWindowSeconds, kWindowHopSeconds, t.count)];
    }

    void publishTimeline(const std::vector<DetectedKey>& keys) {
        Timeline& slot = timeline_.writeSlot();
        slot.count = static_cast<uint32_t>(std::min(keys.size(), kMaxWindows));
        for (uint32_t i = 0; i < slot.count; ++i)
            slot.keys[i] = static_cast<uint16_t>(pack(keys[i]));
        timeline_.publish();
    }

    void startKeyJob() {
        stopKey_.store(false, std::memory_order_relaxed);
        keyThread_ = std::thread([this] {
            uint64_t seen = ~0ull;
            while (!stopKey_.load(std::memory_order_relaxed)) {
                // The ABI has no "file changed" callback, and draw() doesn't run while the panel is hidden, so poll.
                const uint64_t generation = host().workingFileGeneration();
                if (generation != seen && detectKey(generation))
                    seen = generation;
                std::unique_lock lock(wakeMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(250),
                               [this] { return stopKey_.load(std::memory_order_relaxed); });
            }
        });
    }

    void stopKeyJob() {
        if (!keyThread_.joinable())
            return;
        {
            std::lock_guard lock(wakeMutex_);
            stopKey_.store(true, std::memory_order_relaxed);
        }
        wake_.notify_all();
        keyThread_.join();
    }

    // Reads the whole working file. False when it stopped early, so the caller tries again.
    bool detectKey(uint64_t generation) {
        if (generation == 0) {
            publishTimeline({});
            keyChanges_.store(0, std::memory_order_relaxed);
            keyStatus_.store(kNoFile, std::memory_order_release);
            return true;
        }
        keyStatus_.store(kDetecting, std::memory_order_release);
        WorkingFile file(host());
        if (!file) {
            publishTimeline({});
            keyChanges_.store(0, std::memory_order_relaxed);
            keyStatus_.store(kNoFile, std::memory_order_release);
            return true;
        }
        const dsptb_file_view& v = file.view();
        const auto windowFrames = static_cast<uint64_t>(kWindowSeconds * v.sample_rate);
        const auto hopFrames = static_cast<uint64_t>(kWindowHopSeconds * v.sample_rate);
        const auto cancelled = [&] {
            return stopKey_.load(std::memory_order_relaxed) || host().workingFileGeneration() != v.generation;
        };
        // Mono of [from, to) through a fresh detector.
        std::vector<float> mono(kKeyChunk);
        const auto detect = [&](uint64_t from, uint64_t to, dsp::KeyResult& out) {
            dsp::KeyDetector detector(v.sample_rate);
            for (uint64_t at = from; at < to; at += kKeyChunk) {
                if (cancelled())
                    return false;
                const auto n = static_cast<size_t>(std::min<uint64_t>(kKeyChunk, to - at));
                for (size_t i = 0; i < n; ++i)
                    mono[i] = 0.5f * (file.channel(0)[at + i] + file.channel(1)[at + i]);
                detector.push(mono.data(), n);
            }
            out = detector.result();
            return true;
        };

        dsp::KeyResult whole;
        if (!detect(0, v.num_frames, whole))
            return false; // a new file arrived, or the module is going away
        if (whole.valid)
            logKey("Key detected", whole);

        std::vector<KeyWindow> windows;
        for (uint64_t start = 0; windows.size() < kMaxWindows; start += hopFrames) {
            const uint64_t end = std::min(v.num_frames, start + windowFrames);
            dsp::KeyResult r;
            if (!detect(start, end, r))
                return false;
            windows.push_back({r.valid, r.confident, r.root, r.mode == dsp::KeyMode::Minor ? Scale::Minor : Scale::Major});
            if (end == v.num_frames)
                break;
        }
        std::vector<DetectedKey> keys = resolveKeyTimeline(windows);
        // No confident window, but the whole file is: one key throughout (a short file is a single window anyway).
        if ((keys.empty() || !keys.front().valid) && whole.valid && whole.confident)
            keys.assign(std::max<size_t>(1, windows.size()),
                        {true, true, whole.root, whole.mode == dsp::KeyMode::Minor ? Scale::Minor : Scale::Major});
        int changes = 0;
        for (size_t i = 1; i < keys.size(); ++i)
            if (keys[i].valid && keys[i - 1].valid &&
                sameNotesId(keys[i].root, keys[i].scale) != sameNotesId(keys[i - 1].root, keys[i - 1].scale)) {
                ++changes;
                char line[96];
                const double at = (static_cast<double>(i) - 0.5) * kWindowHopSeconds + 0.5 * kWindowSeconds;
                std::snprintf(line, sizeof(line), "Key change at %.1f s: %s %s", at, kKeys[keys[i].root],
                              keys[i].scale == Scale::Minor ? "minor" : "major");
                host().log(DSPTB_LOG_INFO, line);
            }
        publishTimeline(keys);
        keyChanges_.store(changes, std::memory_order_relaxed);
        keyStatus_.store(kDone, std::memory_order_release);
        return true;
    }

    void logKey(const char* what, const dsp::KeyResult& r) const {
        char line[160];
        std::snprintf(line, sizeof(line), "%s: %s %s (%s, r=%.2f, margin=%.2f)", what, kKeys[r.root],
                      r.mode == dsp::KeyMode::Minor ? "minor" : "major", r.confident ? "confident" : "not confident",
                      static_cast<double>(r.correlation), static_cast<double>(r.margin));
        host().log(DSPTB_LOG_INFO, line);
    }

    Key shownKey() const {
        const Host& h = host();
        const bool manual = static_cast<int>(h.getParam(kKeySource) + 0.5f) == kManual;
        return effectiveKey(manual, static_cast<int>(h.getParam(kKey) + 0.5f),
                            static_cast<Scale>(static_cast<int>(h.getParam(kScale) + 0.5f)), detectedKey());
    }

    void drawKeyRow(Canvas& c) {
        const Host& h = host();
        c.paramWidget(kKeySource);
        const bool manual = static_cast<int>(h.getParam(kKeySource) + 0.5f) == kManual;
        const DetectedKey detected = detectedKey();
        int key = manual || !detected.valid ? static_cast<int>(h.getParam(kKey) + 0.5f) : detected.root;
        int scale = manual || !detected.valid ? static_cast<int>(h.getParam(kScale) + 0.5f) : static_cast<int>(detected.scale);
        const bool keyChanged = c.combo("Key", key, kKeys, 12);
        const bool scaleChanged = c.combo("Scale", scale, kScales, 3);
        if (keyChanged || scaleChanged) {
            // Choosing a key by hand means the detected one was wrong, so stop following it.
            h.setParam(kKey, static_cast<float>(key));
            h.setParam(kScale, static_cast<float>(scale));
            h.setParam(kKeySource, static_cast<float>(kManual));
        }
        if (!manual) {
            const int status = keyStatus_.load(std::memory_order_acquire);
            c.label(status == kDetecting     ? "Detecting key..."
                    : !detected.valid        ? "No track loaded: using Chromatic"
                    : !detected.confident ? "Not sure: using Chromatic"
                    : keyChanges_.load(std::memory_order_relaxed) > 0 ? "Detected: follows the key changes"
                                                                      : "Detected");
        } else if (detected.valid) {
            char text[48];
            std::snprintf(text, sizeof(text), "Use detected (%s %s)", kKeys[detected.root],
                          detected.scale == Scale::Minor ? "minor" : "major");
            if (c.button(text))
                h.setParam(kKeySource, static_cast<float>(kTrack));
        }
    }

    void drawNoteReadout(Canvas& c) const {
        if (!latest_.voiced || latest_.target < 0) {
            c.label("Note  -");
            return;
        }
        const int cents = static_cast<int>(std::lround((latest_.sungMidi - static_cast<float>(latest_.target)) * 100.0f));
        char text[48];
        std::snprintf(text, sizeof(text), "Note  %s%d  %+d c", kKeys[latest_.target % 12], latest_.target / 12 - 1, cents);
        c.label(text);
    }

    double sampleRate_ = 0.0;
    uint32_t hop_ = 256;
    static constexpr size_t kDetectors = 3; // full, half and quarter frame length
    std::unique_ptr<dsp::YinPitch> yin_[kDetectors];
    float minHz_[kDetectors] = {};
    dsp::PitchResult estimates_[kDetectors] = {};
    bool fresh_[kDetectors] = {};
    std::unique_ptr<dsp::PsolaShifter> shifter_;
    std::vector<float> mono_, out_;

    // Audio thread only.
    PitchTracker tracker_;
    TargetPicker picker_;
    CorrectionSmoother smoother_;
    TrackedPitch tracked_;
    uint32_t samplesUntilHop_ = 256;
    double hopEnergy_ = 0.0;

    // The key at the playback position, written by the audio thread for the panel.
    std::atomic<uint32_t> detected_{0};
    rt::SpscRing<PitchFrame> frames_{1024};

    // Key job thread.
    std::thread keyThread_;
    std::atomic<bool> stopKey_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<int> keyStatus_{kNoFile};
    std::atomic<int> keyChanges_{0};
    rt::TripleBuffer<Timeline> timeline_; // key thread writes, audio thread reads
    uint64_t timelineSeen_ = 0;
    double blockPosition_ = 0.0;

    // UI thread only.
    PitchGraph graph_;
    PitchFrame latest_;
    bool advancedOpen_ = false;
};

} // namespace dsptb::autotune

using dsptb::autotune::Autotune;

DSPTB_REGISTER_MODULES(Autotune);
