#pragma once

// Glue between the session file (plain data) and the live objects: AudioSession (device, source, loop) and
// SessionModel (the instances). UI-agnostic; the app adds the window layout on top.

#include "audio_session.hpp"
#include "session_file.hpp"
#include "session_model.hpp"

#include <filesystem>
#include <vector>

namespace dsptb {

// The current state as a document. `sessionFile` (may be empty) is where it will be saved, to compute the
// audio file's relative path. The layout is left empty for the caller to fill in.
SessionDoc captureSession(const AudioSession& audio, const SessionModel& model,
                          const std::filesystem::path& sessionFile);

struct ApplyResult {
    // The audio file the session wants, when it names one: either found (loading started) or not.
    bool wantsFile = false;
    bool fileMissing = false;
    std::string missingPath; // as saved, for the "locate" prompt
};

// Replaces the live state with `doc`: audio configuration (falls back with a banner when the device cannot be
// opened), source, loop, master gain, and the instances (see SessionModel::restore). Starts the file load;
// the caller keeps ticking AudioSession as usual.
ApplyResult applySession(const SessionDoc& doc, const std::filesystem::path& sessionFile, AudioSession& audio,
                         SessionModel& model, std::vector<SessionMessage>& messages);

// An empty session: no instances, no file, defaults for loop and master gain.
void resetSession(AudioSession& audio, SessionModel& model);

} // namespace dsptb
