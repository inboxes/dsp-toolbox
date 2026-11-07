#pragma once

// Helpers for module state (save_state / load_state). nlohmann/json is header-only; modules that use these
// helpers link nlohmann_json (dsptb_sdk carries it).

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace dsptb {

using Json = nlohmann::json;

// Parses a state string; returns an empty object when the text is not valid JSON (never throws).
inline Json parseState(std::string_view text) {
    Json j = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : Json::object();
}
inline std::string dumpState(const Json& j) {
    return j.dump();
}

} // namespace dsptb
