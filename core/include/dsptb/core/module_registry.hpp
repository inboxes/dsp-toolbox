#pragma once

#include "module_manifest.hpp"

#include <dsptb/plugin_abi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dsptb {

enum class PackageStatus { Ok, IncompatibleAbi, FailedToLoad, Faulted };

const char* statusLabel(PackageStatus s); // "OK", "Incompatible ABI", "Failed to load", "Faulted"

// One module package folder (MODULE_SDK §2). Created by a scan without loading the DLL; the DLL is loaded the
// first time a module from it is needed and then stays loaded until exit.
struct ModulePackage {
    std::filesystem::path folder;
    ModuleManifest manifest;
    PackageStatus status = PackageStatus::Ok;
    std::string statusText; // the reason when status != Ok

    bool loaded = false;
    void* handle = nullptr; // HMODULE
    std::vector<const dsptb_module_desc*> descs;
    uint32_t (*guardViolations)(void) = nullptr; // optional exports, see sdk.hpp
    void (*guardReset)(void) = nullptr;
    // Optional: JSON of the frames an analyzer has produced (`analyze --dump-frames`). 0 = written.
    int (*dumpFrames)(void* self, void (*write)(void*, const char*, size_t), void* tok) = nullptr;

    const dsptb_module_desc* find(const std::string& moduleId) const;
    std::string dllPathUtf8() const;
};

// Validates a descriptor read from a DLL against the module rules. Returns an
// empty string when it is fine.
std::string validateDescriptor(const dsptb_module_desc& d);
const char* kindName(dsptb_kind k); // "processor" | "analyzer" | "offline"

// UI-thread catalogue of module packages.
class ModuleRegistry {
public:
    // Scans each folder (every subfolder holding a module.json is a package) and adds packages not yet known
    // (by folder). Returns how many packages were added. A package whose module id is already known is a
    // conflict: the higher version wins, then the one found first; the loser is logged.
    size_t scan(const std::vector<std::filesystem::path>& folders);

    // Adds one package folder directly (used by `dsptb-cli validate`). Returns null with `error` on a folder
    // without a readable module.json.
    ModulePackage* addPackage(const std::filesystem::path& folder, std::string& error);

    const std::vector<std::unique_ptr<ModulePackage>>& packages() const { return packages_; }
    ModulePackage* findPackage(const std::string& packageId);
    // Finds the package that provides module `moduleId` (matching the manifest's module list).
    ModulePackage* findByModule(const std::string& moduleId);

    // Loads the DLL (once), checks the ABI and validates the descriptors against the manifest. On failure the
    // package status says why. Returns true when the package is usable.
    bool ensureLoaded(ModulePackage& pkg);

    // Descriptor of a module, loading its package if needed. Null with `error` set when unavailable.
    const dsptb_module_desc* descriptor(const std::string& moduleId, ModulePackage** pkgOut,
                                        std::string& error);

    void markFaulted(const std::string& moduleId, const std::string& reason);

private:
    bool isBetter(const ModulePackage& cand, const ModulePackage& existing) const;
    std::vector<std::unique_ptr<ModulePackage>> packages_;
};

} // namespace dsptb
