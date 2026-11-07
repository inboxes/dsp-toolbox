#pragma once

#include "audio_buffer.hpp"
#include "module_registry.hpp"
#include "session_file.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dsptb {

// What a session file says about one module instance.
struct InstanceDescription {
    std::string module;
    bool enabled = true;
    std::map<std::string, float> params; // param id -> value (booleans accepted as true/false)
    std::string stateJson;               // module state, a JSON object text; empty = none
};

struct SessionDescription {
    uint32_t sampleRate = 0; // "audio.sampleRate"; 0 = keep the input file's rate
    std::vector<InstanceDescription> instances;
};

// Parses a session JSON text ({"format":"dsptb-session","version":1,"instances":[...]}). Only the instance
// list is read; other sections are ignored. Never throws.
bool parseSessionDescription(const std::string& text, SessionDescription& out, std::string& error);

// The processors of a session file as the renderer wants them (analyzers are left out).
SessionDescription descriptionFromSession(const SessionDoc& doc);

struct RenderOptions {
    uint32_t blockFrames = 512; // determinism: the default block size is 512
    double tailSeconds = 2.0;   // appended after the input, so effect tails are not cut off
    // What modules see as the working file. Null: the input itself. A different file stands in for a backing
    // track, as in Mic + track.
    std::shared_ptr<const AudioBuffer> workingFile;
    // Optional: progress in [0,1] (written after every block) and a cancel flag (checked every block; the
    // render then fails with error "cancelled").
    std::atomic<float>* progress = nullptr;
    const std::atomic<bool>* cancel = nullptr;
};

// Offline renderer shared by Export and `dsptb-cli render`: fresh instances of every
// enabled processor, load_state, parameters, prepare(offline), then the same chain code the engine uses (no
// bypass ramps). The total chain latency is trimmed from the start, so the output is input-length + tail and
// aligned with the input. `input` must be stereo at the session rate.
bool renderOffline(ModuleRegistry& registry, const SessionDescription& session, const AudioBuffer& input,
                   const RenderOptions& options, AudioBuffer& output, std::string& error);

} // namespace dsptb
