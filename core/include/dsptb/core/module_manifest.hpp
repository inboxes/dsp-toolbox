#pragma once

#include <map>
#include <string>
#include <vector>

namespace dsptb {

struct ManifestModule {
    std::string id;
    std::string kind; // "processor" | "analyzer" | "offline"
};

// module.json (MODULE_SDK §2).
struct ModuleManifest {
    int schema = 0;
    std::string id;
    std::string name;
    std::string version;
    int abi = 0;
    std::string dll;
    std::vector<ManifestModule> modules;
    std::string author;
    std::string license;
    std::string description;
    std::string homepage;
    std::string minHostVersion;
    // Optional per-parameter help text (param id -> sentence), shown as a tooltip beside the control.
    std::map<std::string, std::string> paramHelp;
};

// Lowercase reverse-DNS: [a-z0-9.-], at least one character, no leading or trailing '.' or '-'.
bool isValidModuleId(const std::string& id);
// Parses and validates a manifest. Returns false with a human-readable reason (never throws).
bool parseManifest(const std::string& jsonText, ModuleManifest& out, std::string& error);
// SemVer comparison on the numeric core (major.minor.patch); pre-release tags are ignored. <0, 0, >0.
int compareVersions(const std::string& a, const std::string& b);

} // namespace dsptb
