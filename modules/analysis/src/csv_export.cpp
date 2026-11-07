#include "csv_export.hpp"

#include <dsptb/branding.h>

#include <ShlObj.h>
#include <Windows.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace dsptb::analysis {
namespace {

// The ABI has no data-directory call, so this follows the host's rule.
std::filesystem::path exportsDir() {
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
    dir /= "exports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

std::string writeCsv(const char* moduleName, const std::string& text) {
    const std::filesystem::path dir = exportsDir();
    if (dir.empty())
        return {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);
    const std::filesystem::path path = dir / (std::string(moduleName) + "-" + stamp + ".csv");
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out)
        return {};
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace dsptb::analysis
