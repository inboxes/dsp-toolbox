// LoudnessMeter and LevelMeter.
//
// LoudnessMeter is a thin wrapper over libebur128 (MIT, EBU R128 /
// ITU-R BS.1770-4); LevelMeter implements its peak/RMS ballistics directly.

#include <dsptb/dsp_common/loudness.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include <ebur128.h>

namespace dsptb::dsp {

namespace {

constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

// libebur128 reports "no energy" as -HUGE_VAL, which is -infinity on IEEE
// platforms but a finite huge negative where HUGE_VAL is DBL_MAX. Map every
// such sentinel (and any NaN, defensively) to -infinity.
double mapLoudness(double value) noexcept {
    if (std::isnan(value) || value <= -1e30)
        return kMinusInfinity;
    return value;
}

// Convert a linear peak from libebur128 to dB (20*log10, amplitude dB).
double linearPeakToDb(double peak) noexcept {
    if (!(peak > 0.0)) // covers 0, negative and NaN
        return kMinusInfinity;
    return 20.0 * std::log10(peak);
}

ebur128_state* createState(double sampleRate, unsigned channels) {
    // The full mode set. HISTOGRAM gives constant memory:
    // without it add_frames appends to a growing block list and allocates.
    constexpr int kMode = EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA |
                          EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_HISTOGRAM;
    const auto rate = static_cast<unsigned long>(std::llround(sampleRate));
    ebur128_state* state = ebur128_init(channels, rate, kMode);
    if (state == nullptr)
        throw std::bad_alloc();
    return state;
}

ebur128_state* rawState(void* state) noexcept { return static_cast<ebur128_state*>(state); }

} // namespace

// ---------------------------------------------------------------------------
// LoudnessMeter
// ---------------------------------------------------------------------------

LoudnessMeter::LoudnessMeter(double sampleRate, unsigned channels, std::size_t maxBlock)
    : sampleRate_(sampleRate), channels_(channels), maxBlock_(maxBlock) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("LoudnessMeter: sampleRate must be positive");
    if (channels == 0)
        throw std::invalid_argument("LoudnessMeter: channels must be at least 1");
    if (maxBlock == 0)
        throw std::invalid_argument("LoudnessMeter: maxBlock must be at least 1");
    scratch_.resize(maxBlock_ * static_cast<std::size_t>(channels_));
    state_ = createState(sampleRate_, channels_);
}

LoudnessMeter::~LoudnessMeter() {
    if (state_ != nullptr) {
        ebur128_state* state = rawState(state_);
        ebur128_destroy(&state);
    }
}

LoudnessMeter::LoudnessMeter(LoudnessMeter&& other) noexcept
    : sampleRate_(other.sampleRate_), channels_(other.channels_), maxBlock_(other.maxBlock_),
      state_(std::exchange(other.state_, nullptr)), scratch_(std::move(other.scratch_)) {}

LoudnessMeter& LoudnessMeter::operator=(LoudnessMeter&& other) noexcept {
    if (this != &other) {
        if (state_ != nullptr) {
            ebur128_state* state = rawState(state_);
            ebur128_destroy(&state);
        }
        sampleRate_ = other.sampleRate_;
        channels_ = other.channels_;
        maxBlock_ = other.maxBlock_;
        state_ = std::exchange(other.state_, nullptr);
        scratch_ = std::move(other.scratch_);
    }
    return *this;
}

void LoudnessMeter::process(const float* const* planar, std::size_t frames) noexcept {
    if (state_ == nullptr || planar == nullptr || frames == 0)
        return;
    // libebur128 wants interleaved frames; chunk through the preallocated
    // scratch so any caller block size works without allocating.
    std::size_t offset = 0;
    while (offset < frames) {
        const std::size_t chunk = std::min(frames - offset, maxBlock_);
        for (std::size_t i = 0; i < chunk; ++i) {
            for (unsigned ch = 0; ch < channels_; ++ch)
                scratch_[i * channels_ + ch] = planar[ch][offset + i];
        }
        // The return value can only report EBUR128_ERROR_NOMEM, which
        // histogram mode does not produce after warm-up; process() is
        // noexcept, so there is nothing actionable to do with it here.
        (void)ebur128_add_frames_float(rawState(state_), scratch_.data(), chunk);
        offset += chunk;
    }
}

double LoudnessMeter::momentaryLufs() const noexcept {
    if (state_ == nullptr)
        return kMinusInfinity;
    double out = kMinusInfinity;
    if (ebur128_loudness_momentary(rawState(state_), &out) != EBUR128_SUCCESS)
        return kMinusInfinity;
    return mapLoudness(out);
}

double LoudnessMeter::shortTermLufs() const noexcept {
    if (state_ == nullptr)
        return kMinusInfinity;
    double out = kMinusInfinity;
    if (ebur128_loudness_shortterm(rawState(state_), &out) != EBUR128_SUCCESS)
        return kMinusInfinity;
    return mapLoudness(out);
}

double LoudnessMeter::integratedLufs() const noexcept {
    if (state_ == nullptr)
        return kMinusInfinity;
    double out = kMinusInfinity;
    if (ebur128_loudness_global(rawState(state_), &out) != EBUR128_SUCCESS)
        return kMinusInfinity;
    return mapLoudness(out);
}

double LoudnessMeter::loudnessRangeLu() const noexcept {
    if (state_ == nullptr)
        return 0.0;
    double out = 0.0;
    if (ebur128_loudness_range(rawState(state_), &out) != EBUR128_SUCCESS)
        return 0.0;
    if (std::isnan(out) || out < 0.0)
        return 0.0;
    return out;
}

double LoudnessMeter::truePeakDbtp(unsigned ch) const noexcept {
    if (state_ == nullptr || ch >= channels_)
        return kMinusInfinity;
    double peak = 0.0;
    if (ebur128_true_peak(rawState(state_), ch, &peak) != EBUR128_SUCCESS)
        return kMinusInfinity;
    return linearPeakToDb(peak);
}

double LoudnessMeter::samplePeakDbfs(unsigned ch) const noexcept {
    if (state_ == nullptr || ch >= channels_)
        return kMinusInfinity;
    double peak = 0.0;
    if (ebur128_sample_peak(rawState(state_), ch, &peak) != EBUR128_SUCCESS)
        return kMinusInfinity;
    return linearPeakToDb(peak);
}

void LoudnessMeter::reset() {
    // No in-place reset exists in libebur128 1.2.6 (see the header note):
    // create the replacement first so a failed init leaves the old state
    // untouched, then swap.
    ebur128_state* fresh = createState(sampleRate_, channels_);
    if (state_ != nullptr) {
        ebur128_state* old = rawState(state_);
        ebur128_destroy(&old);
    }
    state_ = fresh;
}

// ---------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------

namespace {

// Samples between exact running-sum recomputes.
constexpr std::size_t kRecomputeInterval = std::size_t{1} << 16;

std::size_t windowSamplesFor(double sampleRate, double rmsWindowMs) {
    const double samples = std::round(rmsWindowMs / 1000.0 * sampleRate);
    return samples < 1.0 ? std::size_t{1} : static_cast<std::size_t>(samples);
}

} // namespace

LevelMeter::LevelMeter(double sampleRate, unsigned channels, double rmsWindowMs, double peakHoldS,
                       double fallDbPerS)
    : sampleRate_(sampleRate), channels_(channels), peakHoldS_(peakHoldS), fallDbPerS_(fallDbPerS) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("LevelMeter: sampleRate must be positive");
    if (channels == 0)
        throw std::invalid_argument("LevelMeter: channels must be at least 1");
    if (!(rmsWindowMs > 0.0))
        throw std::invalid_argument("LevelMeter: rmsWindowMs must be positive");
    if (!(peakHoldS >= 0.0))
        throw std::invalid_argument("LevelMeter: peakHoldS must not be negative");
    if (!(fallDbPerS >= 0.0))
        throw std::invalid_argument("LevelMeter: fallDbPerS must not be negative");
    windowSamples_ = windowSamplesFor(sampleRate_, rmsWindowMs);
    rings_.assign(channels_, std::vector<float>(windowSamples_, 0.0f));
    sums_.assign(channels_, 0.0);
    writePos_.assign(channels_, 0);
    validCounts_.assign(channels_, 0);
    sinceRecompute_.assign(channels_, 0);
    peakDb_.assign(channels_, kMinusInfinity);
    holdRemaining_.assign(channels_, 0.0);
    clipped_.assign(channels_, 0);
}

double LevelMeter::recomputeSum(unsigned ch, std::size_t valid) const noexcept {
    // Exact sum over the ring's valid entries. Addition order differs from
    // the incremental path, which is the point: accumulated rounding in the
    // running sum is discarded and replaced by a fresh total.
    double sum = 0.0;
    const std::vector<float>& ring = rings_[ch];
    for (std::size_t i = 0; i < valid; ++i) {
        const double v = static_cast<double>(ring[i]);
        sum += v * v;
    }
    return sum;
}

void LevelMeter::updatePeak(unsigned ch, double blockPeakLinear, double blockSeconds) noexcept {
    const double blockDb = linearPeakToDb(blockPeakLinear);
    double& peak = peakDb_[ch];
    if (blockDb >= peak) {
        peak = blockDb;
        holdRemaining_[ch] = peakHoldS_;
        return;
    }
    double remaining = blockSeconds;
    if (holdRemaining_[ch] > 0.0) {
        const double used = std::min(holdRemaining_[ch], remaining);
        holdRemaining_[ch] -= used;
        remaining -= used;
    }
    if (remaining > 0.0) {
        peak -= fallDbPerS_ * remaining;
        // Audio quieter than the old peak can still catch the falling
        // display; treat that as a fresh peak (and restart the hold).
        if (blockDb > peak) {
            peak = blockDb;
            holdRemaining_[ch] = peakHoldS_;
        }
    }
}

void LevelMeter::process(const float* const* planar, std::size_t frames) noexcept {
    if (planar == nullptr || frames == 0)
        return;
    const double blockSeconds = static_cast<double>(frames) / sampleRate_;
    for (unsigned ch = 0; ch < channels_; ++ch) {
        const float* input = planar[ch];
        if (input == nullptr)
            continue;
        std::vector<float>& ring = rings_[ch];
        // Carry the per-channel state in locals for the whole block; the
        // members are written back once, after the sample loop. The exact
        // recompute refreshes the local sum in place at the same sample
        // boundary as before, so the numerical results are unchanged.
        std::size_t pos = writePos_[ch];
        double sum = sums_[ch];
        std::size_t valid = validCounts_[ch];
        std::size_t sinceRecompute = sinceRecompute_[ch];
        double blockPeak = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            const float sample = input[i];
            const double value = static_cast<double>(sample);
            const double magnitude = std::fabs(value);
            if (magnitude >= 1.0)
                clipped_[ch] = 1;
            if (magnitude > blockPeak)
                blockPeak = magnitude;
            // Running sum: add the new square, evict the leaving one.
            const double evicted = static_cast<double>(ring[pos]);
            sum += value * value - evicted * evicted;
            ring[pos] = sample;
            pos = (pos + 1 == windowSamples_) ? 0 : pos + 1;
            if (valid < windowSamples_)
                ++valid;
            if (++sinceRecompute >= kRecomputeInterval) {
                sinceRecompute = 0;
                sum = recomputeSum(ch, valid);
            }
        }
        writePos_[ch] = pos;
        sums_[ch] = sum;
        validCounts_[ch] = valid;
        sinceRecompute_[ch] = sinceRecompute;
        updatePeak(ch, blockPeak, blockSeconds);
    }
}

float LevelMeter::peakDbfs(unsigned ch) const noexcept {
    if (ch >= channels_)
        return static_cast<float>(kMinusInfinity);
    return static_cast<float>(peakDb_[ch]);
}

float LevelMeter::rmsDbfs(unsigned ch) const noexcept {
    if (ch >= channels_ || validCounts_[ch] == 0)
        return static_cast<float>(kMinusInfinity);
    const double meanSquare = sums_[ch] / static_cast<double>(validCounts_[ch]);
    if (!(meanSquare > 0.0))
        return static_cast<float>(kMinusInfinity);
    return static_cast<float>(10.0 * std::log10(meanSquare));
}

bool LevelMeter::clipped(unsigned ch) const noexcept {
    if (ch >= channels_)
        return false;
    return clipped_[ch] != 0;
}

void LevelMeter::clearClip() noexcept { std::fill(clipped_.begin(), clipped_.end(), char{0}); }

void LevelMeter::setRmsWindowMs(double rmsWindowMs) {
    if (!(rmsWindowMs > 0.0))
        throw std::invalid_argument("LevelMeter: rmsWindowMs must be positive");
    windowSamples_ = windowSamplesFor(sampleRate_, rmsWindowMs);
    // New rings start silent (documented in the header); sizes may differ
    // per call, so this allocates and is not real-time safe.
    for (unsigned ch = 0; ch < channels_; ++ch) {
        rings_[ch].assign(windowSamples_, 0.0f);
        sums_[ch] = 0.0;
        writePos_[ch] = 0;
        validCounts_[ch] = 0;
        sinceRecompute_[ch] = 0;
    }
}

void LevelMeter::setPeakHoldS(double peakHoldS) noexcept {
    peakHoldS_ = peakHoldS < 0.0 ? 0.0 : peakHoldS;
}

} // namespace dsptb::dsp
