// 3D Spatializer (org.dsptb.spatializer).
//
// Up to 8 sources are placed around the listener and rendered binaurally with Steam Audio (HRTF), for
// headphones. The motion maths (orbits, keyframe paths) is dsptb::spatial_motion; this file owns the scene
// model, the clocks, the state JSON, the audio path and the hand-over to the audio thread.
//
// Threads:
//   UI thread     load_state / save_state / draw: owns `scene_`, publishes immutable SceneSnapshots
//   audio thread  process: re-blocks to 256 frames (FifoAdapter), evaluates each source's position, runs the
//                 Steam Audio direct and binaural effects, mixes. Allocates nothing: every Steam Audio object
//                 and buffer is created in prepare().

#include "halfband.hpp"
#include "hrtf_bundle.hpp"
#include "scene.hpp"
#include "ui.hpp"
#include "user_dirs.hpp"

#include <phonon.h>

#include <dsptb/dsp/db.hpp>
#include <dsptb/dsp/fifo_adapter.hpp>
#include <dsptb/rt/rt_swap.hpp>
#include <dsptb/rt/smoothed_value.hpp>
#include <dsptb/rt/triple_buffer.hpp>
#include <dsptb/sdk.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <utility>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

DSPTB_DEFINE_RT_GUARD();

namespace {

using namespace dsptb::spat;

constexpr uint32_t kFrame = 256; // internal block size, at the engine rate (spec: Processing 1)
constexpr uint32_t kTailFrames = 512;

enum Param : uint32_t {
    kOutputGain,
    kDistanceAtten,
    kMinDistance,
    kAirAbsorption,
    kHrtfInterp,
    kSpatialBlend,
    kAirAmount,
    kCloseIld
};
constexpr const char* kInterpLabels[] = {"Nearest", "Bilinear"};

} // namespace

// Close-range ear difference. The packaged far-distance sets (the built-in one, SADIE at 1.2 m) have the
// level difference between the ears of a source at their own distance, which is weak for a source a few
// decimetres from the head. Real heads show a larger difference there: the far ear is shadowed, mostly at
// high frequencies (Spagnol et al., EUSIPCO 2012 style proximity correction). This applies, per source and
// per ear, a gain on the lows, a gain on the highs (split at kIldSplitHz) with the far ear down and the near
// ear slightly up, growing with proximity and with how far to the side the source is. Near-field sets are
// measured at close range already and are left alone.
constexpr float kIldSplitHz = 1500.0f;
constexpr float kIldMaxDb = 16.0f; // far-ear highs at full proximity, fully to the side, amount 1

inline bool usesCloseIld(const std::string& hrtfId) {
    return hrtfId == "builtin" || hrtfId == "mid-sadie-d1";
}

struct IldTarget {
    float lo[2]{1.0f, 1.0f}, hi[2]{1.0f, 1.0f}; // linear gains per ear (0 = left), below / above the split
};

inline IldTarget closeIldTarget(const sm::Vec3& pos, float dist, float refDist, float amount) {
    IldTarget t;
    constexpr float kClosest = 0.2f; // full effect at the 20 cm shell's distance
    const float prox =
        std::fmin(1.0f, std::fmax(0.0f, std::log(refDist / dist) / std::log(refDist / kClosest)));
    const float lat = dist > 1e-6f ? pos.x / dist : 0.0f; // +1 = right ear side
    const float q = prox * std::fabs(lat) * amount;
    const int far = lat > 0.0f ? 0 : 1;
    const auto lin = [](float db) { return std::pow(10.0f, db / 20.0f); };
    t.hi[far] = lin(-kIldMaxDb * q);
    t.lo[far] = lin(-0.4f * kIldMaxDb * q);
    t.hi[1 - far] = t.lo[1 - far] = lin(0.3f * kIldMaxDb * q);
    return t;
}

// A one-pole split with the gains ramped per sample across the block (no zipper noise); state per source.
struct IldFilter {
    float lp[2]{0.0f, 0.0f};
    IldTarget cur;
    bool primed = false;

    void bypass() { primed = false; }

    void apply(float* l, float* r, uint32_t n, const IldTarget& target, double rate) {
        if (!primed) { // coming from bypass: start at the target, no sweep
            cur = target;
            lp[0] = lp[1] = 0.0f;
            primed = true;
        }
        const float a = 1.0f - std::exp(-6.2831853f * kIldSplitHz / static_cast<float>(rate));
        float* ch[2] = {l, r};
        const float inv = 1.0f / static_cast<float>(n);
        for (int e = 0; e < 2; ++e) {
            const float dLo = (target.lo[e] - cur.lo[e]) * inv, dHi = (target.hi[e] - cur.hi[e]) * inv;
            float gLo = cur.lo[e], gHi = cur.hi[e], z = lp[e];
            for (uint32_t i = 0; i < n; ++i) {
                z += a * (ch[e][i] - z);
                ch[e][i] = gLo * z + gHi * (ch[e][i] - z);
                gLo += dLo;
                gHi += dHi;
            }
            lp[e] = z;
            cur.lo[e] = target.lo[e];
            cur.hi[e] = target.hi[e];
        }
    }
};

class Spatializer final : public dsptb::Processor<Spatializer>, private IUiHost {
public:
    static constexpr dsptb::ModuleInfo info{.id = "org.dsptb.spatializer",
                                            .name = "3D Spatializer",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox",
                                            .description = "Places up to 8 sources in 3D around the listener "
                                                           "(binaural HRTF, for headphones).",
                                            .caps = DSPTB_CAP_REALTIME | DSPTB_CAP_OFFLINE};
    static constexpr auto params =
        dsptb::params(dsptb::Float("output_gain_db", "Output gain", "dB", -60.f, 12.f, 0.f).smoothing(20.f),
                      dsptb::Bool("distance_atten", "Distance attenuation", true),
                      dsptb::Float("min_distance_m", "Min distance", "m", 0.01f, 10.f, 1.f),
                      dsptb::Bool("air_absorption", "Air absorption", true),
                      dsptb::Enum("hrtf_interp", "HRTF interpolation", kInterpLabels, 1),
                      dsptb::Float("spatial_blend", "Spatial blend", "", 0.f, 1.f, 1.f),
                      dsptb::Float("air_amount", "Air absorption amount", "x", 0.1f, 50.f, 5.f),
                      dsptb::Float("close_ild", "Close-range ear difference", "x", 0.f, 2.f, 1.f));

    Spatializer() : scene_(defaultScene()) { publishScene(); }

    ~Spatializer() override { destroyEffects(); }

    // ------------------------------------------------------------------------------------------------
    // lifecycle
    // ------------------------------------------------------------------------------------------------
    bool prepare(const dsptb::PrepareInfo& p) override {
        destroyEffects();
        hostRate_ = p.sampleRate;
        // Steam Audio's default HRTF exists for 24, 44.1 and 48 kHz only. Hosts at a multiple of 44.1 or 48
        // kHz run it at the base rate and convert by whole octaves (halfband filters, a few ms of extra
        // latency).
        factor_ = 0;
        for (uint32_t f : {1u, 2u, 4u}) {
            const double r = p.sampleRate / f;
            if (std::fabs(r - 44100.0) < 1.0 || std::fabs(r - 48000.0) < 1.0)
                factor_ = f;
        }
        if (factor_ == 0)
            return fail("The Spatializer supports sample rates of 44.1, 48, 88.2, 96, 176.4 and 192 kHz");
        sampleRate_ = p.sampleRate / factor_; // the rate Steam Audio runs at

        ctx_ = sharedContext(); // one context per process (see hrtf_bundle.hpp)
        if (!ctx_)
            return fail("Steam Audio could not create its context");

        audio_.samplingRate = static_cast<IPLint32>(std::lround(sampleRate_));
        audio_.frameSize = static_cast<IPLint32>(kFrame);

        IPLDirectEffectSettings ds{};
        ds.numChannels = 1;
        for (int i = 0; i < kMaxSources; ++i) {
            if (iplDirectEffectCreate(ctx_, &audio_, &ds, &direct_[static_cast<size_t>(i)]) !=
                IPL_STATUS_SUCCESS)
                return fail("Steam Audio could not create a source effect");
        }
        if (iplAudioBufferAllocate(ctx_, 1, kFrame, &monoIn_) != IPL_STATUS_SUCCESS ||
            iplAudioBufferAllocate(ctx_, 1, kFrame, &monoOut_) != IPL_STATUS_SUCCESS ||
            iplAudioBufferAllocate(ctx_, 2, kFrame, &stereo_) != IPL_STATUS_SUCCESS)
            return fail("Steam Audio could not allocate its audio buffers");
        // The first HRTF is built here, synchronously: a failing chosen set falls back to the built-in one.
        scanSets();
        if (!loadHrtfNow(effectiveHrtf()) && !loadHrtfNow("builtin"))
            return fail("Steam Audio could not create an HRTF");
        buffersReady_ = true;

        for (auto& g : gain_)
            g.reset(sampleRate_, 20.0, 0.0f);
        outGain_.reset(sampleRate_, 20.0, 1.0f);
        outGainInit_ = false;
        wasActive_.fill(false);
        ild_.fill(IldFilter{});
        buildConverters();
        fifo_ = std::make_unique<dsptb::dsp::FifoAdapter>(kFrame * factor_);
        publishScene();
        return true;
    }

    void reset() override {
        if (fifo_)
            fifo_->reset();
        for (auto& d : down_)
            d.reset();
        for (auto& u : up_)
            u.reset();
        for (size_t i = 0; i < kMaxSources; ++i) {
            if (const HrtfBundle* hb = hrtfSwap_.rtCurrent())
                if (hb->fx[i])
                    iplBinauralEffectReset(hb->fx[i]);
            if (direct_[i])
                iplDirectEffectReset(direct_[i]);
        }
        wasActive_.fill(false);
    }

    // The re-blocking delay (256 frames at 48 kHz) plus, above 48 kHz, the converters' filters, in host
    // frames. The HRTF's own impulse response starts about 64 frames in (the head delay) and is not counted:
    // it is part of the sound, like the interaural time difference.
    uint32_t latencyFrames() const override { return latency_; }
    uint32_t tailFrames() const override { return kTailFrames * std::max<uint32_t>(1, factor_); }

    // ------------------------------------------------------------------------------------------------
    // audio thread
    // ------------------------------------------------------------------------------------------------
    void process(dsptb::ProcessBlock& b) override {
        const uint32_t n = b.frames();
        if (!buffersReady_ || n == 0)
            return; // unprepared: the block passes through unchanged
        const auto& t = b.transport();
        positionSeconds_ = t.position_seconds;
        useTransport_ = t.source != DSPTB_SOURCE_MIC;
        for (uint32_t i = 0; i < p_.size(); ++i)
            p_[i] = b.param(i);
        float* io[2] = {b.channel(0), b.channel(1)};
        fifo_->process(io, n, [this](const float* const* in, float* const* out, int64_t offset) {
            renderHostBlock(in, out, offset);
        });
        streamFrames_ += n;
    }

    // ------------------------------------------------------------------------------------------------
    // UI thread
    // ------------------------------------------------------------------------------------------------
    void draw(dsptb::Canvas& c) override {
        liveBuf_.update();
        pollHrtf();
        ui_.draw(c, scene_, liveBuf_.read(), *this, sceneVersion_);
    }

    std::string saveState() override { return dsptb::dumpState(toJson(scene_)); }

    bool loadState(std::string_view text) override {
        scene_ = sceneFromJson(dsptb::parseState(text));
        ++sceneVersion_;
        publishScene();
        if (ctx_ && !hrtfWorkerBusy_.load()) // a session or an undo step: switch the set now, not on a worker
            if (effectiveHrtf() != hrtfLoaded_)
                loadHrtfNow(effectiveHrtf());
        return true;
    }

private:
    // ---- IUiHost ----
    void sceneChanged() override { publishScene(); }
    void applyRangePreset(Range r) override {
        host().setParam(kMinDistance, rangeRefDistanceM(r));
        host().setParam(kAirAmount, rangeAirAmount(r));
    }
    const std::vector<HrtfSetInfo>& hrtfSets() const override { return setInfos_; }
    std::string hrtfStatus() const override { return hrtfStatus_; }
    ElevationBand elevationBand() const override { return elevationBandOf(effectiveHrtf()); }
    std::string hrtfFolder() const override { return folder_; }
    void editFinished(const char* undoLabel) override { host().markStateDirty(undoLabel); }
    void freeStart(int s) override {
        freeRunning_[static_cast<size_t>(s)].store(true, std::memory_order_relaxed);
        freeRequest_[static_cast<size_t>(s)].fetch_add(1, std::memory_order_release);
    }
    void freeStop(int s) override {
        freeRunning_[static_cast<size_t>(s)].store(false, std::memory_order_relaxed);
    }
    bool freeRunning(int s) const override {
        return freeRunning_[static_cast<size_t>(s)].load(std::memory_order_relaxed);
    }

    void publishScene() { swap_.publish(std::make_unique<SceneSnapshot>(makeSnapshot(scene_))); }

    bool fail(const char* what) {
        host().log(DSPTB_LOG_ERROR, what);
        destroyEffects();
        return false;
    }

    void destroyEffects() {
        buffersReady_ = false;
        joinHrtfWorker();
        if (ctx_) {
            for (IPLAudioBuffer* buf : {&monoIn_, &monoOut_, &stereo_})
                if (buf->data)
                    iplAudioBufferFree(ctx_, buf);
        }
        monoIn_ = monoOut_ = stereo_ = IPLAudioBuffer{};
        for (size_t i = 0; i < kMaxSources; ++i)
            if (direct_[i])
                iplDirectEffectRelease(&direct_[i]);
        ctx_ = nullptr; // the context is shared and lives as long as the process
    }

    // Seconds on the source's free clock at internal-block time `frame` (stream frames), 0 when stopped.
    double freeSeconds(size_t s, int64_t frame) const {
        if (!freeRunning_[s].load(std::memory_order_relaxed))
            return 0.0;
        return std::max(0.0, static_cast<double>(frame - freeStart_[s]) / hostRate_);
    }

    sm::Vec3 positionOf(size_t s, const RtSource& rs, double transportT, double streamT, int64_t frame) {
        switch (rs.motion) {
        case Motion::Static:
            return rs.position;
        case Motion::Orbit:
            return sm::orbitPosition(rs.orbit, useTransport_ ? transportT : streamT);
        case Motion::Path: {
            ClockSel clock = rs.clock;
            if (clock == ClockSel::Auto)
                clock = useTransport_ ? ClockSel::Transport : ClockSel::Free;
            const double t = clock == ClockSel::Transport ? transportT : freeSeconds(s, frame);
            return rs.path.positionAt(t - static_cast<double>(rs.startOffsetS));
        }
        }
        return rs.position;
    }

    void buildConverters() {
        (void)Down2::fir(); // builds the shared FIR table here, so the audio thread never runs its static init
        down_.clear();
        up_.clear();
        for (auto& pair : scratch_)
            for (auto& v : pair)
                v.assign(kFrame * factor_, 0.0f);
        // Stage s runs at host/2^s: it takes kFrame*factor/2^s frames and hands on half as many.
        uint32_t n = kFrame * factor_;
        latency_ = kFrame * factor_;
        uint32_t stageDelayHost = 2 * (HalfbandFir::kHalf);
        for (uint32_t f = factor_; f > 1; f /= 2) {
            down_.emplace_back(n);
            down_.emplace_back(n); // left and right
            up_.emplace_back(n / 2);
            up_.emplace_back(n / 2);
            latency_ += stageDelayHost;
            n /= 2;
            stageDelayHost *= 2;
        }
    }

    // One block of kFrame * factor host frames: convert down, render, convert up.
    void renderHostBlock(const float* const* in, float* const* out, int64_t offset) noexcept {
        if (factor_ == 1) {
            renderBlock(in, out, offset);
            return;
        }
        // Left channel uses down_[2s], right 2s+1 (same for up_). Ping-pong between two scratch buffers.
        const size_t stages = down_.size() / 2;
        const float* src[2] = {in[0], in[1]};
        uint32_t n = kFrame * factor_;
        for (size_t s = 0; s < stages; ++s) {
            for (size_t ch = 0; ch < 2; ++ch) {
                float* dst = scratch_[(s & 1)][ch].data();
                down_[2 * s + ch].process(src[ch], dst);
                src[ch] = dst;
            }
            n /= 2;
        }
        float* coreIn[2] = {coreIn_[0].data(), coreIn_[1].data()};
        for (size_t ch = 0; ch < 2; ++ch)
            std::copy_n(src[ch], kFrame, coreIn[ch]);
        float* coreOut[2] = {coreOut_[0].data(), coreOut_[1].data()};
        renderBlock(coreIn, coreOut, offset);
        const float* up[2] = {coreOut[0], coreOut[1]};
        for (size_t s = stages; s-- > 0;) {
            for (size_t ch = 0; ch < 2; ++ch) {
                float* dst = s == 0 ? out[ch] : scratch_[(s & 1)][ch].data();
                up_[2 * s + ch].process(up[ch], dst);
                up[ch] = dst;
            }
        }
    }

    // One internal block of kFrame frames. `offset` is where its first input frame sits relative to the
    // first frame of the current host call (negative when it started in an earlier call).
    void renderBlock(const float* const* in, float* const* out, int64_t offset) noexcept {
        SceneSnapshot* snap = swap_.rtAcquire();
        HrtfBundle* hb = hrtfSwap_.rtAcquire();
        std::fill_n(accL_.data(), kFrame, 0.0f);
        std::fill_n(accR_.data(), kFrame, 0.0f);
        if (snap == nullptr || hb == nullptr) {
            std::fill_n(out[0], kFrame, 0.0f);
            std::fill_n(out[1], kFrame, 0.0f);
            return;
        }
        const double invSr = 1.0 / hostRate_; // positions and clocks are in host time
        const int64_t frame = static_cast<int64_t>(streamFrames_) + offset;
        const double transportT = positionSeconds_ + static_cast<double>(offset) * invSr;
        const double streamT = static_cast<double>(frame) * invSr;

        const bool atten = p_[kDistanceAtten] >= 0.5f;
        const bool air = p_[kAirAbsorption] >= 0.5f;
        const float minDist = p_[kMinDistance];
        const IPLHRTFInterpolation interp =
            p_[kHrtfInterp] >= 0.5f ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
        const float blend = std::min(1.0f, std::max(0.0f, p_[kSpatialBlend]));

        LivePositions& live = liveBuf_.writeSlot();
        live.count = snap->count;

        for (size_t s = 0; s < kMaxSources; ++s) {
            if (static_cast<int>(s) >= snap->count) {
                wasActive_[s] = false;
                continue;
            }
            const RtSource& rs = snap->src[s];
            if (freeRequest_[s].load(std::memory_order_acquire) != seenRequest_[s]) {
                seenRequest_[s] = freeRequest_[s].load(std::memory_order_relaxed);
                freeStart_[s] = frame; // the play button restarts the free clock
            }

            // Position: evaluated at the start of the block, clamped away from the head (HRTF
            // singularity).
            sm::Vec3 pos = positionOf(s, rs, transportT, streamT, frame);
            float dist = std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
            if (dist < 1e-6f) {
                pos = {0.0f, 0.0f, -kMinDistanceM};
                dist = kMinDistanceM;
            } else if (dist < kMinDistanceM) {
                const float k = kMinDistanceM / dist;
                pos = {pos.x * k, pos.y * k, pos.z * k};
                dist = kMinDistanceM;
            }
            pos = limitElevation(pos, elevationBandOf(hb->id)); // keeps the distance
            live.pos[s] = pos;

            const bool audible = !rs.mute && (!snap->anySolo || snap->solo[s]);
            const float targetGain = audible ? dsptb::dbToGain(rs.gainDb) : 0.0f;
            if (!wasActive_[s]) { // a slot that has just been switched on starts clean and at its gain
                iplBinauralEffectReset(hb->fx[s]);
                iplDirectEffectReset(direct_[s]);
                gain_[s].reset(sampleRate_, 20.0, targetGain);
                wasActive_[s] = true;
            }
            gain_[s].setTarget(targetGain);
            if (targetGain == 0.0f && gain_[s].current() == 0.0f)
                continue; // muted and faded out: nothing to render

            // Mono input with the source's gain.
            float* mono = monoIn_.data[0];
            for (uint32_t i = 0; i < kFrame; ++i) {
                const float l = in[0][i], r = in[1][i];
                const float x = rs.input == InputSel::Left    ? l
                                : rs.input == InputSel::Right ? r
                                                              : 0.5f * (l + r);
                mono[i] = x * gain_[s].next();
            }

            IPLAudioBuffer* binauralIn = &monoIn_;
            if (atten || air) {
                const IPLVector3 src{pos.x, pos.y, pos.z};
                const IPLVector3 listener{0.0f, 0.0f, 0.0f};
                IPLDirectEffectParams dp{};
                dp.transmissionType = IPL_TRANSMISSIONTYPE_FREQINDEPENDENT;
                dp.distanceAttenuation = 1.0f;
                dp.airAbsorption[0] = dp.airAbsorption[1] = dp.airAbsorption[2] = 1.0f;
                dp.directivity = 1.0f;
                dp.occlusion = 1.0f;
                dp.transmission[0] = dp.transmission[1] = dp.transmission[2] = 1.0f;
                int flags = 0;
                if (atten) {
                    IPLDistanceAttenuationModel dm{};
                    dm.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
                    dm.minDistance = minDist;
                    dp.distanceAttenuation = iplDistanceAttenuationCalculate(ctx_, src, listener, &dm);
                    flags |= IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
                }
                if (air) {
                    IPLAirAbsorptionModel am{};
                    // Steam Audio's default coefficients (0.0002, 0.0017, 0.0182 per metre at low, mid
                    // and high frequencies), scaled: real air does next to nothing within a few metres,
                    // so the amount makes the darkening audible at room and close range.
                    am.type = IPL_AIRABSORPTIONTYPE_EXPONENTIAL;
                    const float amount = std::min(50.0f, std::max(0.1f, p_[kAirAmount]));
                    am.coefficients[0] = 0.0002f * amount;
                    am.coefficients[1] = 0.0017f * amount;
                    am.coefficients[2] = 0.0182f * amount;
                    iplAirAbsorptionCalculate(ctx_, src, listener, &am, dp.airAbsorption);
                    flags |= IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
                }
                dp.flags = static_cast<IPLDirectEffectFlags>(flags);
                iplDirectEffectApply(direct_[s], &dp, &monoIn_, &monoOut_);
                binauralIn = &monoOut_;
            }

            IPLBinauralEffectParams bp{};
            bp.direction = IPLVector3{pos.x / dist, pos.y / dist, pos.z / dist};
            bp.interpolation = interp;
            bp.spatialBlend = blend;
            bp.hrtf = hb->hrtf;
            bp.peakDelays = nullptr;
            iplBinauralEffectApply(hb->fx[s], &bp, binauralIn, &stereo_);
            if (usesCloseIld(hb->id) && p_[kCloseIld] > 0.0f)
                ild_[s].apply(stereo_.data[0], stereo_.data[1], kFrame,
                              closeIldTarget(pos, dist, hb->id == "builtin" ? 1.0f : 1.2f, p_[kCloseIld]),
                              sampleRate_);
            else
                ild_[s].bypass();
            for (uint32_t i = 0; i < kFrame; ++i) {
                accL_[i] += stereo_.data[0][i];
                accR_[i] += stereo_.data[1][i];
            }
        }
        liveBuf_.publish();

        // Output gain, smoothed per sample.
        const float target = dsptb::dbToGain(p_[kOutputGain]);
        if (!outGainInit_) {
            outGain_.reset(sampleRate_, 20.0, target);
            outGainInit_ = true;
        }
        outGain_.setTarget(target);
        for (uint32_t i = 0; i < kFrame; ++i) {
            const float g = outGain_.next();
            out[0][i] = accL_[i] * g;
            out[1][i] = accR_[i] * g;
        }
    }

    // ---- HRTF sets ----
    // Packaged sets live in <module>/assets/*.sofa. User sets live in <data dir>/hrtf/*.sofa, because the
    // package folder may be read-only, or in <module>/assets/custom/*.sofa for portable installs. A user set's
    // id is "custom/<file name without extension>" in both folders, and the data folder wins a clash.
    struct HrtfSet {
        std::string id;
        std::filesystem::path path;
    };

    // Last-write times of the scanned folders: they change when a set is added or removed.
    using AssetsStamp = std::array<std::filesystem::file_time_type, 3>;
    AssetsStamp assetsStamp() const {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(reinterpret_cast<const char8_t*>(host().moduleDir())) / "assets";
        std::error_code ec;
        const auto a = fs::last_write_time(root, ec);
        const auto b = fs::last_write_time(root / "custom", ec);
        const auto c = fs::last_write_time(userDir_, ec);
        return {a, b, c};
    }

    void scanSets() {
        namespace fs = std::filesystem;
        sets_.clear();
        setInfos_.clear();
        if (userDir_.empty())
            userDir_ = userHrtfDir();
        assetsStamp_ = assetsStamp();
        const fs::path root = fs::path(reinterpret_cast<const char8_t*>(host().moduleDir())) / "assets";
        std::error_code ec;
        const struct {
            fs::path dir;
            const char* prefix;
        } folders[] = {{root, ""}, {userDir_, "custom/"}, {root / "custom", "custom/"}};
        for (const auto& [dir, prefix] : folders) {
            if (dir.empty() || !fs::is_directory(dir, ec))
                continue;
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.is_regular_file(ec) && e.path().extension() == ".sofa")
                    files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& p : files) {
                HrtfSet s;
                s.id = prefix + utf8(p.stem());
                if (findSet(s.id))
                    continue;
                s.path = p;
                setInfos_.push_back({s.id, s.id});
                sets_.push_back(std::move(s));
            }
        }
        folder_ = utf8(userDir_.empty() ? root / "custom" : userDir_);
    }

    const HrtfSet* findSet(const std::string& id) const {
        for (const HrtfSet& s : sets_)
            if (s.id == id)
                return &s;
        return nullptr;
    }

    // What should be loaded now: the user's choice, or for "auto" the packaged set of the current range.
    // A set that is not installed falls back to the built-in HRTF.
    std::string effectiveHrtf() const {
        if (scene_.hrtf == "builtin")
            return "builtin";
        if (scene_.hrtf != "auto")
            return findSet(scene_.hrtf) ? scene_.hrtf : "builtin";
        const char* want = scene_.range == Range::Near ? "near-kemar-20cm" : "mid-sadie-d1";
        return findSet(want) ? want : "builtin";
    }

    std::string describe(const std::string& id) const {
        return "Using " + (id == "builtin" ? std::string("the Steam Audio built-in HRTF") : id);
    }

    // Builds the bundle on this thread and hands it to the audio thread. Returns false on failure.
    bool loadHrtfNow(const std::string& id) {
        joinHrtfWorker();
        std::string error;
        const HrtfSet* set = id == "builtin" ? nullptr : findSet(id);
        auto b = HrtfBundle::create(ctx_, audio_, id, set ? narrowPath(set->path) : std::string(), error);
        if (!b) {
            hrtfStatus_ = "Could not load " + id + ": " + error;
            host().log(DSPTB_LOG_ERROR, hrtfStatus_.c_str());
            return false;
        }
        hrtfSwap_.publish(std::move(b));
        hrtfLoaded_ = id;
        hrtfFailed_.clear();
        hrtfStatus_ = describe(id);
        return true;
    }

    void joinHrtfWorker() {
        if (hrtfWorker_.joinable())
            hrtfWorker_.join();
        hrtfWorkerBusy_.store(false);
        hrtfDone_.reset();
    }

    // UI thread, once per frame: collect a finished background load, and start one when the wanted set
    // differs from the loaded one (a 36 MB set takes about half a second to read; the UI must not stall).
    void pollHrtf() {
        if (++framesSinceScan_ >= 120) { // pick up sets dropped into the custom folder
            framesSinceScan_ = 0;
            if (assetsStamp() != assetsStamp_) // two stat calls; the directory listing only when one changed
                scanSets();
        }
        if (!ctx_)
            return;
        if (hrtfWorker_.joinable() && !hrtfWorkerBusy_.load()) {
            hrtfWorker_.join();
            if (hrtfDone_) {
                hrtfSwap_.publish(std::move(hrtfDone_));
                hrtfLoaded_ = hrtfWorkerId_;
                hrtfFailed_.clear();
                hrtfStatus_ = describe(hrtfLoaded_);
            } else {
                hrtfFailed_ = hrtfWorkerId_;
                hrtfStatus_ = "Could not load " + hrtfWorkerId_ + ": " + hrtfWorkerError_;
                host().log(DSPTB_LOG_ERROR, hrtfStatus_.c_str());
            }
        }
        if (hrtfWorker_.joinable())
            return; // still loading
        const std::string want = effectiveHrtf();
        if (want == hrtfLoaded_ || want == hrtfFailed_)
            return;
        hrtfWorkerId_ = want;
        hrtfStatus_ = "Loading " + want + "...";
        const HrtfSet* set = want == "builtin" ? nullptr : findSet(want);
        const std::string path = set ? set->path.string() : std::string();
        hrtfWorkerBusy_.store(true);
        hrtfWorker_ = std::thread([this, want, path] {
            std::string error;
            hrtfDone_ = HrtfBundle::create(ctx_, audio_, want, path, error);
            hrtfWorkerError_ = error;
            hrtfWorkerBusy_.store(false, std::memory_order_release);
        });
    }

    // ---- Steam Audio objects (created in prepare) ----
    IPLContext ctx_ = nullptr;
    IPLAudioSettings audio_{};
    std::array<IPLDirectEffect, kMaxSources> direct_{};
    IPLAudioBuffer monoIn_{}, monoOut_{}, stereo_{};
    bool buffersReady_ = false;

    // ---- audio-thread state ----
    double hostRate_ = 48000.0;
    double sampleRate_ = 48000.0; // the rate Steam Audio and the per-sample smoothing run at
    uint32_t factor_ = 1;         // host rate / engine rate: 1, 2 or 4
    uint32_t latency_ = kFrame;
    std::unique_ptr<dsptb::dsp::FifoAdapter> fifo_;
    std::vector<Down2> down_;
    std::vector<Up2> up_;
    std::array<std::array<std::vector<float>, 2>, 2> scratch_{};
    std::array<std::array<float, kFrame>, 2> coreIn_{}, coreOut_{};
    std::array<dsptb::rt::SmoothedValue, kMaxSources> gain_{};
    dsptb::rt::SmoothedValue outGain_;
    bool outGainInit_ = false;
    std::array<bool, kMaxSources> wasActive_{};
    std::array<float, kFrame> accL_{}, accR_{};
    std::array<float, 8> p_{};
    std::array<IldFilter, kMaxSources> ild_{};
    double positionSeconds_ = 0.0;
    bool useTransport_ = true;
    uint64_t streamFrames_ = 0; // frames processed since the module was created; never reset (free clocks)
    std::array<int64_t, kMaxSources> freeStart_{};
    std::array<uint32_t, kMaxSources> seenRequest_{};

    // ---- HRTF state (UI thread, except the worker's hand-over) ----
    std::vector<HrtfSet> sets_;
    std::vector<HrtfSetInfo> setInfos_;
    std::string folder_;
    std::filesystem::path userDir_;
    std::string hrtfLoaded_, hrtfFailed_, hrtfStatus_ = "Not loaded yet";
    std::thread hrtfWorker_;
    std::atomic<bool> hrtfWorkerBusy_{false};
    std::string hrtfWorkerId_, hrtfWorkerError_;
    uint32_t framesSinceScan_ = 0;
    AssetsStamp assetsStamp_{};
    std::unique_ptr<HrtfBundle> hrtfDone_;
    dsptb::rt::RtSwap<HrtfBundle> hrtfSwap_{8};

    // ---- shared between threads ----
    dsptb::rt::RtSwap<SceneSnapshot> swap_{16};
    dsptb::rt::TripleBuffer<LivePositions> liveBuf_;
    std::array<std::atomic<uint32_t>, kMaxSources> freeRequest_{};
    std::array<std::atomic<bool>, kMaxSources> freeRunning_{};

    // ---- UI thread ----
    Scene scene_;
    uint32_t sceneVersion_ = 0;
    SpatializerUi ui_;
};

DSPTB_REGISTER_MODULES(Spatializer);
