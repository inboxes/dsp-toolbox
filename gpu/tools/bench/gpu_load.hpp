// A background GPU load for dsptb-gpu-bench --gpu-load: large matrix multiplications on their own stream, in
// bursts, so the GPU is busy about N percent of the time. It stands in for another program using the GPU (a
// game is the real test).
#pragma once

#include <atomic>
#include <memory>
#include <thread>

namespace dsptb::gpu {

class GpuLoad {
public:
    // percent 1..100 of the time the GPU should be busy. device: the CUDA device to load.
    GpuLoad(int device, int percent);
    ~GpuLoad();
    GpuLoad(const GpuLoad&) = delete;
    GpuLoad& operator=(const GpuLoad&) = delete;
    // How many matrix products the load thread has finished (to show that it ran).
    unsigned long long products() const { return products_.load(); }

private:
    std::atomic<bool> stop_{false};
    std::atomic<unsigned long long> products_{0};
    std::thread thread_;
};

} // namespace dsptb::gpu
