#include "user_dirs.hpp"

#include <dsptb/branding.h>

#include <ShlObj.h>
#include <Windows.h>

#include <iterator>
#include <system_error>

namespace dsptb::spat {

std::filesystem::path userHrtfDir() {
    std::filesystem::path dir;
    wchar_t over[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"DSPTB_DATA_DIR", over, static_cast<DWORD>(std::size(over)));
    if (n > 0 && n < std::size(over))
        dir = over;
    if (dir.empty()) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) && raw)
            dir = std::filesystem::path(raw) / DSPTB_DATA_DIR_NAME;
        CoTaskMemFree(raw);
    }
    if (dir.empty())
        return {};
    dir /= "hrtf";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string narrowPath(const std::filesystem::path& p) {
    wchar_t shortName[MAX_PATH * 2];
    const DWORD n = GetShortPathNameW(p.c_str(), shortName, static_cast<DWORD>(std::size(shortName)));
    const std::filesystem::path chosen =
        n > 0 && n < std::size(shortName) ? std::filesystem::path(shortName) : p;
    try {
        return chosen.string();
    } catch (const std::system_error&) { // a character the ANSI code page can't represent
        return {};
    }
}

} // namespace dsptb::spat
