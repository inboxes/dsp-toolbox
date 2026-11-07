#include "dsptb/core/module_install.hpp"

#include <dsptb/plugin_abi.h>

#include "dsptb/core/version.hpp"

#include <miniz.h>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

namespace dsptb {

namespace fs = std::filesystem;

namespace {

// A real package is far smaller (the Spatializer is about 90 MB); these stop a zip bomb filling the disk.
constexpr uint64_t kMaxTotalBytes = 1ull << 30;
constexpr mz_uint kMaxEntries = 20000;

// Unix file-type bits, stored in the top 16 bits of the external attributes by zip tools on Unix.
constexpr uint32_t kUnixTypeMask = 0170000u;
constexpr uint32_t kUnixSymlink = 0120000u;
constexpr uint32_t kDosReparsePoint = 0x400u; // FILE_ATTRIBUTE_REPARSE_POINT: a Windows symlink or junction

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension, open devices rather than files on
// Windows.
bool isReservedWindowsName(const std::string& segment) {
    const std::string stem = lower(segment.substr(0, segment.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    return stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '1' &&
           stem[3] <= '9';
}

// Splits a zip entry name into safe path segments. False on anything that could leave the extraction folder
// or that Windows would reinterpret: "..", absolute paths, drive letters and streams (any ':'), and names
// ending in a dot or a space (Windows strips them, so ".. " would become "..").
bool safeSegments(std::string name, std::vector<std::string>& out) {
    out.clear();
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos)
        return false;
    std::stringstream parts(name);
    std::string segment;
    while (std::getline(parts, segment, '/')) {
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
            segment.back() == ' ')
            return false;
        if (isReservedWindowsName(segment))
            return false;
        for (const char c : segment)
            if (static_cast<unsigned char>(c) < 32 || std::string("<>\"|?*").find(c) != std::string::npos)
                return false;
        out.push_back(segment);
    }
    return !out.empty();
}

fs::path fromUtf8(const std::string& s) {
    return fs::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

// Closes the zip and the file behind it on every return path.
struct ZipReader {
    mz_zip_archive zip{};
    FILE* file = nullptr;
    bool open = false;
    ~ZipReader() {
        if (open)
            mz_zip_reader_end(&zip);
        if (file)
            std::fclose(file);
    }
};

struct TempFolder {
    fs::path path;
    ~TempFolder() {
        std::error_code ec;
        if (!path.empty())
            fs::remove_all(path, ec);
    }
};

// Checks the name and size of every entry before anything is written. Finds the single module.json.
bool checkEntries(mz_zip_archive& zip, std::string& manifestEntry, std::string& error) {
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kMaxEntries) {
        error = "The zip has too many files.";
        return false;
    }
    uint64_t total = 0;
    int manifests = 0;
    std::set<std::string> seen; // Windows paths ignore case, so two entries could land on the same file
    std::vector<std::string> segments;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            error = "The zip is damaged.";
            return false;
        }
        if (!safeSegments(st.m_filename, segments)) {
            error = std::string("The zip contains an unsafe file path: ") + st.m_filename;
            return false;
        }
        const uint32_t unixType = (st.m_external_attr >> 16) & kUnixTypeMask;
        if (unixType == kUnixSymlink || (st.m_external_attr & kDosReparsePoint)) {
            error = std::string("The zip contains a symbolic link: ") + st.m_filename;
            return false;
        }
        if (st.m_is_encrypted || !st.m_is_supported) {
            error = std::string("The zip contains a file that can't be read: ") + st.m_filename;
            return false;
        }
        total += st.m_uncomp_size;
        if (total > kMaxTotalBytes) {
            error = "The zip is too large when unpacked.";
            return false;
        }
        if (st.m_is_directory)
            continue;
        std::string key;
        for (const std::string& s : segments)
            key += lower(s) + "/";
        if (!seen.insert(key).second) {
            error = std::string("The zip contains a duplicate file: ") + st.m_filename;
            return false;
        }
        if (segments.size() <= 2 && lower(segments.back()) == "module.json") {
            ++manifests;
            manifestEntry = st.m_filename;
            std::replace(manifestEntry.begin(), manifestEntry.end(), '\\', '/');
        }
    }
    if (manifests != 1) {
        error = manifests == 0 ? "The zip has no module.json at its top level or one folder down."
                               : "The zip has more than one module.json.";
        return false;
    }
    return true;
}

bool extractAll(mz_zip_archive& zip, const fs::path& dest, std::string& error) {
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::vector<std::string> segments;
    std::error_code ec;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || !safeSegments(st.m_filename, segments)) {
            error = "The zip is damaged.";
            return false;
        }
        fs::path target = dest;
        for (const std::string& s : segments)
            target /= fromUtf8(s);
        if (st.m_is_directory) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        FILE* out = _wfopen(target.c_str(), L"wb"); // the wide call: the path may not fit the ANSI code page
        if (!out) {
            error = std::string("Could not write ") + st.m_filename;
            return false;
        }
        // Checks the CRC and the declared size, so a false size in the header can't get past the limit above.
        const bool written = mz_zip_reader_extract_to_cfile(&zip, i, out, 0) != MZ_FALSE;
        std::fclose(out);
        if (!written) {
            error = std::string("The zip is damaged at ") + st.m_filename;
            return false;
        }
    }
    return true;
}

bool readTextFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Moves `from` to `to`, replacing what is there. On failure the old folder is put back.
bool replaceFolder(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::path old = to;
    old += ".old";
    fs::remove_all(old, ec);
    const bool hadOld = fs::exists(to, ec);
    if (hadOld) {
        fs::rename(to, old, ec);
        if (ec) {
            error = "Could not replace " + to.filename().string() + ": " + ec.message();
            return false;
        }
    }
    fs::rename(from, to, ec);
    if (ec) {
        error = "Could not move the module into place: " + ec.message();
        std::error_code ignored;
        if (hadOld)
            fs::rename(old, to, ignored);
        return false;
    }
    fs::remove_all(old, ec);
    return true;
}

} // namespace

ModuleInstallResult installModuleZip(const fs::path& zipPath, const fs::path& modulesDir,
                                     const fs::path& tmpRoot,
                                     const std::function<bool(const std::string&)>& isKnown) {
    ModuleInstallResult r;

    ZipReader reader;
    std::error_code ec;
    const uint64_t zipSize = fs::file_size(zipPath, ec);
    if (ec) {
        r.error = "Could not read the zip file.";
        return r;
    }
    reader.file = _wfopen(zipPath.c_str(), L"rb");
    if (!reader.file || !mz_zip_reader_init_cfile(&reader.zip, reader.file, zipSize, 0)) {
        r.error = "This file isn't a valid zip.";
        return r;
    }
    reader.open = true;

    std::string manifestEntry;
    if (!checkEntries(reader.zip, manifestEntry, r.error))
        return r;

    TempFolder temp;
    fs::create_directories(tmpRoot, ec);
    for (int i = 0; i < 1000 && temp.path.empty(); ++i) {
        const fs::path candidate =
            tmpRoot / ("install-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(i));
        if (fs::create_directory(candidate, ec))
            temp.path = candidate;
    }
    if (temp.path.empty()) {
        r.error = "Could not create a temporary folder.";
        return r;
    }
    if (!extractAll(reader.zip, temp.path, r.error))
        return r;

    const fs::path manifestPath = temp.path / fromUtf8(manifestEntry);
    const fs::path package = manifestPath.parent_path();
    std::string text;
    if (!readTextFile(manifestPath, text)) {
        r.error = "Could not read module.json.";
        return r;
    }
    std::string parseError;
    if (!parseManifest(text, r.manifest, parseError)) {
        r.error = "Invalid module.json: " + parseError;
        return r;
    }
    std::vector<std::string> idSegments;
    if (!safeSegments(r.manifest.id, idSegments) || idSegments.size() != 1) {
        r.error = "The module id can't be used as a folder name: " + r.manifest.id;
        return r;
    }
    if (r.manifest.abi != static_cast<int>(DSPTB_ABI_VERSION)) {
        r.error = "This module was built for a different module interface (ABI " +
                  std::to_string(r.manifest.abi) + "; this app has ABI " + std::to_string(DSPTB_ABI_VERSION) +
                  ").";
        return r;
    }
    if (!r.manifest.minHostVersion.empty() &&
        compareVersions(r.manifest.minHostVersion, versionString()) > 0) {
        r.error = "This module needs version " + r.manifest.minHostVersion + " or newer of the app.";
        return r;
    }
    if (!fs::is_regular_file(package / fromUtf8(r.manifest.dll), ec)) {
        r.error = "The zip doesn't contain the module's DLL (" + r.manifest.dll + ").";
        return r;
    }

    r.needsRestart = isKnown(r.manifest.id);
    const fs::path id = fromUtf8(r.manifest.id);
    const fs::path dest = r.needsRestart ? modulesDir / ".pending" / id : modulesDir / id;
    if (!replaceFolder(package, dest, r.error))
        return r;
    r.ok = true;
    return r;
}

size_t applyPendingModuleInstalls(const fs::path& modulesDir) {
    const fs::path pending = modulesDir / ".pending";
    std::error_code ec;
    if (!fs::is_directory(pending, ec))
        return 0;
    std::vector<fs::path> staged;
    for (const auto& e : fs::directory_iterator(pending, ec))
        if (e.is_directory(ec))
            staged.push_back(e.path());
    size_t applied = 0;
    for (const fs::path& package : staged) {
        std::string error;
        if (replaceFolder(package, modulesDir / package.filename(), error)) {
            ++applied;
            spdlog::info("Installed module {}", package.filename().string());
        } else {
            spdlog::error("Module {} stays staged: {}", package.filename().string(), error);
        }
    }
    fs::remove(pending, ec); // only succeeds once it is empty
    return applied;
}

} // namespace dsptb
