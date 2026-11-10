// LibTorch (TorchScript) on CUDA, with CUDA graphs (docs/GPU_RUNTIME.md section 7.3). No LibTorch header is
// needed to use these classes. Models are TorchScript files made with torch.jit.trace (decision S2:
// AOTInductor does not work on Windows here).
#pragma once

#include <dsptb/gpu/cuda_util.hpp>
#include <dsptb/gpu/ort_stage.hpp> // TensorSpec, TensorType
#include <dsptb/gpu/pipeline.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dsptb::gpu {

struct TorchOptions {
    bool graph = true;          // CUDA graph; falls back to eager (with one warning) if capture fails
    bool cudnnBenchmark = true; // cuDNN algorithm search (decision S3), only costs time in prepare()
    // Load the file straight onto the GPU instead of loading on the CPU and moving it. The default (CPU
    // first) keeps the traced shape constants on the CPU, which graph capture needs. A model with GRU/LSTM
    // layers needs this setting instead: moving it with module.to() breaks the contiguous weight block cuDNN
    // wants (a warning and a copy on every call). Export such a model after flatten_parameters(), and make
    // sure it has no traced shape arithmetic.
    bool loadOnDevice = false;
};

// One TorchScript model with fixed input shapes. forward() takes the inputs in order and returns a tensor or
// a tuple of tensors; every output is copied to pinned host memory after each run. Worker thread only.
class TorchModel {
public:
    TorchModel(std::string path, std::vector<TensorSpec> inputs, TorchOptions options = {});
    ~TorchModel();
    TorchModel(const TorchModel&) = delete;
    TorchModel& operator=(const TorchModel&) = delete;

    // Loads on the CPU and then moves to the device (shape constants must stay on the CPU, or `int(tensor)`
    // syncs the GPU inside capture), allocates the static tensors, warms up on the stream and captures the
    // graph. Hold the capture lock exclusively. Throws std::runtime_error (c10::Error is converted).
    void prepare(CudaStream& stream);

    void* hostInput(size_t i);
    size_t outputCount() const;
    size_t outputBytes(size_t i) const;
    const void* hostOutput(size_t i) const;
    void run(CudaStream& stream);

    bool graphed() const;
    double steadyRunMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A Stage with one audio input [1, 1, window] and one audio output [1, 1, hop].
class TorchStage : public Stage {
public:
    TorchStage(std::string modelPath, uint32_t windowFrames, uint32_t hopFrames, TorchOptions options = {});
    ~TorchStage() override;
    void prepare() override;
    void run(const float* window, uint32_t windowFrames, float* out, uint32_t hopFrames) override;
    bool graphed() const override;
    void teardown() override;

private:
    std::string path_;
    uint32_t window_, hop_;
    TorchOptions options_;
    std::unique_ptr<CudaStream> stream_;
    std::unique_ptr<TorchModel> model_;
};

} // namespace dsptb::gpu
