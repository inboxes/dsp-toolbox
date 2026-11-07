#include "dsptb/core/audio_io.hpp"
#include "dsptb/core/paths.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsptb {

namespace {

constexpr float kMinus3dB = 0.70710678f;

struct Decoder {
    ma_decoder dec{};
    bool open = false;
    ~Decoder() {
        if (open)
            ma_decoder_uninit(&dec);
    }
};

bool openDecoder(Decoder& d, const std::filesystem::path& file, ma_uint32 channels, ma_uint32 rate,
                 std::string& error) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, channels, rate);
    const ma_result r = ma_decoder_init_file_w(file.c_str(), &cfg, &d.dec);
    if (r != MA_SUCCESS) {
        error = std::string("cannot decode '") + toUtf8(file) + "': " + ma_result_description(r);
        return false;
    }
    d.open = true;
    return true;
}

// Mixes one interleaved frame of `channels` channels to stereo. Channel order follows WAV/SMPTE: FL FR FC LFE
// then surrounds in pairs (BL BR, SL SR).
void downmixFrame(const float* in, uint32_t channels, float& l, float& r) {
    l = in[0];
    r = in[1];
    if (channels <= 2)
        return;
    const bool hasLfe = channels >= 6;
    l += kMinus3dB * in[2];
    r += kMinus3dB * in[2];
    uint32_t first = hasLfe ? 4 : 3; // skip the LFE channel
    for (uint32_t c = first; c < channels; ++c) {
        const bool leftSide = ((c - first) % 2) == 0;
        (leftSide ? l : r) += kMinus3dB * in[c];
    }
}

float peakNormalise(float v) {
    return std::clamp(v, -1.0f, 1.0f);
}

} // namespace

bool parseWavFormat(const std::string& name, WavFormat& out) {
    if (name == "f32")
        out = WavFormat::F32;
    else if (name == "s24")
        out = WavFormat::S24;
    else if (name == "s16")
        out = WavFormat::S16;
    else
        return false;
    return true;
}

WaveformOverview computeOverview(const AudioBuffer& audio, size_t columns) {
    WaveformOverview o;
    const size_t frames = audio.frames();
    if (frames == 0 || columns == 0)
        return o;
    columns = std::min(columns, frames);
    o.framesPerColumn = (frames + columns - 1) / columns;
    const size_t cols = (frames + o.framesPerColumn - 1) / o.framesPerColumn;
    o.minimum.assign(cols, 0.0f);
    o.maximum.assign(cols, 0.0f);
    for (size_t c = 0; c < cols; ++c) {
        const size_t begin = c * o.framesPerColumn;
        const size_t end = std::min(frames, begin + o.framesPerColumn);
        float lo = 1e9f, hi = -1e9f;
        for (size_t i = begin; i < end; ++i) {
            const float m = 0.5f * (audio.ch[0][i] + audio.ch[1][i]);
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        o.minimum[c] = lo;
        o.maximum[c] = hi;
    }
    return o;
}

bool readPcm(const std::filesystem::path& file, PcmAudio& out, std::string& error) {
    Decoder d;
    if (!openDecoder(d, file, 0, 0, error))
        return false; // 0 = keep the file's channels and rate
    out = PcmAudio{};
    out.channels = d.dec.outputChannels;
    out.sampleRate = d.dec.outputSampleRate;
    constexpr ma_uint64 kChunk = 4096;
    std::vector<float> chunk(static_cast<size_t>(kChunk) * out.channels);
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&d.dec, chunk.data(), kChunk, &got);
        out.samples.insert(out.samples.end(), chunk.begin(),
                           chunk.begin() + static_cast<size_t>(got) * out.channels);
        if (r != MA_SUCCESS || got < kChunk)
            break;
    }
    return true;
}

namespace {

// Small deterministic PRNG for the dither, so identical input gives identical output files.
struct Rng {
    uint32_t s = 0x1234567u;
    float uniform() { // [0,1)
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
    }
};

} // namespace

bool writeWav(const std::filesystem::path& file, const PcmAudio& audio, WavFormat format, std::string& error,
              bool dither) {
    if (audio.channels == 0 || audio.sampleRate == 0) {
        error = "nothing to write";
        return false;
    }
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    const ma_format maFormat = format == WavFormat::F32   ? ma_format_f32
                               : format == WavFormat::S24 ? ma_format_s24
                                                          : ma_format_s16;
    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, maFormat, audio.channels, audio.sampleRate);
    ma_encoder enc;
    if (ma_encoder_init_file_w(file.c_str(), &cfg, &enc) != MA_SUCCESS) {
        error = "cannot create '" + toUtf8(file) + "'";
        return false;
    }

    bool ok = true;
    const size_t total = audio.samples.size();
    if (format == WavFormat::F32) {
        ma_uint64 written = 0;
        ok =
            ma_encoder_write_pcm_frames(&enc, audio.samples.data(), audio.frames(), &written) == MA_SUCCESS &&
            written == audio.frames();
    } else {
        // Convert in chunks to the integer format, then hand to the encoder.
        constexpr size_t kChunkSamples = 8192 - (8192 % 8); // multiple of any channel count up to 8
        std::vector<uint8_t> bytes;
        Rng rng;
        for (size_t pos = 0; pos < total && ok;) {
            const size_t n = std::min(kChunkSamples - (kChunkSamples % audio.channels), total - pos);
            bytes.resize(n * (format == WavFormat::S24 ? 3 : 2));
            for (size_t i = 0; i < n; ++i) {
                const float v = peakNormalise(audio.samples[pos + i]);
                if (format == WavFormat::S16) {
                    float scaled = v * 32767.0f;
                    if (dither)
                        scaled += rng.uniform() - rng.uniform(); // TPDF, +-1 LSB
                    const int s = static_cast<int>(std::lround(std::clamp(scaled, -32768.0f, 32767.0f)));
                    const auto u = static_cast<uint16_t>(static_cast<int16_t>(s));
                    bytes[i * 2] = static_cast<uint8_t>(u & 0xff);
                    bytes[i * 2 + 1] = static_cast<uint8_t>(u >> 8);
                } else {
                    const int s = static_cast<int>(std::lround(static_cast<double>(v) * 8388607.0));
                    bytes[i * 3] = static_cast<uint8_t>(s & 0xff);
                    bytes[i * 3 + 1] = static_cast<uint8_t>((s >> 8) & 0xff);
                    bytes[i * 3 + 2] = static_cast<uint8_t>((s >> 16) & 0xff);
                }
            }
            ma_uint64 written = 0;
            ok =
                ma_encoder_write_pcm_frames(&enc, bytes.data(), n / audio.channels, &written) == MA_SUCCESS &&
                written == n / audio.channels;
            pos += n;
        }
    }
    ma_encoder_uninit(&enc);
    if (!ok)
        error = "write failed for '" + toUtf8(file) + "'";
    return ok;
}

AudioBuffer pcmToStereo(const PcmAudio& pcm) {
    AudioBuffer out;
    out.sampleRate = pcm.sampleRate;
    const size_t frames = pcm.frames();
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const float* in = &pcm.samples[i * pcm.channels];
        if (pcm.channels == 1) {
            out.ch[0][i] = out.ch[1][i] = in[0];
        } else {
            downmixFrame(in, pcm.channels, out.ch[0][i], out.ch[1][i]);
        }
    }
    return out;
}

bool decodeToStereo(const std::filesystem::path& file, uint32_t targetRate, AudioBuffer& out,
                    std::string& error, std::atomic<float>* progress, const std::atomic<bool>* cancel) {
    // Probe the channel count first: <= 2 channels let miniaudio do channel conversion and resampling in one
    // pass; more channels are decoded natively (resampled) and downmixed here with our own coefficients.
    ma_uint32 nativeChannels = 0;
    {
        Decoder probe;
        if (!openDecoder(probe, file, 0, 0, error))
            return false;
        nativeChannels = probe.dec.outputChannels;
        if (targetRate == 0)
            targetRate = probe.dec.outputSampleRate;
    }
    const ma_uint32 decodeChannels = nativeChannels <= 2 ? 2 : nativeChannels;

    Decoder d;
    if (!openDecoder(d, file, decodeChannels, targetRate, error))
        return false;
    ma_uint64 expected = 0;
    ma_decoder_get_length_in_pcm_frames(&d.dec, &expected);

    out = AudioBuffer{};
    out.sampleRate = targetRate;
    if (expected > 0) {
        out.ch[0].reserve(static_cast<size_t>(expected));
        out.ch[1].reserve(static_cast<size_t>(expected));
    }
    constexpr ma_uint64 kChunk = 8192;
    std::vector<float> chunk(static_cast<size_t>(kChunk) * decodeChannels);
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&d.dec, chunk.data(), kChunk, &got);
        for (ma_uint64 i = 0; i < got; ++i) {
            const float* in = &chunk[static_cast<size_t>(i) * decodeChannels];
            float l, rr;
            if (decodeChannels == 2) {
                l = in[0];
                rr = in[1];
            } else {
                downmixFrame(in, decodeChannels, l, rr);
            }
            out.ch[0].push_back(l);
            out.ch[1].push_back(rr);
        }
        if (progress && expected > 0)
            progress->store(std::min(1.0f, static_cast<float>(out.frames()) / static_cast<float>(expected)),
                            std::memory_order_relaxed);
        if (r != MA_SUCCESS || got < kChunk)
            break;
    }
    if (progress)
        progress->store(1.0f, std::memory_order_relaxed);
    if (out.frames() == 0) {
        error = "the file contains no audio";
        return false;
    }
    return true;
}

} // namespace dsptb
