#pragma once

// Where the Spatializer finds user HRTF sets. It stays in its own file so Windows.h stays out of the module's
// main source.
#include <filesystem>
#include <string>

namespace dsptb::spat {

// <data dir>/hrtf, created if missing. The plugin ABI has no data-directory call, so this follows the host's
// rule (core/src/paths.cpp): DSPTB_DATA_DIR if set, otherwise %LOCALAPPDATA%/<DSPTB_DATA_DIR_NAME>.
std::filesystem::path userHrtfDir();

// A path or file name as UTF-8, which is what session JSON and the UI expect.
std::string utf8(const std::filesystem::path& p);

// A path in the form Steam Audio's file loader opens (the ANSI code page). Prefers the 8.3 short name, which
// is plain ASCII. Empty if the path has no ANSI form.
std::string narrowPath(const std::filesystem::path& p);

} // namespace dsptb::spat
