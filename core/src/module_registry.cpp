#include "dsptb/core/module_registry.hpp"

#include "dsptb/core/seh_guard.hpp"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace dsptb {

namespace fs = std::filesystem;

const char* statusLabel(PackageStatus s) {
    switch (s) {
    case PackageStatus::Ok:
        return "OK";
    case PackageStatus::IncompatibleAbi:
        return "Incompatible ABI";
    case PackageStatus::FailedToLoad:
        return "Failed to load";
    case PackageStatus::Faulted:
        return "Faulted";
    }
    return "?";
}

const char* kindName(dsptb_kind k) {
    switch (k) {
    case DSPTB_KIND_PROCESSOR:
        return "processor";
    case DSPTB_KIND_ANALYZER:
        return "analyzer";
    case DSPTB_KIND_OFFLINE:
        return "offline";
    }
    return "?";
}

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string lastErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out = "error " + std::to_string(code);
    if (n && buf) {
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), len, nullptr, nullptr);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '.'))
            s.pop_back();
        out += ": " + s;
    }
    if (buf)
        LocalFree(buf);
    return out;
}

bool snakeCase(const char* s) {
    if (!s || !*s)
        return false;
    for (const char* p = s; *p; ++p) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok)
            return false;
    }
    return *s >= 'a' && *s <= 'z';
}

std::string validateParam(const dsptb_param_desc& p, uint32_t index) {
    const std::string who = "param #" + std::to_string(index);
    if (p.struct_size < sizeof(dsptb_param_desc))
        return who + ": struct_size too small";
    if (!p.id || !p.name || !p.unit)
        return who + ": id, name and unit must be non-null";
    if (!snakeCase(p.id))
        return who + ": id \"" + p.id + "\" is not snake_case";
    switch (p.type) {
    case DSPTB_PARAM_FLOAT:
    case DSPTB_PARAM_INT:
    case DSPTB_PARAM_BOOL:
    case DSPTB_PARAM_ENUM:
        break;
    default:
        return who + " (" + p.id + "): unknown type";
    }
    if (!(p.min_value < p.max_value))
        return who + " (" + p.id + "): min must be below max";
    if (!(p.default_value >= p.min_value && p.default_value <= p.max_value))
        return who + " (" + p.id + "): default outside the range";
    if (p.type == DSPTB_PARAM_BOOL && !(p.min_value == 0.0f && p.max_value == 1.0f))
        return who + " (" + p.id + "): a bool must have range 0..1";
    if (p.type == DSPTB_PARAM_ENUM) {
        if (!p.enum_labels || p.enum_count == 0)
            return who + " (" + p.id + "): enum without labels";
        if (p.min_value != 0.0f || p.max_value != static_cast<float>(p.enum_count - 1))
            return who + " (" + p.id + "): enum range must be 0..labels-1";
        for (uint32_t i = 0; i < p.enum_count; ++i)
            if (!p.enum_labels[i])
                return who + " (" + p.id + "): null enum label";
    }
    if ((p.type == DSPTB_PARAM_FLOAT) && (p.flags & DSPTB_PARAM_LOG) && p.min_value <= 0.0f)
        return who + " (" + p.id + "): a log parameter needs min > 0";
    return {};
}

} // namespace

std::string validateDescriptor(const dsptb_module_desc& d) {
    if (d.struct_size < sizeof(dsptb_module_desc))
        return "descriptor struct_size is smaller than ABI v1 requires";
    if (!d.id || !d.name || !d.version || !d.vendor || !d.description)
        return "descriptor strings (id, name, version, vendor, description) must be non-null";
    if (!isValidModuleId(d.id))
        return std::string("invalid module id \"") + d.id + "\"";
    if (d.kind != DSPTB_KIND_PROCESSOR && d.kind != DSPTB_KIND_ANALYZER && d.kind != DSPTB_KIND_OFFLINE)
        return "invalid module kind";
    if (!d.create || !d.destroy || !d.prepare)
        return "create, destroy and prepare are required";
    switch (d.kind) {
    case DSPTB_KIND_PROCESSOR:
        if (!d.reset || !d.latency_frames || !d.process)
            return "a processor needs reset, latency_frames and process";
        break;
    case DSPTB_KIND_ANALYZER:
        if (!d.analyze)
            return "an analyzer needs analyze";
        break;
    case DSPTB_KIND_OFFLINE:
        if (!d.run_offline)
            return "an offline module needs run_offline";
        break;
    }
    if (d.num_params > 0 && !d.params)
        return "num_params > 0 but params is null";
    if (d.num_params > 256)
        return "too many parameters (max 256)";
    std::set<std::string> seen;
    for (uint32_t i = 0; i < d.num_params; ++i) {
        std::string e = validateParam(d.params[i], i);
        if (!e.empty())
            return e;
        if (!seen.insert(d.params[i].id).second)
            return std::string("duplicate parameter id \"") + d.params[i].id + "\"";
    }
    return {};
}

const dsptb_module_desc* ModulePackage::find(const std::string& moduleId) const {
    for (const auto* d : descs)
        if (moduleId == d->id)
            return d;
    return nullptr;
}

std::string ModulePackage::dllPathUtf8() const {
    return utf8(folder / manifest.dll);
}

// ----------------------------------------------------------------------------------------------------------
// scanning
// ----------------------------------------------------------------------------------------------------------

bool ModuleRegistry::isBetter(const ModulePackage& cand, const ModulePackage& existing) const {
    return compareVersions(cand.manifest.version, existing.manifest.version) > 0;
}

ModulePackage* ModuleRegistry::addPackage(const fs::path& folder, std::string& error) {
    std::ifstream in(folder / "module.json", std::ios::binary);
    if (!in) {
        error = "no module.json in " + utf8(folder);
        return nullptr;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    auto pkg = std::make_unique<ModulePackage>();
    // LoadLibraryExW with LOAD_LIBRARY_SEARCH_* flags needs an absolute path.
    std::error_code absEc;
    pkg->folder = fs::absolute(folder, absEc);
    if (absEc)
        pkg->folder = folder;
    if (!parseManifest(ss.str(), pkg->manifest, error)) {
        error = "invalid module.json in " + utf8(folder) + ": " + error;
        return nullptr;
    }
    if (pkg->manifest.abi != static_cast<int>(DSPTB_ABI_VERSION)) {
        pkg->status = PackageStatus::IncompatibleAbi;
        pkg->statusText = "built for plugin ABI " + std::to_string(pkg->manifest.abi) +
                          ", this host speaks " + std::to_string(DSPTB_ABI_VERSION);
    } else if (!fs::exists(folder / pkg->manifest.dll)) {
        pkg->status = PackageStatus::FailedToLoad;
        pkg->statusText = pkg->manifest.dll + " is missing from the package folder";
    }

    if (ModulePackage* dup = findPackage(pkg->manifest.id)) {
        if (isBetter(*pkg, *dup) && !dup->loaded) {
            spdlog::info("module {}: {} replaces {} (newer version)", pkg->manifest.id,
                         utf8(pkg->folder), utf8(dup->folder));
            *dup = std::move(*pkg);
            return dup;
        }
        spdlog::info("module {}: ignoring {} (conflicts with {})", pkg->manifest.id, utf8(folder),
                     utf8(dup->folder));
        error = "duplicate module id " + pkg->manifest.id;
        return nullptr;
    }
    packages_.push_back(std::move(pkg));
    return packages_.back().get();
}

size_t ModuleRegistry::scan(const std::vector<fs::path>& folders) {
    size_t added = 0;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        if (!fs::is_directory(folder, ec))
            continue;
        std::vector<fs::path> children;
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (e.is_directory(ec) && fs::exists(e.path() / "module.json", ec))
                children.push_back(e.path());
        std::sort(children.begin(), children.end());
        for (const fs::path& dir : children) {
            const bool known = std::any_of(packages_.begin(), packages_.end(),
                                           [&](const auto& p) { return p->folder == dir; });
            if (known)
                continue;
            std::string error;
            const size_t before = packages_.size();
            ModulePackage* p = addPackage(dir, error);
            if (p && packages_.size() > before)
                ++added;
            else if (!p && error.rfind("duplicate", 0) != 0)
                spdlog::warn("module scan: {}", error);
        }
    }
    return added;
}

ModulePackage* ModuleRegistry::findPackage(const std::string& packageId) {
    for (auto& p : packages_)
        if (p->manifest.id == packageId)
            return p.get();
    return nullptr;
}

ModulePackage* ModuleRegistry::findByModule(const std::string& moduleId) {
    for (auto& p : packages_)
        for (const auto& m : p->manifest.modules)
            if (m.id == moduleId)
                return p.get();
    return nullptr;
}

// ----------------------------------------------------------------------------------------------------------
// loading
// ----------------------------------------------------------------------------------------------------------

namespace {

struct ExportCall {
    uint32_t (*count)(void) = nullptr;
    const dsptb_module_desc* (*get)(uint32_t, uint32_t) = nullptr;
    uint32_t n = 0;
    std::vector<const dsptb_module_desc*> out;
    bool refused = false;
};

} // namespace

bool ModuleRegistry::ensureLoaded(ModulePackage& pkg) {
    if (pkg.loaded)
        return pkg.status == PackageStatus::Ok || pkg.status == PackageStatus::Faulted;
    if (pkg.status == PackageStatus::IncompatibleAbi || pkg.status == PackageStatus::FailedToLoad)
        return false;

    const fs::path dll = pkg.folder / pkg.manifest.dll;
    HMODULE h = LoadLibraryExW(dll.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
        pkg.status = PackageStatus::FailedToLoad;
        pkg.statusText = lastErrorText(GetLastError());
        spdlog::warn("module {}: LoadLibrary failed: {}", pkg.manifest.id, pkg.statusText);
        return false;
    }

    const auto fail = [&](PackageStatus st, std::string why) {
        FreeLibrary(h); // never instantiated, so nothing refers into it
        pkg.status = st;
        pkg.statusText = std::move(why);
        spdlog::warn("module {}: {}: {}", pkg.manifest.id, statusLabel(st), pkg.statusText);
        return false;
    };

    ExportCall call;
    call.count = reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(h, "dsptb_module_count"));
    call.get = reinterpret_cast<const dsptb_module_desc* (*)(uint32_t, uint32_t)>(
        GetProcAddress(h, "dsptb_module_get"));
    if (!call.count || !call.get)
        return fail(PackageStatus::FailedToLoad, "missing export dsptb_module_count / dsptb_module_get");

    // The DLL is foreign code: ask for its descriptors through the SEH guard.
    const bool ok = guarded([&] {
        call.n = call.count();
        for (uint32_t i = 0; i < call.n && i < 64; ++i) {
            const dsptb_module_desc* d = call.get(i, DSPTB_ABI_VERSION);
            if (!d) {
                call.refused = true;
                return;
            }
            call.out.push_back(d);
        }
    });
    if (!ok)
        return fail(PackageStatus::FailedToLoad, "the module crashed while reporting its descriptors");
    if (call.refused)
        return fail(PackageStatus::IncompatibleAbi,
                    "the module refused plugin ABI " + std::to_string(DSPTB_ABI_VERSION));
    if (call.out.empty())
        return fail(PackageStatus::FailedToLoad, "the DLL exports no modules");

    for (const dsptb_module_desc* d : call.out) {
        if (d->struct_size < sizeof(uint32_t) * 2 || d->abi_version != DSPTB_ABI_VERSION)
            return fail(PackageStatus::IncompatibleAbi,
                        "descriptor reports ABI version " + std::to_string(d->abi_version) +
                            ", host speaks " + std::to_string(DSPTB_ABI_VERSION));
        std::string e = validateDescriptor(*d);
        if (!e.empty())
            return fail(PackageStatus::FailedToLoad, "invalid descriptor: " + e);
    }

    // The manifest and the DLL must describe the same modules.
    for (const ManifestModule& mm : pkg.manifest.modules) {
        const auto it = std::find_if(call.out.begin(), call.out.end(),
                                     [&](const dsptb_module_desc* d) { return mm.id == d->id; });
        if (it == call.out.end())
            return fail(PackageStatus::FailedToLoad, "module.json lists " + mm.id + " but the DLL does not");
        if (mm.kind != kindName((*it)->kind))
            return fail(PackageStatus::FailedToLoad, "module.json says " + mm.id + " is a " + mm.kind +
                                                         " but the DLL says " + kindName((*it)->kind));
    }
    for (const dsptb_module_desc* d : call.out) {
        const bool listed = std::any_of(pkg.manifest.modules.begin(), pkg.manifest.modules.end(),
                                        [&](const ManifestModule& m) { return m.id == d->id; });
        if (!listed)
            return fail(PackageStatus::FailedToLoad,
                        std::string("the DLL exports ") + d->id + " which module.json does not list");
    }

    pkg.handle = h;
    pkg.descs = std::move(call.out);
    pkg.guardViolations =
        reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(h, "dsptb_rt_guard_violations"));
    pkg.guardReset = reinterpret_cast<void (*)(void)>(GetProcAddress(h, "dsptb_rt_guard_reset"));
    pkg.dumpFrames = reinterpret_cast<int (*)(void*, void (*)(void*, const char*, size_t), void*)>(
        GetProcAddress(h, "dsptb_dump_frames"));
    pkg.loaded = true;
    return true;
}

const dsptb_module_desc* ModuleRegistry::descriptor(const std::string& moduleId, ModulePackage** pkgOut,
                                                    std::string& error) {
    ModulePackage* pkg = findByModule(moduleId);
    if (!pkg) {
        error = "module " + moduleId + " is not installed";
        return nullptr;
    }
    if (pkg->status == PackageStatus::Faulted) {
        error = "module " + moduleId + " faulted earlier in this run (" + pkg->statusText +
                "); restart the app to try again";
        return nullptr;
    }
    if (!ensureLoaded(*pkg)) {
        error = std::string(statusLabel(pkg->status)) + ": " + pkg->statusText;
        return nullptr;
    }
    const dsptb_module_desc* d = pkg->find(moduleId);
    if (!d) {
        error = "module " + moduleId + " is not exported by " + pkg->manifest.dll;
        return nullptr;
    }
    if (pkgOut)
        *pkgOut = pkg;
    return d;
}

void ModuleRegistry::markFaulted(const std::string& moduleId, const std::string& reason) {
    if (ModulePackage* pkg = findByModule(moduleId)) {
        pkg->status = PackageStatus::Faulted;
        pkg->statusText = reason;
    }
}

} // namespace dsptb
