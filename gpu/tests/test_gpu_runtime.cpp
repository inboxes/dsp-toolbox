// Tests for dsptb_gpu.dll, loaded the way the host does it: LoadLibraryExW and GetProcAddress, no import library.
// Cases that need a GPU skip themselves (the test passes) on a machine without one.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "dsptb/gpu_runtime.h"

namespace fs = std::filesystem;

namespace {
struct Api {
    HMODULE dll = nullptr;
    int (*init)(const char*, int32_t, int32_t, dsptb_gpu_runtime_info*) = nullptr;
    void (*getInfo)(dsptb_gpu_runtime_info*) = nullptr;
    int (*deviceInfo)(int32_t, struct dsptb_gpu_device_info*) = nullptr;
    int32_t (*selected)() = nullptr;
    void (*lockShared)() = nullptr;
    void (*unlockShared)() = nullptr;
    void (*lockExclusive)() = nullptr;
    void (*unlockExclusive)() = nullptr;
    void (*shutdown)() = nullptr;
};

Api& api() {
    static Api a = [] {
        Api r;
        const std::wstring path = fs::path(DSPTB_GPU_RUNTIME_DIR).wstring() + L"\\dsptb_gpu.dll";
        r.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        REQUIRE_MESSAGE(r.dll, "cannot load dsptb_gpu.dll, error ", GetLastError());
        auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(r.dll, n)); };
        r.init = reinterpret_cast<decltype(r.init)>(get("dsptb_gpu_init"));
        r.getInfo = reinterpret_cast<decltype(r.getInfo)>(get("dsptb_gpu_get_info"));
        r.deviceInfo = reinterpret_cast<decltype(r.deviceInfo)>(get("dsptb_gpu_device_info"));
        r.selected = reinterpret_cast<decltype(r.selected)>(get("dsptb_gpu_selected_device"));
        r.lockShared = reinterpret_cast<decltype(r.lockShared)>(get("dsptb_gpu_lock_shared"));
        r.unlockShared = reinterpret_cast<decltype(r.unlockShared)>(get("dsptb_gpu_unlock_shared"));
        r.lockExclusive = reinterpret_cast<decltype(r.lockExclusive)>(get("dsptb_gpu_lock_exclusive"));
        r.unlockExclusive = reinterpret_cast<decltype(r.unlockExclusive)>(get("dsptb_gpu_unlock_exclusive"));
        r.shutdown = reinterpret_cast<decltype(r.shutdown)>(get("dsptb_gpu_shutdown"));
        REQUIRE(r.init);
        REQUIRE(r.getInfo);
        REQUIRE(r.deviceInfo);
        REQUIRE(r.selected);
        REQUIRE(r.shutdown);
        return r;
    }();
    return a;
}

fs::path tempDir(const char* name) {
    const fs::path d = fs::temp_directory_path() / (std::string("dsptb-gpu-test-") + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
}  // namespace

TEST_CASE("before init the runtime reports that it is not initialized") {
    dsptb_gpu_runtime_info info;
    std::memset(&info, 0xAB, sizeof info);
    api().getInfo(&info);
    CHECK(info.struct_size == sizeof info);
    CHECK(info.status == DSPTB_GPU_NOT_INITIALIZED);
    CHECK(info.selected_device == -1);
}

TEST_CASE("a folder without gpu-runtime.json is DLL_MISSING with the folder in the text") {
    const fs::path d = tempDir("nomanifest");
    dsptb_gpu_runtime_info info;
    CHECK(api().init(d.string().c_str(), 0, DSPTB_GPU_SYNC_SPIN, &info) == DSPTB_GPU_DLL_MISSING);
    CHECK(info.status == DSPTB_GPU_DLL_MISSING);
    CHECK(std::string(info.status_text).find("not found") != std::string::npos);
    CHECK(info.selected_device == -1);
    fs::remove_all(d);
}

TEST_CASE("a manifest with another ABI number is VERSION_MISMATCH") {
    const fs::path d = tempDir("abi");
    std::ofstream(d / "gpu-runtime.json") << R"({"schema":1,"runtimeAbi":2,"files":[]})";
    dsptb_gpu_runtime_info info;
    CHECK(api().init(d.string().c_str(), 0, DSPTB_GPU_SYNC_SPIN, &info) == DSPTB_GPU_VERSION_MISMATCH);
    fs::remove_all(d);
}

TEST_CASE("a manifest without its DLLs names the first missing one") {
    const fs::path d = tempDir("nodlls");
    fs::create_directories(d / "bin");
    std::ofstream(d / "gpu-runtime.json") << R"({"schema":1,"runtimeAbi":1,"libtorch":"x","files":[]})";
    dsptb_gpu_runtime_info info;
    CHECK(api().init(d.string().c_str(), 0, DSPTB_GPU_SYNC_SPIN, &info) == DSPTB_GPU_DLL_MISSING);
    CHECK(std::string(info.status_text).find("cudart64_13.dll") != std::string::npos);
    fs::remove_all(d);
}

TEST_CASE("the real runtime folder initialises and reports its versions") {
    dsptb_gpu_runtime_info info;
    const int status = api().init(DSPTB_GPU_RUNTIME_DIR, 0, DSPTB_GPU_SYNC_BLOCKING, &info);
    if (status == DSPTB_GPU_NO_DRIVER || status == DSPTB_GPU_NO_DEVICE) {
        MESSAGE("skipped: ", info.status_text);
        return;
    }
    INFO(info.status_text);
    REQUIRE(status == DSPTB_GPU_OK);
    MESSAGE(info.status_text);
    CHECK(info.abi_version == DSPTB_GPU_RUNTIME_ABI);
    CHECK(info.driver_cuda_version >= 13000);
    CHECK(info.runtime_cuda_version >= 13000);
    CHECK(info.cudnn_version >= 90000);
    CHECK(std::string(info.ort_version) == "1.31.1");
    CHECK(std::string(info.torch_version).find("2.14.1") == 0);
    CHECK(info.device_count >= 1);
    CHECK(info.selected_device == 0);
    CHECK(info.sync_mode == DSPTB_GPU_SYNC_BLOCKING);

    // Idempotent: other arguments after success change nothing.
    dsptb_gpu_runtime_info again;
    CHECK(api().init("C:\\somewhere\\else", 3, DSPTB_GPU_SYNC_SPIN, &again) == DSPTB_GPU_OK);
    CHECK(again.sync_mode == DSPTB_GPU_SYNC_BLOCKING);
    CHECK(std::string(again.runtime_dir) == std::string(info.runtime_dir));
    CHECK(api().selected() == 0);

    struct dsptb_gpu_device_info dev;
    REQUIRE(api().deviceInfo(0, &dev) == 1);
    CHECK(dev.struct_size == sizeof dev);
    CHECK(std::string(dev.name).find("NVIDIA") != std::string::npos);
    CHECK(dev.cc_major >= 7);
    CHECK(dev.vram_total_bytes > 1000000000ull);
    CHECK(dev.vram_free_bytes > 0);
    CHECK(dev.vram_free_bytes <= dev.vram_total_bytes);
    CHECK(api().deviceInfo(99, &dev) == 0);

    // The capture lock: several shared holders at once, exclusive after they leave.
    api().lockShared();
    api().lockShared();
    api().unlockShared();
    api().unlockShared();
    api().lockExclusive();
    api().unlockExclusive();

    api().shutdown();
    dsptb_gpu_runtime_info after;
    api().getInfo(&after);
    CHECK(after.status == DSPTB_GPU_NOT_INITIALIZED);
}
