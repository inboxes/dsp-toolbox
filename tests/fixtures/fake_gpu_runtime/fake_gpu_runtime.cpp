// Test fixture: a stand-in for dsptb_gpu.dll. It speaks the runtime ABI (gpu/include/dsptb/gpu_runtime.h) and
// returns whatever the environment asks for, so the host loader, module gating, the CLI and the settings UI can be
// tested on machines without a GPU.
//   DSPTB_FAKE_GPU_STATUS   a DSPTB_GPU_* value (default 0 = OK)
//   DSPTB_FAKE_GPU_ABI      the abi_version it reports (default 1)
#define DSPTB_GPU_BUILD 1
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dsptb/gpu_runtime.h"

namespace {
int envInt(const char* name, int fallback) {
    char buf[32];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::atoi(buf) : fallback;
}
dsptb_gpu_runtime_info gInfo{};
bool gHave = false;
}  // namespace

extern "C" {

DSPTB_GPU_API int dsptb_gpu_init(const char* dir, int32_t device, int32_t sync, dsptb_gpu_runtime_info* out) {
    std::memset(&gInfo, 0, sizeof gInfo);
    gInfo.struct_size = sizeof gInfo;
    gInfo.abi_version = static_cast<uint32_t>(envInt("DSPTB_FAKE_GPU_ABI", 1));
    gInfo.status = envInt("DSPTB_FAKE_GPU_STATUS", DSPTB_GPU_OK);
    std::snprintf(gInfo.runtime_dir, sizeof gInfo.runtime_dir, "%s", dir ? dir : "");
    gInfo.sync_mode = sync;
    if (gInfo.status == DSPTB_GPU_OK) {
        std::snprintf(gInfo.status_text, sizeof gInfo.status_text,
                      "GPU: Fake GPU (8.6, 24 GB), driver CUDA 13.0, cuDNN 9.24, ORT 1.0.0, LibTorch 2.0.0, sync fake");
        gInfo.driver_cuda_version = 13000;
        gInfo.runtime_cuda_version = 13000;
        gInfo.cudnn_version = 92400;
        std::snprintf(gInfo.ort_version, sizeof gInfo.ort_version, "1.0.0");
        std::snprintf(gInfo.torch_version, sizeof gInfo.torch_version, "2.0.0+fake");
        gInfo.device_count = 1;
        gInfo.selected_device = device == 0 ? 0 : 0;
    } else {
        std::snprintf(gInfo.status_text, sizeof gInfo.status_text, "fake runtime says status %d", gInfo.status);
        gInfo.selected_device = -1;
    }
    gHave = true;
    if (out) *out = gInfo;
    return gInfo.status;
}

DSPTB_GPU_API void dsptb_gpu_get_info(dsptb_gpu_runtime_info* out) {
    if (out && gHave) *out = gInfo;
}

DSPTB_GPU_API int dsptb_gpu_device_info(int32_t index, struct dsptb_gpu_device_info* out) {
    if (!out || index != 0 || !gHave || gInfo.status != DSPTB_GPU_OK) return 0;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    std::snprintf(out->name, sizeof out->name, "Fake GPU");
    out->cc_major = 8;
    out->cc_minor = 6;
    out->vram_total_bytes = 24ull << 30;
    out->vram_free_bytes = 20ull << 30;
    return 1;
}

DSPTB_GPU_API int32_t dsptb_gpu_selected_device(void) { return gHave ? gInfo.selected_device : -1; }
DSPTB_GPU_API void dsptb_gpu_lock_shared(void) {}
DSPTB_GPU_API void dsptb_gpu_unlock_shared(void) {}
DSPTB_GPU_API void dsptb_gpu_lock_exclusive(void) {}
DSPTB_GPU_API void dsptb_gpu_unlock_exclusive(void) {}
DSPTB_GPU_API void dsptb_gpu_shutdown(void) { gHave = false; }

}  // extern "C"
