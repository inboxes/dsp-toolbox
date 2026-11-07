#pragma once

#include "audio_buffer.hpp"

#include <atomic>
#include <filesystem>
#include <string>

namespace dsptb {

enum class WavFormat { F32, S24, S16 };

bool parseWavFormat(const std::string& name, WavFormat& out); // "f32", "s24", "s16"

// Reads WAV, FLAC or MP3 at the file's own channel count and sample rate (no conversion).
bool readPcm(const std::filesystem::path& file, PcmAudio& out, std::string& error);

// Writes a WAV file. S16 optionally gets TPDF dither (deterministic: fixed seed), S24 and F32 never do.
bool writeWav(const std::filesystem::path& file, const PcmAudio& audio, WavFormat format, std::string& error,
              bool dither = false);

// Decodes a file into the engine bus format: stereo, `targetRate` (0 = keep the file's rate).
// Mono is duplicated to both channels; more than two channels are downmixed with ITU-R BS.775-style
// coefficients (centre and surrounds at -3 dB, LFE dropped). Reports progress in [0,1] and honours `cancel`.
// Returns false with an error (or "cancelled").
bool decodeToStereo(const std::filesystem::path& file, uint32_t targetRate, AudioBuffer& out,
                    std::string& error, std::atomic<float>* progress = nullptr,
                    const std::atomic<bool>* cancel = nullptr);

// Converts interleaved PCM of any channel count to the stereo bus format (same rules as decodeToStereo,
// no resampling).
AudioBuffer pcmToStereo(const PcmAudio& pcm);

} // namespace dsptb
