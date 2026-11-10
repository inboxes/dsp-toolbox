// ONNX Runtime on the CUDA execution provider, with CUDA graphs (docs/GPU_RUNTIME.md section 7.2). No ORT header is
// needed to use these classes.
#pragma once

#include <dsptb/gpu/cuda_util.hpp>
#include <dsptb/gpu/pipeline.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dsptb::gpu {

enum class TensorType { Float32, Int64 };

struct TensorSpec {
    std::string name;
    std::vector<int64_t> shape; // every dimension fixed
    TensorType type = TensorType::Float32;
    size_t elements() const {
        size_t n = 1;
        for (int64_t d : shape) n *= static_cast<size_t>(d);
        return n;
    }
    size_t bytes() const { return elements() * (type == TensorType::Float32 ? 4u : 8u); }
};

struct OrtOptions {
    bool graph = true;             // CUDA graph capture; falls back (with one warning) when the model cannot be graphed
    bool exhaustiveSearch = true;  // cuDNN algorithm search (the default chosen by measurement, see docs/GPU_RUNTIME.md section 10); only costs time in prepare()
};

// One ONNX model with fixed shapes: device buffers bound once, pinned host staging buffers, one run() per call.
// Use it from the worker thread only: prepare() and run() on the same thread.
class OrtModel {
public:
    OrtModel(std::string path, std::vector<TensorSpec> inputs, std::vector<TensorSpec> outputs, OrtOptions options = {});
    ~OrtModel();
    OrtModel(const OrtModel&) = delete;
    OrtModel& operator=(const OrtModel&) = delete;

    // Creates the session on the CUDA EP, binds the buffers, warms up (3 runs) and lets ORT capture the graph.
    // Hold the capture lock exclusively (Pipeline::prepareStage does). Throws std::runtime_error on failure.
    void prepare(CudaStream& stream);

    void* hostInput(size_t i);              // pinned; fill, then run()
    const void* hostOutput(size_t i) const; // pinned; valid after run()
    // Host to device copies, the run, device to host copies, then waits for the stream.
    void run(CudaStream& stream);

    bool graphed() const;                   // false when prepare() fell back, or graphs were not wanted
    double firstRunMs() const;              // the run that captured the graph, for the log
    double steadyRunMs() const;             // a warmed-up run

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A Stage with one audio input [1, 1, window] and one audio output [1, 1, hop]; input "x", output "y".
class OrtStage : public Stage {
public:
    OrtStage(std::string modelPath, uint32_t windowFrames, uint32_t hopFrames, OrtOptions options = {});
    ~OrtStage() override;
    void prepare() override;
    void run(const float* window, uint32_t windowFrames, float* out, uint32_t hopFrames) override;
    bool graphed() const override;
    void teardown() override;

private:
    std::string path_;
    uint32_t window_, hop_;
    OrtOptions options_;
    std::unique_ptr<CudaStream> stream_;
    std::unique_ptr<OrtModel> model_;
};

} // namespace dsptb::gpu
