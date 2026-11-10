#include <dsptb/gpu/cuda_util.hpp>
#include <dsptb/gpu/runtime_api.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace dsptb::gpu {

namespace {

struct Api {
    HMODULE dll = nullptr;
    int (*init)(const char*, int32_t, int32_t, dsptb_gpu_runtime_info*) = nullptr;
    void (*getInfo)(dsptb_gpu_runtime_info*) = nullptr;
    int32_t (*selected)() = nullptr;
    void (*lockShared)() = nullptr;
    void (*unlockShared)() = nullptr;
    void (*lockExclusive)() = nullptr;
    void (*unlockExclusive)() = nullptr;
};

Api& api() {
    static Api a;
    return a;
}

// Fills the table from a loaded dsptb_gpu.dll.
bool bind(HMODULE h) {
    Api& a = api();
    a.dll = h;
    a.init = reinterpret_cast<decltype(a.init)>(GetProcAddress(h, "dsptb_gpu_init"));
    a.getInfo = reinterpret_cast<decltype(a.getInfo)>(GetProcAddress(h, "dsptb_gpu_get_info"));
    a.selected = reinterpret_cast<decltype(a.selected)>(GetProcAddress(h, "dsptb_gpu_selected_device"));
    a.lockShared = reinterpret_cast<decltype(a.lockShared)>(GetProcAddress(h, "dsptb_gpu_lock_shared"));
    a.unlockShared = reinterpret_cast<decltype(a.unlockShared)>(GetProcAddress(h, "dsptb_gpu_unlock_shared"));
    a.lockExclusive = reinterpret_cast<decltype(a.lockExclusive)>(GetProcAddress(h, "dsptb_gpu_lock_exclusive"));
    a.unlockExclusive = reinterpret_cast<decltype(a.unlockExclusive)>(GetProcAddress(h, "dsptb_gpu_unlock_exclusive"));
    return a.init && a.getInfo && a.selected && a.lockShared && a.unlockShared && a.lockExclusive && a.unlockExclusive;
}

// The host (or a standalone tool) loaded dsptb_gpu.dll by path; a module just finds the loaded instance by name.
Api& resolved() {
    static std::once_flag once;
    Api& a = api();
    std::call_once(once, [] {
        if (!api().dll) {
            if (HMODULE h = GetModuleHandleW(L"dsptb_gpu.dll")) bind(h);
        }
    });
    return a;
}

LogSink gSink = nullptr;

} // namespace

int loadRuntime(const std::filesystem::path& runtimeDir, int device, bool blocking, dsptb_gpu_runtime_info& info) {
    std::memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    Api& a = api();
    if (!a.dll) {
        const std::filesystem::path dll = std::filesystem::absolute(runtimeDir) / "dsptb_gpu.dll";
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!h || !bind(h)) {
            info.status = DSPTB_GPU_DLL_MISSING;
            std::snprintf(info.status_text, sizeof info.status_text, "cannot load %s", dll.string().c_str());
            return info.status;
        }
    }
    const std::string dir = std::filesystem::absolute(runtimeDir).string();
    return a.init(dir.c_str(), device, blocking ? DSPTB_GPU_SYNC_BLOCKING : DSPTB_GPU_SYNC_SPIN, &info);
}

int selectedDevice() {
    Api& a = resolved();
    return a.selected ? a.selected() : -1;
}

bool blockingSync() {
    Api& a = resolved();
    if (!a.getInfo) return false;
    dsptb_gpu_runtime_info info{};
    info.struct_size = sizeof info;
    a.getInfo(&info);
    return info.sync_mode == DSPTB_GPU_SYNC_BLOCKING;
}

CaptureLock runtimeCaptureLock() {
    Api& a = resolved();
    CaptureLock l;
    if (a.lockShared) {
        l.lockShared = a.lockShared;
        l.unlockShared = a.unlockShared;
        l.lockExclusive = a.lockExclusive;
        l.unlockExclusive = a.unlockExclusive;
    }
    return l;
}

ScopedCapture::ScopedCapture() {
    if (Api& a = resolved(); a.lockExclusive) a.lockExclusive();
}

ScopedCapture::~ScopedCapture() {
    if (Api& a = resolved(); a.unlockExclusive) a.unlockExclusive();
}

void setLogSink(LogSink sink) { gSink = sink; }

void logMessage(int level, const std::string& message) {
    if (gSink) gSink(level, message.c_str());
    else if (level >= 1) std::fprintf(stderr, "[gpu] %s\n", message.c_str());
}

// ---- cuda_util.hpp ----

CudaStream::CudaStream(int device) : device_(device) {
    DSPTB_CUDA_CHECK(cudaSetDevice(device));
    int least = 0, greatest = 0;
    DSPTB_CUDA_CHECK(cudaDeviceGetStreamPriorityRange(&least, &greatest));
    DSPTB_CUDA_CHECK(cudaStreamCreateWithPriority(&stream_, cudaStreamNonBlocking, greatest));
    const unsigned flags = cudaEventDisableTiming | (blockingSync() ? cudaEventBlockingSync : 0u);
    DSPTB_CUDA_CHECK(cudaEventCreateWithFlags(&event_, flags));
}

CudaStream::~CudaStream() {
    if (event_) cudaEventDestroy(event_);
    if (stream_) cudaStreamDestroy(stream_);
}

void CudaStream::synchronize() {
    DSPTB_CUDA_CHECK(cudaEventRecord(event_, stream_));
    DSPTB_CUDA_CHECK(cudaEventSynchronize(event_));
}

} // namespace dsptb::gpu
