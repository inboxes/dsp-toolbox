#pragma once

// Parameter descriptor builders (MODULE_SDK §5). Everything is constexpr, so a module's parameter table
// lives in static storage and its address stays valid for the life of the DLL.

#include "plugin_abi.h"

#include <array>
#include <cstddef>

namespace dsptb {

struct ParamSpec {
    dsptb_param_desc desc{};

    constexpr ParamSpec smoothing(float ms) const {
        ParamSpec c = *this;
        c.desc.smoothing_ms = ms;
        return c;
    }
    constexpr ParamSpec log() const {
        ParamSpec c = *this;
        c.desc.flags |= DSPTB_PARAM_LOG;
        return c;
    }
    constexpr ParamSpec hidden() const {
        ParamSpec c = *this;
        c.desc.flags |= DSPTB_PARAM_HIDDEN;
        return c;
    }
};

namespace detail {
constexpr ParamSpec makeParam(const char* id, const char* name, const char* unit, dsptb_param_type type,
                              float mn, float mx, float def) {
    ParamSpec s;
    s.desc.struct_size = static_cast<uint32_t>(sizeof(dsptb_param_desc));
    s.desc.id = id;
    s.desc.name = name;
    s.desc.unit = unit;
    s.desc.type = type;
    s.desc.min_value = mn;
    s.desc.max_value = mx;
    s.desc.default_value = def;
    return s;
}
} // namespace detail

constexpr ParamSpec Float(const char* id, const char* name, const char* unit, float mn, float mx, float def) {
    return detail::makeParam(id, name, unit, DSPTB_PARAM_FLOAT, mn, mx, def);
}
constexpr ParamSpec Int(const char* id, const char* name, const char* unit, int mn, int mx, int def) {
    return detail::makeParam(id, name, unit, DSPTB_PARAM_INT, static_cast<float>(mn), static_cast<float>(mx),
                             static_cast<float>(def));
}
constexpr ParamSpec Bool(const char* id, const char* name, bool def = false) {
    return detail::makeParam(id, name, "", DSPTB_PARAM_BOOL, 0.0f, 1.0f, def ? 1.0f : 0.0f);
}
// `labels` must have static storage duration (a static constexpr array in the module class).
template <size_t N>
constexpr ParamSpec Enum(const char* id, const char* name, const char* const (&labels)[N], int def = 0) {
    ParamSpec s = detail::makeParam(id, name, "", DSPTB_PARAM_ENUM, 0.0f, static_cast<float>(N - 1),
                                    static_cast<float>(def));
    s.desc.enum_labels = labels;
    s.desc.enum_count = static_cast<uint32_t>(N);
    return s;
}

template <class... Specs> constexpr auto params(Specs... specs) {
    return std::array<dsptb_param_desc, sizeof...(Specs)>{specs.desc...};
}
// A module without parameters.
constexpr auto noParams() {
    return std::array<dsptb_param_desc, 0>{};
}

} // namespace dsptb
