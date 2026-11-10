// dsptb_gpu.dll: finds and loads the GPU runtime, picks the device, and owns the process-wide capture lock.
// docs/GPU_RUNTIME.md section 6. It does not create ORT sessions or load LibTorch.
//
// Everything native (cudart, the driver, cuDNN, ORT) is reached through GetProcAddress after the preload, so this
// DLL itself has no import that could fail while the host loads it, before the runtime folder is on the search path.
#define DSPTB_GPU_BUILD 1
#include <windows.h>

#include <cuda_runtime_api.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dsptb/gpu_runtime.h"

namespace {

// --- small helpers ---
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
template <size_t N> void copyText(char (&dst)[N], const std::string& src) {
    std::snprintf(dst, N, "%s", src.c_str());
}

// --- state ---
std::mutex gMutex;           // init and shutdown
bool gReady = false;         // a successful init is stored
dsptb_gpu_runtime_info gInfo{};
DLL_DIRECTORY_COOKIE gCookie = nullptr;
HMODULE gCudart = nullptr;
HMODULE gDriver = nullptr;
bool gDriverInit = false;
SRWLOCK gCaptureLock = SRWLOCK_INIT;

void fillNotInitialized(dsptb_gpu_runtime_info& i) {
    std::memset(&i, 0, sizeof i);
    i.struct_size = sizeof i;
    i.abi_version = DSPTB_GPU_RUNTIME_ABI;
    i.status = DSPTB_GPU_NOT_INITIALIZED;
    copyText(i.status_text, "GPU runtime not initialized");
    i.selected_device = -1;
}

int fail(dsptb_gpu_runtime_info& i, int status, const std::string& text) {
    i.status = status;
    copyText(i.status_text, text);
    i.selected_device = -1;
    return status;
}

// --- the native calls that can crash inside a broken driver: plain C code, no destructors (SEH rule) ---
struct Probe {
    int driver = 0, runtime = 0, count = 0, selected = -1;
    int cudartOk = 1;
    unsigned long seh = 0;
};
using FnGetInt = int (*)(int*);
using FnSetDevice = int (*)(int);
using FnSetFlags = int (*)(unsigned);

bool probeCuda(Probe& p, FnGetInt driverVersion, FnGetInt runtimeVersion, FnGetInt deviceCount, FnSetDevice setDevice,
               FnSetFlags setFlags, int wantDevice, int blocking) {
    __try {
        driverVersion(&p.driver);
        runtimeVersion(&p.runtime);
        if (p.driver < 13000) return true;
        if (deviceCount(&p.count) != 0 || p.count <= 0) return true;
        p.selected = wantDevice < 0 || wantDevice >= p.count ? 0 : wantDevice;
        // The flag must be set before anything creates the CUDA context on this device.
        if (blocking) setFlags(cudaDeviceScheduleBlockingSync);
        if (setDevice(p.selected) != 0) p.selected = -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p.seh = GetExceptionCode();
        return false;
    }
}

// Driver API (nvcuda.dll ships with the driver). Device names and attributes without creating a context.
using FnCuInit = int (*)(unsigned);
using FnCuCount = int (*)(int*);
using FnCuName = int (*)(char*, int, int);
using FnCuAttr = int (*)(int*, int, int);
using FnCuTotal = int (*)(size_t*, int);

bool driverDeviceInfo(int index, struct dsptb_gpu_device_info& d) {
    if (!gDriver) gDriver = LoadLibraryW(L"nvcuda.dll");
    if (!gDriver) return false;
    auto cuInit = reinterpret_cast<FnCuInit>(GetProcAddress(gDriver, "cuInit"));
    auto cuCount = reinterpret_cast<FnCuCount>(GetProcAddress(gDriver, "cuDeviceGetCount"));
    auto cuName = reinterpret_cast<FnCuName>(GetProcAddress(gDriver, "cuDeviceGetName"));
    auto cuAttr = reinterpret_cast<FnCuAttr>(GetProcAddress(gDriver, "cuDeviceGetAttribute"));
    auto cuTotal = reinterpret_cast<FnCuTotal>(GetProcAddress(gDriver, "cuDeviceTotalMem_v2"));
    if (!cuInit || !cuCount || !cuName || !cuAttr || !cuTotal) return false;
    __try {
        if (!gDriverInit) {
            if (cuInit(0) != 0) return false;
            gDriverInit = true;
        }
        int n = 0;
        if (cuCount(&n) != 0 || index < 0 || index >= n) return false;
        std::memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.index = index;
        if (cuName(d.name, static_cast<int>(sizeof d.name) - 1, index) != 0) return false;
        cuAttr(&d.cc_major, 75 /*COMPUTE_CAPABILITY_MAJOR*/, index);
        cuAttr(&d.cc_minor, 76 /*COMPUTE_CAPABILITY_MINOR*/, index);
        size_t total = 0;
        cuTotal(&total, index);
        d.vram_total_bytes = total;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool freeVram(uint64_t& freeBytes) {
    if (!gCudart) return false;
    using Fn = int (*)(size_t*, size_t*);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(gCudart, "cudaMemGetInfo"));
    if (!fn) return false;
    __try {
        size_t f = 0, t = 0;
        if (fn(&f, &t) != 0) return false;
        freeBytes = f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- preload (spec 6.2 step 4) ---
// Found by S1: this order loads; cuDNN's engine DLLs before nvrtc / nvJitLink fail with error 127. We do not
// preload onnxruntime_providers_cuda.dll: loading it ourselves fails with 1114, ORT loads it from next to
// onnxruntime.dll by itself.
const wchar_t* const kPreload[] = {L"cudart64_13.dll", L"cublasLt64_13.dll", L"cublas64_13.dll", L"nvrtc64_130_0.dll",
                                   L"nvJitLink_130_0.dll", L"cufft64_12.dll", L"curand64_10.dll", L"cudnn64_9.dll",
                                   L"cudnn_graph64_9.dll", L"cudnn_engines_precompiled64_9.dll",
                                   L"cudnn_engines_runtime_compiled64_9.dll", L"cudnn_heuristic64_9.dll",
                                   L"onnxruntime.dll", L"onnxruntime_providers_shared.dll"};

bool preloadAll(const std::wstring& bin, std::string& error) {
    std::set<std::wstring> done;
    auto load = [&](const std::wstring& name, bool required) {
        const std::wstring path = bin + L"\\" + name;
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!h) {
            const DWORD e = GetLastError();
            if (required) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " (Windows error %lu)", e);
                error = "GPU runtime: " + narrow(name) + (e == ERROR_MOD_NOT_FOUND || e == ERROR_FILE_NOT_FOUND ? " is missing or one of its dependencies is" : " could not be loaded") + buf;
            }
            return false;
        }
        if (name == L"cudart64_13.dll") gCudart = h;
        done.insert(name);
        return true;
    };
    for (const wchar_t* n : kPreload)
        if (!load(n, true)) return false;
    // Any other cuDNN sub-library (ops, cnn, adv, ext, tensor_ir): loaded when present.
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((bin + L"\\cudnn_*.dll").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!done.count(fd.cFileName)) load(fd.cFileName, false);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return true;
}

struct OrtApiBaseLite {
    const void* (*GetApi)(uint32_t);
    const char* (*GetVersionString)();
};

std::wstring ownDir() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dsptb_gpu_selected_device), &h);
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(h, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(0, slash);
}

int initLocked(const std::string& dirUtf8, int deviceIndex, int syncMode, dsptb_gpu_runtime_info& out) {
    dsptb_gpu_runtime_info i;
    fillNotInitialized(i);
    i.sync_mode = syncMode == DSPTB_GPU_SYNC_BLOCKING ? DSPTB_GPU_SYNC_BLOCKING : DSPTB_GPU_SYNC_SPIN;
    copyText(i.runtime_dir, dirUtf8);
    auto finish = [&](int status) {
        gInfo = i;
        out = i;
        return status;
    };

    // 1 and 2: the manifest.
    const std::wstring dir = widen(dirUtf8);
    std::string torchVersion;
    try {
        std::ifstream f(dir + L"\\gpu-runtime.json", std::ios::binary);
        if (!f) return finish(fail(i, DSPTB_GPU_DLL_MISSING, "GPU runtime not found in " + dirUtf8));
        const auto j = nlohmann::json::parse(f);
        if (j.value("runtimeAbi", 0) != static_cast<int>(DSPTB_GPU_RUNTIME_ABI))
            return finish(fail(i, DSPTB_GPU_VERSION_MISMATCH, "GPU runtime in " + dirUtf8 + " has a different ABI version"));
        torchVersion = j.value("libtorch", std::string());
    } catch (const std::exception& e) {
        return finish(fail(i, DSPTB_GPU_DLL_MISSING, std::string("GPU runtime manifest unreadable: ") + e.what()));
    }
    copyText(i.torch_version, torchVersion);

    // 3: DLL search. AddDllDirectory serves LoadLibraryEx with default dirs (modules); PATH serves cuDNN's own plain LoadLibrary calls.
    const std::wstring bin = dir + L"\\bin";
    if (gCookie) RemoveDllDirectory(gCookie);
    gCookie = AddDllDirectory(bin.c_str());
    {
        const DWORD need = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring old(need ? need - 1 : 0, L'\0');
        if (need) GetEnvironmentVariableW(L"PATH", old.data(), need);
        SetEnvironmentVariableW(L"PATH", (bin + L";" + old).c_str());
    }

    // 4: preload.
    std::string error;
    if (!preloadAll(bin, error)) return finish(fail(i, DSPTB_GPU_DLL_MISSING, error));

    // 5 to 7: CUDA (under SEH).
    auto fn = [&](const char* name) { return GetProcAddress(gCudart, name); };
    auto driverVersion = reinterpret_cast<FnGetInt>(fn("cudaDriverGetVersion"));
    auto runtimeVersion = reinterpret_cast<FnGetInt>(fn("cudaRuntimeGetVersion"));
    auto deviceCount = reinterpret_cast<FnGetInt>(fn("cudaGetDeviceCount"));
    auto setDevice = reinterpret_cast<FnSetDevice>(fn("cudaSetDevice"));
    auto setFlags = reinterpret_cast<FnSetFlags>(fn("cudaSetDeviceFlags"));
    if (!driverVersion || !runtimeVersion || !deviceCount || !setDevice || !setFlags)
        return finish(fail(i, DSPTB_GPU_DLL_MISSING, "GPU runtime: cudart64_13.dll lacks an expected function"));
    Probe p;
    if (!probeCuda(p, driverVersion, runtimeVersion, deviceCount, setDevice, setFlags, deviceIndex,
                   i.sync_mode == DSPTB_GPU_SYNC_BLOCKING)) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "crash during GPU init (0x%08lX)", p.seh);
        return finish(fail(i, DSPTB_GPU_INIT_FAILED, buf));
    }
    i.driver_cuda_version = p.driver;
    i.runtime_cuda_version = p.runtime;
    i.device_count = p.count;
    if (p.driver < 13000) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "NVIDIA driver too old for CUDA 13.0 (needs R580 or newer); found CUDA %d.%d",
                      p.driver / 1000, p.driver % 1000 / 10);
        return finish(fail(i, DSPTB_GPU_NO_DRIVER, p.driver == 0 ? "No NVIDIA driver found (needs R580 or newer)" : buf));
    }
    if (p.count <= 0) return finish(fail(i, DSPTB_GPU_NO_DEVICE, "No NVIDIA GPU found"));
    if (p.selected < 0) return finish(fail(i, DSPTB_GPU_INIT_FAILED, "cudaSetDevice failed"));
    i.selected_device = p.selected;

    // 8: versions.
    if (HMODULE cudnn = GetModuleHandleW(L"cudnn64_9.dll")) {
        using Fn = size_t (*)();
        if (auto get = reinterpret_cast<Fn>(GetProcAddress(cudnn, "cudnnGetVersion"))) i.cudnn_version = static_cast<int32_t>(get());
    }
    if (HMODULE ort = GetModuleHandleW(L"onnxruntime.dll")) {
        using Fn = const OrtApiBaseLite* (*)();
        if (auto get = reinterpret_cast<Fn>(GetProcAddress(ort, "OrtGetApiBase"))) {
            if (const OrtApiBaseLite* base = get()) copyText(i.ort_version, base->GetVersionString());
        }
    }
    struct dsptb_gpu_device_info d;
    char name[128] = "unknown GPU";
    int ccMajor = 0, ccMinor = 0;
    double gb = 0;
    if (driverDeviceInfo(i.selected_device, d)) {
        std::snprintf(name, sizeof name, "%s", d.name);
        ccMajor = d.cc_major;
        ccMinor = d.cc_minor;
        gb = static_cast<double>(d.vram_total_bytes) / (1024.0 * 1024.0 * 1024.0);
    }
    // 9: the one-line summary the host logs.
    char line[256];
    std::snprintf(line, sizeof line, "GPU: %s (%d.%d, %.0f GB), driver CUDA %d.%d, cuDNN %d.%d, ORT %s, LibTorch %s, sync %s", name, ccMajor,
                  ccMinor, gb, p.driver / 1000, p.driver % 1000 / 10, i.cudnn_version / 10000, i.cudnn_version % 10000 / 100,
                  i.ort_version, i.torch_version, i.sync_mode == DSPTB_GPU_SYNC_BLOCKING ? "blocking" : "spin");
    i.status = DSPTB_GPU_OK;
    copyText(i.status_text, line);
    gReady = true;
    return finish(DSPTB_GPU_OK);
}

}  // namespace

extern "C" {

DSPTB_GPU_API int dsptb_gpu_init(const char* runtime_dir_utf8, int32_t device_index, int32_t sync_mode,
                                 dsptb_gpu_runtime_info* out) {
    std::lock_guard lock(gMutex);
    dsptb_gpu_runtime_info scratch;
    dsptb_gpu_runtime_info& dst = out ? *out : scratch;
    if (gReady) {  // idempotent; different arguments after a successful init are ignored
        dst = gInfo;
        return gInfo.status;
    }
    try {
        return initLocked(runtime_dir_utf8 ? runtime_dir_utf8 : "", device_index, sync_mode, dst);
    } catch (...) {
        dsptb_gpu_runtime_info i;
        fillNotInitialized(i);
        fail(i, DSPTB_GPU_INIT_FAILED, "unexpected exception during GPU init");
        gInfo = i;
        dst = i;
        return i.status;
    }
}

DSPTB_GPU_API void dsptb_gpu_get_info(dsptb_gpu_runtime_info* out) {
    if (!out) return;
    std::lock_guard lock(gMutex);
    if (gInfo.struct_size == 0) fillNotInitialized(*out);
    else *out = gInfo;
}

DSPTB_GPU_API int dsptb_gpu_device_info(int32_t index, struct dsptb_gpu_device_info* out) {
    if (!out) return 0;
    std::lock_guard lock(gMutex);
    if (!driverDeviceInfo(index, *out)) return 0;
    if (gReady && index == gInfo.selected_device) freeVram(out->vram_free_bytes);
    return 1;
}

DSPTB_GPU_API int32_t dsptb_gpu_selected_device(void) {
    {
        std::lock_guard lock(gMutex);
        if (gReady) return gInfo.selected_device;
    }
    const std::string dir = narrow(ownDir());
    dsptb_gpu_runtime_info info;
    dsptb_gpu_init(dir.c_str(), 0, DSPTB_GPU_SYNC_SPIN, &info);
    return info.status == DSPTB_GPU_OK ? info.selected_device : -1;
}

DSPTB_GPU_API void dsptb_gpu_lock_shared(void) { AcquireSRWLockShared(&gCaptureLock); }
DSPTB_GPU_API void dsptb_gpu_unlock_shared(void) { ReleaseSRWLockShared(&gCaptureLock); }
DSPTB_GPU_API void dsptb_gpu_lock_exclusive(void) { AcquireSRWLockExclusive(&gCaptureLock); }
DSPTB_GPU_API void dsptb_gpu_unlock_exclusive(void) { ReleaseSRWLockExclusive(&gCaptureLock); }

DSPTB_GPU_API void dsptb_gpu_shutdown(void) {
    std::lock_guard lock(gMutex);
    if (gCookie) {
        RemoveDllDirectory(gCookie);
        gCookie = nullptr;
    }
    gReady = false;
    std::memset(&gInfo, 0, sizeof gInfo);
}

}  // extern "C"
