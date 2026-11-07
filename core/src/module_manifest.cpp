#include "dsptb/core/module_manifest.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>

namespace dsptb {

bool isValidModuleId(const std::string& id) {
    if (id.empty() || id.size() > 128)
        return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok)
            return false;
    }
    const auto edge = [](char c) { return c == '.' || c == '-'; };
    return !edge(id.front()) && !edge(id.back());
}

int compareVersions(const std::string& a, const std::string& b) {
    const auto parse = [](const std::string& s, long out[3]) {
        out[0] = out[1] = out[2] = 0;
        const char* p = s.c_str();
        for (int i = 0; i < 3 && *p; ++i) {
            char* end = nullptr;
            out[i] = std::strtol(p, &end, 10);
            if (end == p)
                break;
            p = end;
            if (*p == '.')
                ++p;
            else
                break;
        }
    };
    long x[3], y[3];
    parse(a, x);
    parse(b, y);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

namespace {

bool validKind(const std::string& k) {
    return k == "processor" || k == "analyzer" || k == "offline";
}

// The DLL name is a file inside the package folder, never a path.
bool validDllName(const std::string& n) {
    if (n.empty() || n.find('/') != std::string::npos || n.find('\\') != std::string::npos ||
        n.find("..") != std::string::npos || n.find(':') != std::string::npos)
        return false;
    return n.size() > 4 && (n.substr(n.size() - 4) == ".dll" || n.substr(n.size() - 4) == ".DLL");
}

std::string str(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

bool parseManifest(const std::string& jsonText, ModuleManifest& out, std::string& error) {
    out = ModuleManifest{};
    nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        error = "module.json is not a JSON object";
        return false;
    }
    if (!j.contains("schema") || !j["schema"].is_number_integer() || j["schema"].get<int>() != 1) {
        error = "unsupported manifest schema (expected 1)";
        return false;
    }
    out.schema = 1;
    out.id = str(j, "id");
    if (!isValidModuleId(out.id)) {
        error = "invalid module id \"" + out.id + "\" (lowercase letters, digits, '.' and '-')";
        return false;
    }
    out.name = str(j, "name");
    out.version = str(j, "version");
    if (out.version.empty()) {
        error = "missing version";
        return false;
    }
    if (!j.contains("abi") || !j["abi"].is_number_integer()) {
        error = "missing abi version";
        return false;
    }
    out.abi = j["abi"].get<int>();
    out.dll = str(j, "dll");
    if (!validDllName(out.dll)) {
        error = "invalid dll name \"" + out.dll + "\"";
        return false;
    }
    if (!j.contains("modules") || !j["modules"].is_array() || j["modules"].empty()) {
        error = "modules list is missing or empty";
        return false;
    }
    for (const auto& m : j["modules"]) {
        ManifestModule mm;
        mm.id = m.is_object() ? str(m, "id") : std::string();
        mm.kind = m.is_object() ? str(m, "kind") : std::string();
        mm.name = m.is_object() ? str(m, "name") : std::string();
        if (!isValidModuleId(mm.id)) {
            error = "invalid module id in modules list";
            return false;
        }
        if (!validKind(mm.kind)) {
            error = "module \"" + mm.id + "\" has an invalid kind \"" + mm.kind + "\"";
            return false;
        }
        out.modules.push_back(std::move(mm));
    }
    if (out.name.empty())
        out.name = out.id;
    out.author = str(j, "author");
    out.license = str(j, "license");
    out.description = str(j, "description");
    out.homepage = str(j, "homepage");
    out.minHostVersion = str(j, "minHostVersion");
    if (j.contains("paramHelp") && j["paramHelp"].is_object())
        for (const auto& [key, value] : j["paramHelp"].items())
            if (value.is_string())
                out.paramHelp[key] = value.get<std::string>();
    return true;
}

} // namespace dsptb
