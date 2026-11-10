#pragma once

#include <dsptb/gpu_runtime.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dsptb {

// What the host knows about the optional GPU runtime (docs/GPU_RUNTIME.md section 6.5). Plain data.
struct GpuRuntimeState {
    bool dllFound = false;             // dsptb_gpu.dll was present and loaded
    int status = DSPTB_GPU_NOT_INITIALIZED; // DSPTB_GPU_*
    std::string statusText = "GPU runtime not loaded";
    std::string runtimeDir;            // the folder that was tried
    uint32_t abiVersion = 0;
    int driverCuda = 0, runtimeCuda = 0, cudnn = 0; // e.g. 13000, 13030, 92400
    std::string ortVersion, torchVersion;
    int deviceCount = 0;
    int selectedDevice = -1;
    int syncMode = DSPTB_GPU_SYNC_BLOCKING;
    bool ok() const { return status == DSPTB_GPU_OK; }
};

struct GpuDevice {
    int index = 0;
    std::string name;
    int ccMajor = 0, ccMinor = 0;
    uint64_t vramTotal = 0, vramFree = 0; // free is 0 when unknown
};

struct GpuRuntimeRequest {
    std::filesystem::path dir; // empty = <exe dir>/gpu-runtime
    int device = 0;
    bool blocking = true;      // gpu.sync: "blocking" (default) or "spin"
};

// The one place where the GPL host reaches the MIT GPU runtime: LoadLibraryExW + GetProcAddress on
// dsptb_gpu.dll, never linked. No CUDA, ONNX Runtime or LibTorch header is included here. A missing runtime is not
// an error: the state says why and modules that need it stay greyed out. UI thread only.
class GpuRuntimeLoader {
public:
    static GpuRuntimeLoader& instance();

    // Loads and initialises once; later calls change nothing until shutdown(). Never throws. Call it before the
    // module scan (the device flags must be set before any module creates a CUDA context).
    void load(const GpuRuntimeRequest& req);
    // After every module is gone: dsptb_gpu_shutdown, then FreeLibrary. Idempotent; a new load() may follow.
    void shutdown();

    bool loaded() const { return loaded_; }
    const GpuRuntimeState& state() const { return state_; }
    bool available() const { return state_.ok(); }
    std::vector<GpuDevice> devices() const;
    // "Needs the GPU runtime: <reason>" for a module that cannot be used.
    std::string unavailableText() const;

private:
    GpuRuntimeLoader() = default;
    bool loaded_ = false;      // load() ran
    GpuRuntimeState state_;
    void* dll_ = nullptr;      // HMODULE
    int (*init_)(const char*, int32_t, int32_t, dsptb_gpu_runtime_info*) = nullptr;
    int (*deviceInfo_)(int32_t, struct dsptb_gpu_device_info*) = nullptr;
    void (*shutdown_)(void) = nullptr;
};

struct Settings;
// The request for these settings (gpu.runtimeDir, gpu.device, gpu.sync). The environment variable
// DSPTB_GPU_RUNTIME_DIR overrides the folder (tests and CI).
GpuRuntimeRequest gpuRuntimeRequest(const Settings& settings);
// Reads the settings file and loads the runtime. For commands that have no Settings object of their own
// (dsptb-cli). Idempotent.
void loadGpuRuntimeFromSettings();

} // namespace dsptb
