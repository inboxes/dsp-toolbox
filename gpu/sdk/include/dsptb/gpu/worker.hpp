// Platform helpers for the inference worker thread (docs/GPU_RUNTIME.md section 7.1). Windows only, no CUDA. The
// implementation is in worker.cpp so that this header, and with it pipeline.hpp, does not include <windows.h>: a
// module that also includes <dsptb/sdk.hpp> would hit its own declaration of RaiseException.
#pragma once

#include <cstdint>

namespace dsptb::gpu {

// Raises the calling thread with MMCSS to the "Audio" task, one priority step below the audio callback thread
// (which uses Pro Audio). Failure is harmless: the thread just keeps its normal priority.
class MmcssScope {
public:
    MmcssScope();
    ~MmcssScope();
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    void* handle_ = nullptr;
};

// A waitable timer that can sleep for about a millisecond. The high-resolution flavour (Windows 10 1803 and later)
// is exact; without it the timer is as coarse as the system tick.
class HighResTimer {
public:
    HighResTimer();
    ~HighResTimer();
    HighResTimer(const HighResTimer&) = delete;
    HighResTimer& operator=(const HighResTimer&) = delete;

    // Sleeps about `microseconds` (at least one timer tick).
    void wait(int64_t microseconds);

private:
    void* handle_ = nullptr;
};

// Microseconds on the QPC clock, for timing one inference.
int64_t nowMicros();

} // namespace dsptb::gpu
