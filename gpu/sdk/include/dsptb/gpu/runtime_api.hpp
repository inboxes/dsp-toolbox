// How code inside the GPU runtime's reach (stages, test tools, GPU modules) talks to dsptb_gpu.dll. Resolved
// at run time with GetProcAddress instead of an import library: a module DLL or a test tool that imported
// dsptb_gpu.dll would fail to start whenever the runtime folder is not already on the DLL search path, and
// the host loads dsptb_gpu.dll before any module anyway, so the module gets the same instance, device and
// lock.
#pragma once

#include <dsptb/gpu_runtime.h>

#include <dsptb/gpu/pipeline.hpp>

#include <filesystem>
#include <string>

namespace dsptb::gpu {

// Standalone tools (tests, dsptb-gpu-bench) have no host: they load and initialise the runtime themselves.
// Returns the init status (DSPTB_GPU_OK on success); `info` gets the details. Idempotent.
int loadRuntime(const std::filesystem::path& runtimeDir, int device, bool blocking,
                dsptb_gpu_runtime_info& info);

// The device the runtime selected (auto-initialises with defaults when nobody did), or -1 without a runtime.
int selectedDevice();
// True when the runtime waits for the GPU without spinning (gpu.sync = blocking).
bool blockingSync();
// The runtime's process-wide capture lock as hooks for Pipeline::setCaptureLock. Empty hooks without a
// runtime.
CaptureLock runtimeCaptureLock();

// Holds the runtime's capture lock exclusively for its lifetime (a no-op without a runtime). A stage takes it
// around a CUDA graph capture and nothing else: while one thread captures in global mode, allocations and
// synchronising calls are illegal on every other thread, so the running workers (which hold the lock shared
// around each run) wait for it.
class ScopedCapture {
public:
    ScopedCapture();
    ~ScopedCapture();
    ScopedCapture(const ScopedCapture&) = delete;
    ScopedCapture& operator=(const ScopedCapture&) = delete;
};

// Library messages (a stage that fell back from CUDA graphs, timings). level: 0 debug, 1 info, 2 warn.
using LogSink = void (*)(int level, const char* message);
void setLogSink(LogSink sink);
void logMessage(int level, const std::string& message);

} // namespace dsptb::gpu
