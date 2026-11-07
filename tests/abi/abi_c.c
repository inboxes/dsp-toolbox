/* The ABI header must compile as plain C (C11), warnings as errors, and its layout asserts must hold. */
#include <dsptb/plugin_abi.h>

static const dsptb_param_desc kParam = {sizeof(dsptb_param_desc),
                                        "gain_db",
                                        "Gain",
                                        "dB",
                                        DSPTB_PARAM_FLOAT,
                                        -60.0f,
                                        24.0f,
                                        0.0f,
                                        20.0f,
                                        0u,
                                        0,
                                        0u};

uint32_t dsptb_abi_c_check(void) {
    dsptb_module_desc d = {0};
    d.struct_size = sizeof(d);
    d.abi_version = DSPTB_ABI_VERSION;
    d.params = &kParam;
    d.num_params = 1u;
    d.kind = DSPTB_KIND_PROCESSOR;
    return d.abi_version + d.num_params + (uint32_t)sizeof(dsptb_canvas_api);
}
