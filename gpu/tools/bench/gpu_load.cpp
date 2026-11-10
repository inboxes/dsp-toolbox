#include "gpu_load.hpp"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <torch/torch.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dsptb::gpu {

GpuLoad::GpuLoad(int device, int percent) {
    const int duty = std::clamp(percent, 1, 100);
    thread_ = std::thread([this, device, duty] {
        try {
            c10::InferenceMode inference;
            const torch::Device dev(torch::kCUDA, static_cast<c10::DeviceIndex>(device));
            c10::cuda::CUDAGuard deviceGuard(static_cast<c10::DeviceIndex>(device));
            // Lowest priority, so it competes like a different program would, not like part of the pipeline.
            c10::cuda::CUDAStreamGuard streamGuard(c10::cuda::getStreamFromPool(false, static_cast<c10::DeviceIndex>(device)));
            auto a = torch::randn({3072, 3072}, torch::TensorOptions().device(dev));
            auto b = torch::randn({3072, 3072}, torch::TensorOptions().device(dev));
            // How long one product takes on an otherwise idle GPU.
            for (int i = 0; i < 5; ++i) torch::mm(a, b);
            torch::cuda::synchronize();
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i) torch::mm(a, b);
            torch::cuda::synchronize();
            const double productMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
            constexpr double kPeriodMs = 20.0;
            const int perPeriod = std::max(1, static_cast<int>(std::lround(kPeriodMs * duty / 100.0 / std::max(0.05, productMs))));
            while (!stop_.load()) {
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < perPeriod; ++i) {
                    torch::mm(a, b);
                    products_.fetch_add(1);
                }
                torch::cuda::synchronize();
                const double used = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (used < kPeriodMs) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(kPeriodMs - used));
            }
        } catch (const std::exception&) {
            // The load is best effort; the bench reports products() == 0.
        }
    });
}

GpuLoad::~GpuLoad() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

} // namespace dsptb::gpu
