#pragma once

#include "module_registry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace dsptb {

struct ValidationCheck {
    std::string name;
    bool passed = true;
    std::string detail;
};

struct ModuleValidation {
    std::string moduleId;
    std::string kind;
    std::vector<ValidationCheck> checks;
    bool passed() const;
};

struct ValidationReport {
    std::string path;
    std::string packageId;
    std::vector<ValidationCheck> packageChecks; // manifest, load, ABI
    std::vector<ModuleValidation> modules;
    uint32_t rtGuardHits = 0;
    bool rtGuardActive = false; // the module exports the guard counter and the host build enables it
    bool passed() const;
    std::string toJson() const;
    std::string toText() const;
};

// The module conformance suite. `target` is a package folder or a module DLL (the folder's
// module.json is used). Loads the DLL into `registry`, so run it in a process that may load modules.
ValidationReport validateModulePackage(ModuleRegistry& registry, const std::filesystem::path& target);

} // namespace dsptb
