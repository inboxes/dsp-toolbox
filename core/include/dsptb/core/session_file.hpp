#pragma once

// The session file: plain data, JSON in and out. No audio objects here, so
// the CLI, the tests and the app share one parser and one writer. `SessionModel` and `AudioSession` convert
// to and from this (session_io.hpp).

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace dsptb {

struct SessionInstanceDoc {
    std::string instanceId; // "i1", "i2", ... by position; ids are not kept across a save
    std::string module;
    std::string moduleVersion;
    bool enabled = true;
    std::string role;                     // "processor", "offline" or "analyzer" ("" = unknown)
    std::string tap;                      // analyzers: "pre" or "post"; "" = null
    std::string label;                    // user-chosen name; "" = the module's own
    std::map<std::string, double> params; // param id -> value, ordered so files are stable
    std::string stateJson;                // opaque module state, a JSON object text; "" = none
    bool panelOpen = true;
};

struct SessionSourceDoc {
    std::string path;         // absolute, UTF-8
    std::string relativePath; // relative to the session file, '/' separators; "" = none
    bool loopEnabled = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    bool monitorMic = false;
};

// Exactly one input has the route "chain". gainDb applies to the "output" route.
struct SessionInputDoc {
    std::string route = "off"; // "chain", "output" or "off"
    double gainDb = 0.0;
};

struct SessionInputsDoc {
    SessionInputDoc file{"chain"};
    SessionInputDoc mic;
};

struct SessionAudioDoc {
    std::string backend = "wasapi-shared";
    std::string input, output; // device ids, "" = default
    uint32_t sampleRate = 48000;
    uint32_t bufferFrames = 256;
};

struct SessionDoc {
    static constexpr int kVersion = 2; // 2: "inputs" replaced "source.type"
    std::string appName, appVersion;
    SessionAudioDoc audio;
    SessionSourceDoc source;
    SessionInputsDoc inputs;
    double masterGainDb = 0.0;
    std::vector<SessionInstanceDoc> instances; // chain order for processors; analyzers in the order added
    std::string layout;                        // ImGui ini text
    // Set by the parser, not written: true when the file had an "audio" section (the minimal files `render`
    // accepted by older versions do not).
    bool hasAudio = false;
};

// Serialises the document. Output is deterministic (keys sorted, 2-space indent, trailing newline), so
// save -> load -> save gives identical bytes.
std::string sessionToJson(const SessionDoc& doc);

// Parses a session. Lenient: unknown keys and parameters are kept or ignored, missing ones take
// defaults. Fails (false, `error`) only when the text is not a dsptb-session object, the version is newer
// than this build understands, or an instance has no module id. Never throws.
bool sessionFromJson(const std::string& text, SessionDoc& out, std::string& error);

// Fills `source.relativePath` for the path as seen from the session file's folder (empty if not possible,
// for example another drive).
std::string relativeToSession(const std::filesystem::path& sessionFile, const std::filesystem::path& audio);

// Where the audio file is: the absolute path if it exists, else the relative path next to the session file.
// Empty when neither exists (the UI then asks the user to locate it).
std::filesystem::path resolveSessionAudio(const std::filesystem::path& sessionFile,
                                          const SessionSourceDoc& source);

// Atomic write: <path>.tmp, then replace.
bool writeSessionFile(const std::filesystem::path& file, const SessionDoc& doc, std::string& error);
bool readSessionFile(const std::filesystem::path& file, SessionDoc& out, std::string& error);

// Writes `bytes` to `file` atomically; shared with the autosave.
bool writeFileAtomic(const std::filesystem::path& file, const std::string& bytes, std::string& error);

} // namespace dsptb
