#pragma once

// VoiceStats: formants, vocal-tract length (VTL), LTAS and HNR of a speaker.
//
// Measures a speaker from voiced frames. The caller runs YinPitch
// itself and passes only voiced frames plus the YIN confidence. Runs offline
// on a worker thread, not on the audio thread.
//
// Formants: the frame is decimated by D = floor(fs/11025) with a
// Hann-windowed-sinc low-pass (cutoff 0.45*fs/D, 16*D+1 taps), pre-emphasised
// (0.7, see voice_stats.cpp for why it is not the usual 0.97), Hann-windowed and fit with LPC order 12 (autocorrelation +
// Levinson-Durbin). The envelope |1/A(z)| is evaluated on 2048 points over
// 0...fs/(2D) in the linear domain (|A|^2 from the autocorrelation of the
// A(z) coefficients, Chebyshev recurrence, two points interleaved — see
// voice_stats.cpp; it must stay in double, a float evaluation leaves
// rounding ripples that read as formants on steep voices); peaks between
// 200 and 5000 Hz are parabolically interpolated (on dB) and the first 4
// whose -3 dB width is below 600 Hz are kept.
//
// VTL: single-tube regression through the origin,
//   Fi = (2i-1)*c/(4L)  =>  L = c*sum(2i-1)^2 / (4*sum(2i-1)*Fi),
// over i = 1...count. Valid when count >= 3 and the result lies in
// [minVtlCm, maxVtlCm]. vtlFromFormants returns 17.00 +/- 0.01 on the exact
// tube formants for L = 17 cm, with 4 or 3 formants.
//
// LTAS: the Hann-windowed power spectrum (RealFft) of the full-rate frame,
// reduced to 32 log-spaced bands (centres 100*128^(i/31) Hz, edges at the
// geometric midpoints, outer edges half a band beyond). Each band value is
// the MEAN bin power over its bins, with fractional bin-overlap weights
// normalised by the effective bin count (so a band that contains no whole
// bin, e.g. band 1 at 44.1 kHz with a 2048-point frame, still gets a value
// from the bins it partly overlaps, and white noise stays flat). summary()
// converts to dB and removes the mean over bands. Sample rates below
// ~23.7 kHz are rejected by the constructor: the top band starts at
// ~11.84 kHz and Nyquist must reach it, or a band gets no bin overlap
// and its value is 0/0 = NaN.
//
// HNR: per frame, 10*log10(c/(1-c)) with c = the YIN confidence clamped to
// [0.01, 0.9999], then clamped to [-10, 40] dB; summary() gives the median
// (Boersma 1993).
//
// All memory is allocated by the constructor (median storage for maxFrames
// frames, one mutable scratch buffer for the median selection, the FIR and
// the FFT). pushFrame, summary and reset are noexcept and never allocate,
// lock or throw. Frames beyond maxFrames are counted in summary().frames
// but their VTL/HNR values are not stored, so the medians cover the first
// maxFrames frames. summary() on zero frames returns 0 for the medians and
// all-zero ltasDb.
//
// References: Reby et al. (2005), formant dispersion; Anikin, Barreda &
// Reby, "A practical guide to calculating vocal tract length and
// scale-invariant formant patterns", Behavior Research Methods (2024);
// Boersma (1993), HNR.
//
// Thread safety: one instance is a single stateful stream; it must not be
// used concurrently from several threads.

#include <dsptb/dsp_common/fft.hpp>

#include <cstddef>
#include <vector>

namespace dsptb::dsp {

struct VoiceStatsConfig {
    std::size_t frameSize = 0;           // 0 = auto: 2048 at fs <= 50 kHz, else 4096
    std::size_t maxFrames = 120000;      // storage for medians, reserved in the constructor
    double speedOfSoundCmS = 35000;      // warm, humid air (Stevens)
    float minVtlCm = 8.f, maxVtlCm = 24.f;
};

struct FormantFrame {
    int count;      // formants found (0..4); hz beyond count is 0
    float hz[4];    // formant frequencies in Hz, ascending
    float vtlCm;    // regression VTL when count >= 3, else 0
    bool valid;     // count >= 3 and vtlCm in [minVtlCm, maxVtlCm]
};

struct VoiceSummary {
    float vtlCmMedian;    // median over stored valid-VTL frames (0 if none)
    std::size_t vtlFrames; // frames with a valid VTL
    float ltasDb[32];     // mean removed; all 0 when no frames were pushed
    float hnrDbMedian;    // median over stored frames (0 if none)
    std::size_t frames;   // frames pushed, including beyond maxFrames
};

class VoiceStats {
public:
    // Throws std::invalid_argument for a non-positive or non-finite sample
    // rate, a sample rate whose Nyquist is below the top LTAS band's lower
    // edge (~11.84 kHz, so fs ≲ 23.7 kHz), a frameSize that is not a power
    // of two or is < 512 (when non-zero), maxFrames == 0, minVtlCm >=
    // maxVtlCm, a non-positive speed of sound, or a frameSize/decimation
    // combination that leaves too few decimated samples for the order-12 LPC.
    VoiceStats(double sampleRate, const VoiceStatsConfig& config = {});

    std::size_t frameSize() const noexcept { return frameSize_; }

    // One voiced frame (frameSize() samples) and its YIN confidence
    // (1 - CMND at the chosen lag). frame must point to frameSize() floats.
    // A NaN confidence is treated as unvoiced (-10 dB).
    FormantFrame pushFrame(const float* frame, float yinConfidence) noexcept;

    VoiceSummary summary() const noexcept;

    void reset() noexcept;

    // Single-tube VTL from `count` formant frequencies in Hz (count >= 1;
    // returns 0 for count <= 0). The regression is through the origin.
    static float vtlFromFormants(const float* hz, int count, double speedOfSoundCmS) noexcept;

    // Band centres: 100 * 128^(i/31) Hz.
    static void ltasBandCentres(float out[32]) noexcept;

private:
    // One LTAS band: the bins it overlaps, with fractional weights on the
    // edge bins (interior bins have weight 1). value = sum(w*P)/sum(w).
    struct LtasBand {
        std::size_t firstBin;
        std::size_t lastBin;
        double firstWeight;
        double lastWeight;
        double weightSum;
    };

    FormantFrame measureFormants(const float* frame) noexcept;
    void accumulateLtas(const float* frame) noexcept;

    double sampleRate_;
    std::size_t frameSize_;
    std::size_t maxFrames_;
    double speedOfSoundCmS_;
    float minVtlCm_;
    float maxVtlCm_;
    std::size_t decimation_;     // D = floor(fs/11025)
    std::size_t decimatedSize_;  // frameSize_/D

    RealFft fft_;                // frameSize_, for the LTAS
    std::vector<float> fir_;    // windowed-sinc decimation filter (float:
                                // the FIR is the hot loop; float halves its
                                // cost and the formant errors are unchanged)
    std::vector<double> decimated_;
    std::vector<double> windowedDec_; // pre-emphasised, Hann-windowed decimated frame
    std::vector<double> hannDec_;     // Hann window over decimatedSize_
    std::vector<double> envelopeMag2_; // |A(z)|^2 on 2048 points (linear, not dB)
    std::vector<float> hannFull_;     // Hann window over frameSize_
    std::vector<float> windowedFull_; // Hann-windowed full-rate frame
    std::vector<float> power_;        // RealFft power spectrum, bins()
    std::vector<LtasBand> ltasBands_;
    double ltasSum_[32] = {};    // accumulated mean bin power per band

    std::vector<float> vtlValues_; // valid VTLs, up to maxFrames_
    std::vector<float> hnrValues_; // per-frame HNR dB, up to maxFrames_
    mutable std::vector<float> medianScratch_; // for the median selection in summary()

    std::size_t frames_ = 0;     // frames pushed (uncapped)
    std::size_t vtlFrames_ = 0;  // frames with a valid VTL (uncapped)
};

} // namespace dsptb::dsp
