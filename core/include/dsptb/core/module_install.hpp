#pragma once

#include "module_manifest.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace dsptb {

struct ModuleInstallResult {
    bool ok = false;
    bool needsRestart = false; // staged in <modules>/.pending/, swapped in at the next launch
    ModuleManifest manifest;   // valid when ok
    std::string error;         // the reason when !ok
};

// Installs a module package from a zip. Only a temporary folder inside `modulesDir` is written until every
// entry name, the manifest and the DLL check out. It's inside because the package is moved into place by
// renaming, which can't cross drives. `isKnown` says whether this process already knows a package with that
// id: its DLL may be loaded and its manifest cached, so the new files must wait for a restart.
ModuleInstallResult installModuleZip(const std::filesystem::path& zipPath,
                                     const std::filesystem::path& modulesDir,
                                     const std::function<bool(const std::string&)>& isKnown);

// Swaps in the packages staged by an earlier install. Call before scanning for modules. Returns how many were
// applied; a package that can't be swapped stays staged and is logged.
size_t applyPendingModuleInstalls(const std::filesystem::path& modulesDir);

} // namespace dsptb
