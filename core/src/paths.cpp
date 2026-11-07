#include "dsptb/core/paths.hpp"

#include <dsptb/branding.h>

#include <ShlObj.h>
#include <Windows.h>

#include <iterator>
#include <system_error>

namespace dsptb {

namespace {

std::filesystem::path knownFolder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw)
        result = raw;
    CoTaskMemFree(raw);
    return result;
}

} // namespace

std::filesystem::path dataDir() {
    std::filesystem::path dir;
    // GetEnvironmentVariableW (not getenv) so changes made at runtime by SetEnvironmentVariableW are seen.
    wchar_t over[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"DSPTB_DATA_DIR", over, static_cast<DWORD>(std::size(over)));
    if (n > 0 && n < std::size(over))
        dir = over;
    if (dir.empty()) {
        dir = knownFolder(FOLDERID_LocalAppData);
        if (dir.empty())
            dir = std::filesystem::temp_directory_path();
        dir /= DSPTB_DATA_DIR_NAME;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path appDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) // failed, or the path was truncated
        return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::filesystem::path modulesDir() {
    return appDir() / "modules";
}

std::filesystem::path logDir() {
    auto dir = dataDir() / "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path systemFontsDir() {
    return knownFolder(FOLDERID_Fonts);
}

} // namespace dsptb
