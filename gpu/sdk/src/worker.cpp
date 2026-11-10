#include <dsptb/gpu/worker.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <avrt.h>
// clang-format on

namespace dsptb::gpu {

MmcssScope::MmcssScope() {
    DWORD index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    if (h)
        AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
    handle_ = h;
}

MmcssScope::~MmcssScope() {
    if (handle_)
        AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle_));
}

HighResTimer::HighResTimer() {
    HANDLE h =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!h)
        h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    handle_ = h;
}

HighResTimer::~HighResTimer() {
    if (handle_)
        CloseHandle(static_cast<HANDLE>(handle_));
}

void HighResTimer::wait(int64_t microseconds) {
    if (!handle_) {
        Sleep(1);
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -10 * microseconds; // relative, in 100 ns units
    if (SetWaitableTimer(static_cast<HANDLE>(handle_), &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(static_cast<HANDLE>(handle_), 50);
    else
        Sleep(1);
}

int64_t nowMicros() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart) * 1000000 / freq;
}

} // namespace dsptb::gpu
