#include "dsptb/core/gpu_runtime_loader.hpp"

#include "dsptb/core/paths.hpp"
#include "dsptb/core/settings.hpp"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <cstdlib>

namespace dsptb {

namespace fs = std::filesystem;

GpuRuntimeLoader& GpuRuntimeLoader::instance() {
    static GpuRuntimeLoader loader;
    return loader;
}

void GpuRuntimeLoader::load(const GpuRuntimeRequest& req) {
    if (loaded_)
        return;
    loaded_ = true;
    state_ = GpuRuntimeState{};
    fs::path dir = req.dir.empty() ? appDir() / "gpu-runtime" : req.dir;
    std::error_code ec;
    dir = fs::absolute(dir, ec);
    state_.runtimeDir = toUtf8(dir);
    state_.syncMode = req.blocking ? DSPTB_GPU_SYNC_BLOCKING : DSPTB_GPU_SYNC_SPIN;

    const auto notInstalled = [&](int status, std::string text) {
        state_.status = status;
        state_.statusText = std::move(text);
        spdlog::debug("{}", state_.statusText);
    };

    const fs::path dll = dir / "dsptb_gpu.dll";
    if (!fs::exists(dll, ec)) {
        notInstalled(DSPTB_GPU_DLL_MISSING, "GPU runtime not installed (no dsptb_gpu.dll in " + state_.runtimeDir + ")");
        return;
    }
    HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
        notInstalled(DSPTB_GPU_DLL_MISSING,
                     "dsptb_gpu.dll could not be loaded (Windows error " + std::to_string(GetLastError()) + ")");
        return;
    }
    dll_ = h;
    state_.dllFound = true;
    init_ = reinterpret_cast<decltype(init_)>(GetProcAddress(h, "dsptb_gpu_init"));
    deviceInfo_ = reinterpret_cast<decltype(deviceInfo_)>(GetProcAddress(h, "dsptb_gpu_device_info"));
    shutdown_ = reinterpret_cast<decltype(shutdown_)>(GetProcAddress(h, "dsptb_gpu_shutdown"));
    if (!init_ || !shutdown_) {
        notInstalled(DSPTB_GPU_VERSION_MISMATCH, "dsptb_gpu.dll lacks the functions of GPU runtime ABI 1");
        return;
    }

    dsptb_gpu_runtime_info info{};
    info.struct_size = sizeof info;
    const std::string dirUtf8 = state_.runtimeDir;
    init_(dirUtf8.c_str(), req.device, req.blocking ? DSPTB_GPU_SYNC_BLOCKING : DSPTB_GPU_SYNC_SPIN, &info);
    state_.status = info.status;
    state_.statusText = info.status_text;
    state_.abiVersion = info.abi_version;
    state_.driverCuda = info.driver_cuda_version;
    state_.runtimeCuda = info.runtime_cuda_version;
    state_.cudnn = info.cudnn_version;
    state_.ortVersion = info.ort_version;
    state_.torchVersion = info.torch_version;
    state_.deviceCount = info.device_count;
    state_.selectedDevice = info.selected_device;
    state_.syncMode = info.sync_mode;
    if (info.status == DSPTB_GPU_OK)
        spdlog::debug("{}", state_.statusText);
    else
        spdlog::debug("{}", state_.statusText);
}

void GpuRuntimeLoader::shutdown() {
    if (!loaded_)
        return;
    if (shutdown_)
        shutdown_();
    if (dll_)
        FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
    init_ = nullptr;
    deviceInfo_ = nullptr;
    shutdown_ = nullptr;
    state_ = GpuRuntimeState{};
    loaded_ = false;
}

std::vector<GpuDevice> GpuRuntimeLoader::devices() const {
    std::vector<GpuDevice> out;
    if (!deviceInfo_ || state_.deviceCount <= 0)
        return out;
    for (int i = 0; i < state_.deviceCount; ++i) {
        struct dsptb_gpu_device_info d{};
        d.struct_size = sizeof d;
        if (!deviceInfo_(i, &d))
            continue;
        GpuDevice g;
        g.index = d.index;
        g.name = d.name;
        g.ccMajor = d.cc_major;
        g.ccMinor = d.cc_minor;
        g.vramTotal = d.vram_total_bytes;
        g.vramFree = d.vram_free_bytes;
        out.push_back(std::move(g));
    }
    return out;
}

std::string GpuRuntimeLoader::unavailableText() const {
    return "Needs the GPU runtime: " + state_.statusText;
}

GpuRuntimeRequest gpuRuntimeRequest(const Settings& s) {
    GpuRuntimeRequest req;
    req.dir = fromUtf8(s.gpu.runtimeDir);
    req.device = s.gpu.device;
    req.blocking = s.gpu.sync != "spin";
    wchar_t env[1024];
    if (const DWORD n = GetEnvironmentVariableW(L"DSPTB_GPU_RUNTIME_DIR", env, 1024); n > 0 && n < 1024)
        req.dir = fs::path(std::wstring(env, n));
    return req;
}

void loadGpuRuntimeFromSettings() {
    GpuRuntimeLoader& loader = GpuRuntimeLoader::instance();
    if (loader.loaded())
        return;
    Settings s;
    s.load(dataDir() / "settings.json"); // missing or corrupt: defaults
    loader.load(gpuRuntimeRequest(s));
}

} // namespace dsptb
