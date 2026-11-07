// Test fixture: a hand-written module (no SDK trampolines) whose descriptor claims ABI version 99. The host
// must list it as "Incompatible ABI" and never instantiate it.
#include <dsptb/plugin_abi.h>

namespace {
const dsptb_module_desc kDesc = [] {
    dsptb_module_desc d{};
    d.struct_size = sizeof(d);
    d.abi_version = 99u;
    d.id = "test.abimismatch";
    d.name = "ABI mismatch (test)";
    d.version = "0.1.0";
    d.vendor = "DSP Toolbox tests";
    d.description = "";
    d.kind = DSPTB_KIND_PROCESSOR;
    return d;
}();
} // namespace

extern "C" {
DSPTB_EXPORT uint32_t dsptb_module_count(void) {
    return 1;
}
DSPTB_EXPORT const dsptb_module_desc* dsptb_module_get(uint32_t index, uint32_t) {
    return index == 0 ? &kDesc : nullptr; // ignores the host's ABI version on purpose
}
}
