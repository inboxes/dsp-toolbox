// PsolaShifter.
//
// Live mode follows Lent (1989): a read head replays the input history at
// speed `ratio`; when its delay would leave [L, L + T0] it jumps by
// exactly one period (the splice lands on the same point of the
// waveform), crossfaded over min(T0/2, 2 ms).
//
// Quality mode is TD-PSOLA (Moulines & Charpentier, 1990): pitch marks
// one period apart, grains of two periods overlap-added at synthesis
// marks spaced T0/ratio, normalised by the window sum. Two streaming
// details matter for the spec's identity test (ratio = 1 must reproduce
// the delayed input):
//   * synthesis positions walk the analysis marks themselves: a
//     fractional mark index advances by 1/ratio per grain and positions
//     interpolate between adjacent marks. At ratio = 1 the index is
//     always integral, so every synthesis mark coincides with an
//     analysis mark and each grain contributes exactly the delayed
//     input sample. (An independent real-valued synthesis grid cannot
//     do this: its sub-sample offset from the marks misplaces every
//     waveform discontinuity by a fraction of a sample, which costs
//     far more than the -40 dB budget on a sawtooth.) The walk is
//     anchored at the second finalised mark — the first mark's search
//     window is truncated by the voicing start and can be up to half a
//     period off the phase all later marks agree on;
//   * grain samples are read with linear interpolation at the real
//     synthesis position, so placement is sub-sample accurate.

#include <dsptb/dsp_common/psola.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dsptb::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kNoMark = -1; // qualityNearestMark sentinel (marks are >= 0)

// Smallest power of two >= v (v >= 1).
std::size_t nextPow2(std::size_t v) noexcept {
    std::size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

// Raised cosine ramp, 0 at phase 0 and 1 at phase 1.
double raisedCos(double phase) noexcept { return 0.5 * (1.0 - std::cos(kPi * phase)); }

} // namespace

PsolaShifter::PsolaShifter(double sampleRate, const PsolaConfig& config)
    : sampleRate_(sampleRate), config_(config) {
    if (!(sampleRate > 0.0))
        throw std::invalid_argument("PsolaShifter: sampleRate must be positive");
    if (!(config.minHz > 0.0))
        throw std::invalid_argument("PsolaShifter: minHz must be positive");
    if (!(config.maxHz > config.minHz))
        throw std::invalid_argument("PsolaShifter: maxHz must be greater than minHz");
    if (config.maxBlock == 0)
        throw std::invalid_argument("PsolaShifter: maxBlock must be at least 1");
    if (!(config.crossfadeMs >= 0.0))
        throw std::invalid_argument("PsolaShifter: crossfadeMs must not be negative");

    if (config.mode == PsolaMode::Live)
        latency_ = static_cast<std::size_t>(std::ceil(0.001 * sampleRate)) + 4;
    else
        latency_ = static_cast<std::size_t>(std::ceil(sampleRate / config.minHz));
    crossfadeSamples_ =
        static_cast<std::size_t>(std::llround(config.crossfadeMs * sampleRate / 1000.0));

    // Longest period the configured range can produce; sizes every buffer.
    const std::size_t t0Max = static_cast<std::size_t>(std::ceil(sampleRate / config.minHz));
    const std::size_t histNeed = config.mode == PsolaMode::Live
                                     ? latency_ + t0Max + 512
                                     : latency_ + 6 * t0Max + 1024;
    hist_.assign(nextPow2(histNeed), 0.0f);
    histMask_ = hist_.size() - 1;
    if (config.mode == PsolaMode::Quality) {
        olaAcc_.assign(nextPow2(4 * t0Max + 2048), 0.0);
        olaWeight_.assign(olaAcc_.size(), 0.0);
        olaMask_ = olaAcc_.size() - 1;
        marks_.assign(2048, 0);
    }
    initState();
}

void PsolaShifter::initState() noexcept {
    inCount_ = 0;
    voiceGain_ = 0.0;
    voicedTarget_ = false;
    readPos_ = -static_cast<double>(latency_);
    jumpFromPos_ = 0.0;
    jumpFromSpeed_ = 1.0;
    readSpeed_ = 1.0;
    jumpPos_ = 0;
    jumpLen_ = 0;
    lastT0_ = 0.0;
    qualityRunning_ = false;
    qualityT0_ = 0.0;
    synthAnchorPending_ = false;
    marksHead_ = 0;
    marksCount_ = 0;
    totalMarks_ = 0;
    marksAbsBase_ = 0;
    synthMu_ = 0.0;
    nextMarkExpected_ = 0.0;
    nextSynth_ = 0.0;
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    std::fill(olaAcc_.begin(), olaAcc_.end(), 0.0);
    std::fill(olaWeight_.begin(), olaWeight_.end(), 0.0);
}

void PsolaShifter::reset() noexcept { initState(); }

void PsolaShifter::process(const float* in, float* out, std::size_t n, float f0Hz,
                           float ratio) noexcept {
    const bool voiced = f0Hz > 0.0f; // NaN fails the comparison: unvoiced
    double r = static_cast<double>(ratio);
    if (!(r > 0.0))
        r = 1.0; // defensive: a non-positive or NaN ratio means unity
    r = std::clamp(r, 0.5, 2.0);
    double t0 = 0.0;
    if (voiced) {
        const double f0 = std::clamp(static_cast<double>(f0Hz), config_.minHz, config_.maxHz);
        t0 = sampleRate_ / f0;
        qualityT0_ = t0;
    }
    if (config_.mode == PsolaMode::Quality && voiced && !voicedTarget_)
        qualityRestartVoiced(t0);
    voicedTarget_ = voiced;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = step(in[i], voiced, t0, r);
}

float PsolaShifter::step(float x, bool voiced, double t0, double ratio) noexcept {
    hist_[static_cast<std::size_t>(inCount_) & histMask_] = x;
    ++inCount_;
    const std::int64_t m = inCount_ - 1; // output index produced now

    // Voiced <-> unvoiced crossfade gain (spec: crossfadeMs).
    const double target = voiced ? 1.0 : 0.0;
    if (crossfadeSamples_ == 0) {
        voiceGain_ = target;
    } else {
        const double delta = 1.0 / static_cast<double>(crossfadeSamples_);
        if (voiceGain_ < target)
            voiceGain_ = std::min(1.0, voiceGain_ + delta);
        else if (voiceGain_ > target)
            voiceGain_ = std::max(0.0, voiceGain_ - delta);
    }
    if (config_.mode == PsolaMode::Quality && !voiced && voiceGain_ == 0.0)
        qualityRunning_ = false; // fully unvoiced: PSOLA state sleeps until restart

    float path;
    float delayed;
    if (config_.mode == PsolaMode::Live) {
        path = liveSample();
        delayed = histAt(m - static_cast<std::int64_t>(latency_));
        liveAdvance(voiced, t0, ratio);
    } else {
        delayed = histAt(m - static_cast<std::int64_t>(latency_));
        path = qualitySample(voiced, t0, ratio);
    }
    if (voiceGain_ >= 1.0)
        return path;
    if (voiceGain_ <= 0.0)
        return delayed;
    return static_cast<float>(voiceGain_ * static_cast<double>(path) +
                              (1.0 - voiceGain_) * static_cast<double>(delayed));
}

float PsolaShifter::histAt(std::int64_t index) const noexcept {
    if (index < 0 || index >= inCount_)
        return 0.0f;
    return hist_[static_cast<std::size_t>(index) & histMask_];
}

float PsolaShifter::readHermite(double pos) const noexcept {
    // Safety clamp (see header): never read closer than 2 samples to the
    // newest input; only reachable in the extreme ratio/f0 corner.
    const double maxPos = static_cast<double>(inCount_ - 1) - 2.0;
    if (pos > maxPos)
        pos = maxPos;
    const std::int64_t i = static_cast<std::int64_t>(std::floor(pos));
    const double mu = pos - static_cast<double>(i);
    const double x0 = static_cast<double>(histAt(i - 1));
    const double x1 = static_cast<double>(histAt(i));
    const double x2 = static_cast<double>(histAt(i + 1));
    const double x3 = static_cast<double>(histAt(i + 2));
    // Catmull-Rom spline through x1 (at mu = 0) and x2 (at mu = 1).
    const double a = -0.5 * x0 + 1.5 * x1 - 1.5 * x2 + 0.5 * x3;
    const double b = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3;
    const double c = -0.5 * x0 + 0.5 * x2;
    return static_cast<float>(((a * mu + b) * mu + c) * mu + x1);
}

// ---------------------------------------------------------------- Live

float PsolaShifter::liveSample() noexcept {
    if (jumpLen_ > 0) {
        const double w =
            raisedCos(static_cast<double>(jumpPos_ + 1) / static_cast<double>(jumpLen_));
        return static_cast<float>((1.0 - w) * static_cast<double>(readHermite(jumpFromPos_)) +
                                  w * static_cast<double>(readHermite(readPos_)));
    }
    return readHermite(readPos_);
}

int PsolaShifter::jumpLength(double t0) const noexcept {
    // min(T0/2, 2 ms) per spec §4.11, at least one sample.
    const double ms2 = 0.002 * sampleRate_;
    const double len = std::min(0.5 * t0, ms2);
    const long long samples = std::llround(len);
    return static_cast<int>(samples < 1 ? 1 : samples);
}

void PsolaShifter::startJump(double toPos, double fromSpeed, double t0) noexcept {
    jumpFromPos_ = readPos_;
    jumpFromSpeed_ = fromSpeed;
    readPos_ = toPos;
    jumpPos_ = 0;
    jumpLen_ = jumpLength(t0);
}

void PsolaShifter::liveAdvance(bool voiced, double t0, double ratio) noexcept {
    if (voiced)
        lastT0_ = t0;
    if (jumpLen_ > 0) {
        jumpFromPos_ += jumpFromSpeed_;
        readPos_ += readSpeed_;
        if (++jumpPos_ >= jumpLen_)
            jumpLen_ = 0;
        return; // no new triggers while a jump crossfade runs
    }
    const bool shifting = voiced && ratio != 1.0;
    const double speed = shifting ? ratio : 1.0;
    const double prevSpeed = readSpeed_;
    readPos_ += speed;
    readSpeed_ = speed;
    const double dNext = static_cast<double>(inCount_) - readPos_; // delay at the next sample
    const double lat = static_cast<double>(latency_);
    if (shifting) {
        if (dNext < lat) {
            // Shift up: the head catches up with the input; jump back one
            // whole period (e += T0) so the splice is phase-aligned.
            startJump(readPos_ - t0, speed, t0);
        } else if (dNext > lat + t0) {
            // Shift down: the head falls behind; jump forward one period.
            startJump(readPos_ + t0, speed, t0);
        }
    } else if (dNext != lat) {
        // Unvoiced or ratio = 1: return e to 0 with one crossfaded jump,
        // landing exactly on the integer delay grid so the output is the
        // input delayed by precisely latencyFrames().
        const double tj = t0 > 0.0 ? t0 : (lastT0_ > 0.0 ? lastT0_ : sampleRate_ / config_.minHz);
        startJump(static_cast<double>(inCount_) - lat, prevSpeed, tj);
    }
}

// ---------------------------------------------------------------- Quality

void PsolaShifter::qualityRestartVoiced(double t0) noexcept {
    std::fill(olaAcc_.begin(), olaAcc_.end(), 0.0);
    std::fill(olaWeight_.begin(), olaWeight_.end(), 0.0);
    marksHead_ = 0;
    marksCount_ = 0;
    totalMarks_ = 0;
    marksAbsBase_ = 0;
    synthMu_ = 0.0;
    // First expected mark a quarter period ahead of the newest sample, so
    // its +-0.25*T0 search window lies in the near future.
    nextMarkExpected_ = static_cast<double>(inCount_ - 1) + 0.25 * t0;
    synthAnchorPending_ = true; // synthesis grid anchors at the first mark
    qualityRunning_ = true;
}

void PsolaShifter::qualityFinaliseMarks(double t0) noexcept {
    const std::int64_t newest = inCount_ - 1;
    const double quarter = 0.25 * t0;
    while (nextMarkExpected_ + quarter <= static_cast<double>(newest)) {
        std::int64_t lo = static_cast<std::int64_t>(std::ceil(nextMarkExpected_ - quarter));
        std::int64_t hi = static_cast<std::int64_t>(std::floor(nextMarkExpected_ + quarter));
        if (lo < 0)
            lo = 0;
        std::int64_t best = lo;
        float bestVal = histAt(lo);
        for (std::int64_t i = lo + 1; i <= hi; ++i) {
            const float v = histAt(i);
            if (v > bestVal) {
                bestVal = v;
                best = i;
            }
        }
        if (marksCount_ < marks_.size()) {
            marks_[(marksHead_ + marksCount_) % marks_.size()] = best;
            ++marksCount_;
        } else {
            marks_[marksHead_] = best; // ring full: overwrite the oldest
            marksHead_ = (marksHead_ + 1) % marks_.size();
            ++marksAbsBase_;
        }
        ++totalMarks_;
        if (synthAnchorPending_ && totalMarks_ >= 2) {
            // Anchor the synthesis walk at the SECOND mark, not the
            // first: the first search window is truncated by the
            // voicing start and can miss the cycle's maximum entirely,
            // leaving its mark up to half a period off the phase every
            // later mark agrees on.
            synthMu_ = static_cast<double>(totalMarks_ - 1);
            nextSynth_ = static_cast<double>(best);
            synthAnchorPending_ = false;
        }
        // The expected grid advances by exactly one period per mark
        // (it is NOT re-centred on the snapped mark): every search
        // window then sees the same point of the waveform cycle, so the
        // snap offset is the same for every mark. That consistency is
        // what lets the synthesis grid, anchored at the first mark,
        // coincide with the analysis marks at ratio = 1.
        nextMarkExpected_ += t0;
    }
}

std::int64_t PsolaShifter::qualityNearestMark(double synthPos, double t0) const noexcept {
    // A mark is usable only when its whole grain has arrived.
    const std::int64_t limit = (inCount_ - 1) - static_cast<std::int64_t>(std::llround(t0));
    std::int64_t best = 0;
    double bestDist = 0.0;
    bool found = false;
    for (std::size_t k = 0; k < marksCount_; ++k) {
        const std::int64_t a = marks_[(marksHead_ + k) % marks_.size()];
        if (a > limit)
            continue;
        const double dist = std::abs(static_cast<double>(a) - synthPos);
        if (!found || dist < bestDist) {
            found = true;
            best = a;
            bestDist = dist;
        }
    }
    return found ? best : kNoMark;
}

void PsolaShifter::qualityAddGrain(double synthPos, double t0) noexcept {
    const std::int64_t a = qualityNearestMark(synthPos, t0);
    if (a == kNoMark)
        return;
    const std::int64_t half = static_cast<std::int64_t>(std::llround(t0));
    const std::int64_t c = static_cast<std::int64_t>(std::llround(synthPos));
    const std::int64_t latency = static_cast<std::int64_t>(latency_);
    const std::int64_t mNow = inCount_ - 1;
    for (std::int64_t p = c - half; p < c + half; ++p) {
        const std::int64_t outIdx = p + latency;
        if (outIdx < mNow)
            continue; // that output is already emitted; its window weight is ~0 there anyway
        const double u = (static_cast<double>(p) - synthPos) / t0;
        const double w = 0.5 * (1.0 + std::cos(kPi * u)); // Hann, zero at |u| = 1
        if (w <= 0.0)
            continue;
        // Grain sample: input centred on the analysis mark, read at the
        // real synthesis offset with linear interpolation.
        const double src = static_cast<double>(a) + (static_cast<double>(p) - synthPos);
        const std::int64_t si = static_cast<std::int64_t>(std::floor(src));
        const double frac = src - static_cast<double>(si);
        const double v = static_cast<double>(histAt(si)) * (1.0 - frac) +
                         static_cast<double>(histAt(si + 1)) * frac;
        const std::size_t slot = static_cast<std::size_t>(outIdx) & olaMask_;
        olaAcc_[slot] += w * v;
        olaWeight_[slot] += w;
    }
}

bool PsolaShifter::markAtAbs(std::int64_t absIndex, std::int64_t& out) const noexcept {
    if (absIndex < marksAbsBase_ || absIndex >= totalMarks_)
        return false;
    out = marks_[(marksHead_ + static_cast<std::size_t>(absIndex - marksAbsBase_)) %
                 marks_.size()];
    return true;
}

double PsolaShifter::qualitySynthPos(double mu, double t0) const noexcept {
    const std::int64_t i0 = static_cast<std::int64_t>(std::floor(mu));
    const double frac = mu - static_cast<double>(i0);
    std::int64_t a0 = 0;
    if (!markAtAbs(i0, a0)) {
        // Past the last finalised mark: extrapolate from it with the
        // nominal period (used only for deadline bookkeeping; the grain
        // itself takes the nearest mark that has actually arrived).
        std::int64_t aLast = 0;
        if (!markAtAbs(totalMarks_ - 1, aLast))
            return 1e30; // no marks at all: park the position far away
        return static_cast<double>(aLast) +
               (mu - static_cast<double>(totalMarks_ - 1)) * t0;
    }
    if (frac <= 0.0)
        return static_cast<double>(a0);
    std::int64_t a1 = 0;
    if (markAtAbs(i0 + 1, a1))
        return static_cast<double>(a0) + frac * static_cast<double>(a1 - a0);
    return static_cast<double>(a0) + frac * t0;
}

void PsolaShifter::qualityProcessDueGrains(double t0, double ratio) noexcept {
    if (synthAnchorPending_)
        return; // no grains until the second analysis mark anchors the walk
    const double tau = static_cast<double>(inCount_ - 1 - static_cast<std::int64_t>(latency_));
    const double dmu = 1.0 / ratio; // analysis marks consumed per grain
    nextSynth_ = qualitySynthPos(synthMu_, t0);
    // Skip synthesis marks whose grain would end before the content
    // position now being emitted (can only happen after a restart race).
    while (nextSynth_ + t0 < tau) {
        synthMu_ += dmu;
        nextSynth_ = qualitySynthPos(synthMu_, t0);
    }
    // A grain centred at s contributes to outputs up to s + T0, and the
    // output for s - T0 is emitted now: process every mark up to tau + T0.
    while (nextSynth_ <= tau + t0) {
        qualityAddGrain(nextSynth_, t0);
        synthMu_ += dmu;
        nextSynth_ = qualitySynthPos(synthMu_, t0);
    }
}

float PsolaShifter::qualitySample(bool /*voiced*/, double t0, double ratio) noexcept {
    const std::int64_t m = inCount_ - 1;
    const float delayed = histAt(m - static_cast<std::int64_t>(latency_));
    if (!qualityRunning_)
        return delayed;
    const double t0q = t0 > 0.0 ? t0 : qualityT0_;
    if (t0q <= 0.0)
        return delayed;
    qualityFinaliseMarks(t0q);
    qualityProcessDueGrains(t0q, ratio);
    // Drop marks far behind the emission point; they can never be the
    // nearest mark again and the history ring no longer holds them.
    const double tau = static_cast<double>(m - static_cast<std::int64_t>(latency_));
    const double t0Max = sampleRate_ / config_.minHz;
    while (marksCount_ > 1 &&
           static_cast<double>(marks_[marksHead_]) < tau - 4.0 * t0Max) {
        marksHead_ = (marksHead_ + 1) % marks_.size();
        --marksCount_;
        ++marksAbsBase_;
    }
    const std::size_t slot = static_cast<std::size_t>(m) & olaMask_;
    const double w = olaWeight_[slot];
    float out = delayed; // spec step 4: below 1e-3 window sum, use the delayed input
    if (w >= 1e-3)
        out = static_cast<float>(olaAcc_[slot] / w);
    olaAcc_[slot] = 0.0;
    olaWeight_[slot] = 0.0;
    return out;
}

} // namespace dsptb::dsp
