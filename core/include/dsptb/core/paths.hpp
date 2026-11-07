#pragma once

#include <filesystem>
#include <string>

namespace dsptb {

// Per-user data directory: %LOCALAPPDATA%/<DSPTB_DATA_DIR_NAME>. The environment variable DSPTB_DATA_DIR
// overrides it (used by tests and CI so they never touch the real profile). The directory is created.
std::filesystem::path dataDir();

// The folder holding the running executable. Empty if it can't be worked out.
std::filesystem::path appDir();

// <appDir>/modules: the one place modules live, bundled or installed. Not created here.
std::filesystem::path modulesDir();

// <dataDir>/logs, created on demand.
std::filesystem::path logDir();

// Absolute path to the Windows fonts folder (usually C:\Windows\Fonts); empty if it cannot be resolved.
std::filesystem::path systemFontsDir();

inline std::filesystem::path fromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// The path as UTF-8 text. `path::string()` on Windows converts to the ANSI code page, which turns CJK and
// other non-Latin characters into '?'; anything shown in the UI or logged must go through this.
inline std::string toUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace dsptb
