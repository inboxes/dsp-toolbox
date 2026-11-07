#include "dsptb/core/engine.hpp"

#include <dsptb/rt/rt_guard.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace dsptb {

namespace {

int64_t qpcNow() noexcept {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

float dbToLinear(float db) noexcept {
    return std::pow(10.0f, db / 20.0f);
}

} // namespace

Engine::Engine(EngineOptions options) : options_(options) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcToSeconds_ = 1.0 / static_cast<double>(f.QuadPart);
}

Engine::~Engine() {
    close();
}

// ----------------------------------------------------------------------------------------------------------
// UI-thread control
// ----------------------------------------------------------------------------------------------------------

bool Engine::open(const AudioConfig& config, std::string& error) {
    close();
    auto backend = createBackend(config.backend);
    if (!backend->open(config, this, error))
        return false;
    backend_ = std::move(backend);
    config_ = config;
    sampleRate_ = backend_->sampleRate();
    bufferFrames_ = backend_->bufferFrames();
    latencyFrames_ = backend_->latencyFrames();
    maxBlock_ = std::max<uint32_t>(1024, 2 * bufferFrames_);
    prepareState();
    if (onOpened_)
        onOpened_();
    return true;
}

void Engine::prepareState() {
    busL_.assign(maxBlock_, 0.0f);
    busR_.assign(maxBlock_, 0.0f);
    micL_.assign(maxBlock_, 0.0f);
    micR_.assign(maxBlock_, 0.0f);
    micGate_.prepare(static_cast<double>(sampleRate_));
    gateThresholdApplied_ = 1000.0f;
    gateWasEnabled_ = false;
    const double sr = sampleRate_;
    const bool micSelected = source() == SourceKind::Mic;
    fileGain_.reset(sr, options_.sourceFadeMs, micSelected ? 0.0f : 1.0f);
    micGain_.reset(sr, options_.sourceFadeMs, micSelected ? 1.0f : 0.0f);
    monitorGate_.reset(sr, options_.sourceFadeMs, (micSelected && !monitorMic()) ? 0.0f : 1.0f);
    master_.reset(sr, options_.masterRampMs, dbToLinear(masterDb_.load()));
    muteGate_.reset(sr, options_.muteRampMs, muted() ? 0.0f : 1.0f);
    chainFade_.reset(sr, options_.chainDipMs, 1.0f);
    sampleClock_ = 0;
    seekedFlag_ = false;
    wasMic_ = false;
    gateStep_ = options_.declickMs > 0.0 ? static_cast<float>(1.0 / (options_.declickMs * 0.001 * sr)) : 1.0f;
    gate_ = 0.0f;
    // Peak meter falls about 20 dB per 0.3 s: per-chunk decay is computed in processChunk from the chunk
    // size.
    peakDecay_ = static_cast<float>(1.0 / (0.3 * sr));
    dspEma_ = 0.0f;
}

bool Engine::start(std::string& error) {
    if (!backend_) {
        error = "audio stream is not open";
        return false;
    }
    if (running_)
        return true;
    if (!backend_->start(error))
        return false;
    running_ = true;
    return true;
}

void Engine::stop() {
    if (backend_ && running_)
        backend_->stop();
    running_ = false;
}

void Engine::close() {
    stop();
    if (backend_) {
        backend_->close();
        backend_.reset();
    }
    sampleRate_ = bufferFrames_ = latencyFrames_ = 0;
}

void Engine::setFile(std::shared_ptr<const LoadedFile> file) {
    playing_.store(false, std::memory_order_relaxed);
    position_.store(0, std::memory_order_relaxed);
    seekRequest_.store(-1, std::memory_order_relaxed);
    lengthFrames_.store(file ? file->audio.frames() : 0, std::memory_order_relaxed);
    file_.publish(std::make_unique<FileHandle>(FileHandle{std::move(file)}));
}

void Engine::collectGarbage() {
    file_.collect();
    chain_.collect();
}

void Engine::play() {
    const uint64_t len = lengthFrames_.load(std::memory_order_relaxed);
    if (len == 0)
        return;
    if (position_.load(std::memory_order_relaxed) >= len) { // finished earlier: start over
        const bool loop = loopEnabled_.load(std::memory_order_relaxed);
        seekRequest_.store(loop ? static_cast<int64_t>(loopStart_.load(std::memory_order_relaxed)) : 0,
                           std::memory_order_release);
    }
    playing_.store(true, std::memory_order_relaxed);
}

void Engine::stopTransport() {
    playing_.store(false, std::memory_order_relaxed);
    const bool loop = loopEnabled_.load(std::memory_order_relaxed);
    seekRequest_.store(loop ? static_cast<int64_t>(loopStart_.load(std::memory_order_relaxed)) : 0,
                       std::memory_order_release);
}

void Engine::setLoop(bool enabled, uint64_t startFrame, uint64_t endFrame) {
    loopStart_.store(startFrame, std::memory_order_relaxed);
    loopEnd_.store(endFrame, std::memory_order_relaxed);
    loopEnabled_.store(enabled, std::memory_order_relaxed);
}

void Engine::setMasterGainDb(float db) {
    masterDb_.store(std::clamp(db, -60.0f, 12.0f), std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------------------------------------
// Audio thread
// ----------------------------------------------------------------------------------------------------------

void Engine::pushEvent(EngineEventType type, uint32_t value, uint64_t extra) noexcept {
    if (!events_.tryPush(EngineEvent{type, value, extra}))
        eventsDropped_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::pushTap(TapRing& tap, const float* l, const float* r, uint32_t n) noexcept {
    constexpr uint32_t kSlice = 256;
    float tmp[kSlice * 2];
    for (uint32_t done = 0; done < n;) {
        const uint32_t m = std::min(kSlice, n - done);
        for (uint32_t i = 0; i < m; ++i) {
            tmp[2 * i] = l[done + i];
            tmp[2 * i + 1] = r[done + i];
        }
        // Whole blocks only, so the consumer never sees a split frame.
        if (tap.ring.freeSpace() >= static_cast<size_t>(m) * 2) {
            tap.ring.write(tmp, static_cast<size_t>(m) * 2);
            tap.framesWritten.fetch_add(m, std::memory_order_relaxed);
        } else {
            tap.droppedFrames.fetch_add(m, std::memory_order_relaxed);
        }
        done += m;
    }
}

void Engine::onAudio(const float* in, float* out, uint32_t frames) noexcept {
    rt::RtScope rtScope; // the debug allocation guard watches everything below
    const int64_t t0 = qpcNow();

    if (frames == 0)
        return;
    if (maxBlock_ == 0) {
        std::fill_n(out, static_cast<size_t>(frames) * 2, 0.0f);
        return;
    }
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = std::min(maxBlock_, frames - done);
        processChunk(in ? in + static_cast<size_t>(done) * 2 : nullptr, out + static_cast<size_t>(done) * 2,
                     n);
        done += n;
    }

    const double seconds = static_cast<double>(qpcNow() - t0) * qpcToSeconds_;
    const double period = static_cast<double>(frames) / static_cast<double>(sampleRate_);
    const float load = static_cast<float>(seconds / period);
    dspEma_ += 0.05f * (load - dspEma_); // EMA, alpha = 0.05
    dspLoad_.store(dspEma_, std::memory_order_relaxed);
    if (load > 1.0f) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
        pushEvent(EngineEventType::Underrun, static_cast<uint32_t>(load * 100.0f));
    }
    callbacks_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::processChunk(const float* in, float* out, uint32_t n) noexcept {
    // ---- block start: pick up published objects and control changes ----
    FileHandle* handle = file_.rtAcquire();
    const LoadedFile* file = handle ? handle->file.get() : nullptr;
    if (file != curFile_) { // a new file arrived: start from the top, silently
        curFile_ = file;
        pos_ = 0;
        gate_ = 0.0f;
        seekPending_ = false;
        atEnd_ = false;
        position_.store(0, std::memory_order_relaxed);
    }
    ChainSnapshot* chain = chain_.rtAcquire();

    const int64_t req = seekRequest_.exchange(-1, std::memory_order_acq_rel);
    if (req >= 0) {
        seekTarget_ = static_cast<uint64_t>(req);
        seekPending_ = true;
    }
    const bool playRequested = playing_.load(std::memory_order_relaxed);
    const uint64_t len = curFile_ ? curFile_->audio.frames() : 0;
    const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
    const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);
    const bool loopOn = loopEnabled_.load(std::memory_order_relaxed) && loopEnd > loopStart && loopEnd <= len;
    const float* fileL = curFile_ ? curFile_->audio.ch[0].data() : nullptr;
    const float* fileR = curFile_ ? curFile_->audio.ch[1].data() : nullptr;

    const bool micSelected = source_.load(std::memory_order_relaxed) == static_cast<int>(SourceKind::Mic);
    fileGain_.setTarget(micSelected ? 0.0f : 1.0f);
    micGain_.setTarget(micSelected ? 1.0f : 0.0f);
    monitorGate_.setTarget((micSelected && !monitor_.load(std::memory_order_relaxed)) ? 0.0f : 1.0f);
    master_.setTarget(dbToLinear(masterDb_.load(std::memory_order_relaxed)));
    muteGate_.setTarget(mute_.load(std::memory_order_relaxed) ? 0.0f : 1.0f);

    // ---- microphone: deinterleave, meter (pre-gate), noise gate ----
    if (in) {
        float pl = 0.0f, pr = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            micL_[i] = in[2 * i];
            micR_[i] = in[2 * i + 1];
            pl = std::max(pl, std::fabs(micL_[i]));
            pr = std::max(pr, std::fabs(micR_[i]));
        }
        const float micDecay = std::exp(-peakDecay_ * static_cast<float>(n));
        micPeakL_.store(std::max(pl, micPeakL_.load(std::memory_order_relaxed) * micDecay),
                        std::memory_order_relaxed);
        micPeakR_.store(std::max(pr, micPeakR_.load(std::memory_order_relaxed) * micDecay),
                        std::memory_order_relaxed);

        const bool gateOn = gateEnabled_.load(std::memory_order_relaxed);
        if (gateOn) {
            const float thr = gateThresholdDb_.load(std::memory_order_relaxed);
            if (thr != gateThresholdApplied_) {
                micGate_.setThresholdDb(thr);
                gateThresholdApplied_ = thr;
            }
            micGate_.process(micL_.data(), micR_.data(), n);
            gateOpen_.store(micGate_.isOpen(), std::memory_order_relaxed);
        } else {
            if (gateWasEnabled_)
                micGate_.reset();
            gateOpen_.store(true, std::memory_order_relaxed); // no gate: the mic is always "open"
        }
        gateWasEnabled_ = gateOn;
    }

    // ---- sources -> bus ----
    bool reachedEnd = false;
    bool didSeek = false, didWrap = false;
    const uint64_t chunkStartPos = pos_;
    if (micSelected && !wasMic_)
        micStartClock_ = sampleClock_;
    wasMic_ = micSelected;
    auto stepGate = [this](float target) noexcept {
        if (gate_ < target)
            gate_ = std::min(target, gate_ + gateStep_);
        else if (gate_ > target)
            gate_ = std::max(target, gate_ - gateStep_);
    };
    for (uint32_t i = 0; i < n; ++i) {
        // File transport: a gate ramps audio in and out so play, pause and seek do not click. A seek first
        // fades the gate to zero, jumps, then fades back in. With declickMs = 0 the gate is simply on/off.
        bool wantPlay = playRequested && curFile_ != nullptr && !atEnd_;
        stepGate((wantPlay && !seekPending_) ? 1.0f : 0.0f);
        if (seekPending_ && gate_ == 0.0f) {
            pos_ = std::min(seekTarget_, len);
            atEnd_ = false;
            seekPending_ = false;
            didSeek = true;
            wantPlay = playRequested && curFile_ != nullptr;
            stepGate(wantPlay ? 1.0f : 0.0f);
        }

        float fl = 0.0f, fr = 0.0f;
        if (curFile_ && gate_ > 0.0f && pos_ < len) {
            fl = fileL[pos_] * gate_;
            fr = fileR[pos_] * gate_;
            ++pos_;
            if (loopOn && pos_ >= loopEnd) {
                pos_ = loopStart; // sample-accurate wrap
                didWrap = true;
            } else if (pos_ >= len) {
                atEnd_ = true;
                reachedEnd = true;
            }
        }

        const float fg = fileGain_.next();
        const float mg = micGain_.next();
        float l = fl * fg;
        float r = fr * fg;
        if (mg != 0.0f && in) {
            l += micL_[i] * mg;
            r += micR_[i] * mg;
        }
        busL_[i] = l;
        busR_[i] = r;
    }
    position_.store(pos_, std::memory_order_relaxed);
    if (reachedEnd) {
        playing_.store(false, std::memory_order_relaxed);
        pushEvent(EngineEventType::EndOfFile);
    }

    // ---- PRE tap -> processor chain -> POST tap ----
    if (chain) {
        for (TapRing* tap : chain->preTaps)
            pushTap(*tap, busL_.data(), busR_.data(), n);

        dsptb_transport transport{};
        transport.struct_size = sizeof(transport);
        transport.source = micSelected ? DSPTB_SOURCE_MIC : DSPTB_SOURCE_FILE;
        transport.playing = (micSelected || playRequested) ? 1u : 0u;
        const double sr = static_cast<double>(sampleRate_);
        transport.position_seconds = micSelected ? static_cast<double>(sampleClock_ - micStartClock_) / sr
                                                 : static_cast<double>(chunkStartPos) / sr;
        transport.sample_clock = sampleClock_;
        transport.seeked = seekedFlag_ ? 1u : 0u;

        ChainFaults faults;
        if (didSeek)
            resetChain(*chain, faults); // a seek resets every processor
        processChain(*chain, busL_.data(), busR_.data(), n, transport, /*ramps=*/true, faults);
        for (uint32_t k = 0; k < faults.count; ++k)
            pushEvent(EngineEventType::ModuleFaulted, faults.ids[k],
                      static_cast<uint64_t>(faults.reasons[k]));

        // Reorder dip: fade the chain output out/in (only touches samples while a fade is in progress).
        chainFade_.setTarget(chain->outputMuted ? 0.0f : 1.0f);
        if (chainFade_.isSmoothing() || chainFade_.target() == 0.0f) {
            for (uint32_t i = 0; i < n; ++i) {
                const float g = chainFade_.next();
                busL_[i] *= g;
                busR_[i] *= g;
            }
        }
        for (TapRing* tap : chain->postTaps)
            pushTap(*tap, busL_.data(), busR_.data(), n);
    }
    seekedFlag_ = didSeek || didWrap;
    sampleClock_ += n;

    // ---- output safety stage: gains, NaN/Inf scrub, brickwall clip, meters ----
    float peakL = 0.0f, peakR = 0.0f;
    uint32_t scrubbed = 0;
    bool clipped = false;
    for (uint32_t i = 0; i < n; ++i) {
        const float g = master_.next() * muteGate_.next() * monitorGate_.next();
        float l = busL_[i] * g;
        float r = busR_[i] * g;
        if (!std::isfinite(l)) {
            l = 0.0f;
            ++scrubbed;
        }
        if (!std::isfinite(r)) {
            r = 0.0f;
            ++scrubbed;
        }
        if (l > 1.0f) {
            l = 1.0f;
            clipped = true;
        } else if (l < -1.0f) {
            l = -1.0f;
            clipped = true;
        }
        if (r > 1.0f) {
            r = 1.0f;
            clipped = true;
        } else if (r < -1.0f) {
            r = -1.0f;
            clipped = true;
        }
        out[2 * i] = l;
        out[2 * i + 1] = r;
        peakL = std::max(peakL, std::fabs(l));
        peakR = std::max(peakR, std::fabs(r));
    }

    const float decay = std::exp(-peakDecay_ * static_cast<float>(n));
    peakL_.store(std::max(peakL, peakL_.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
    peakR_.store(std::max(peakR, peakR_.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
    if (clipped) {
        clip_.store(true, std::memory_order_relaxed);
        if (!clipWasLatched_)
            pushEvent(EngineEventType::Clip); // one event per clip episode
    }
    clipWasLatched_ = clip_.load(std::memory_order_relaxed);
    if (scrubbed) {
        nonFinite_.fetch_add(1, std::memory_order_relaxed);
        pushEvent(EngineEventType::NonFinite, scrubbed);
    }
}

} // namespace dsptb
