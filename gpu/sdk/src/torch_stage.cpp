#include <dsptb/gpu/runtime_api.hpp>
#include <dsptb/gpu/torch_stage.hpp>

#include <ATen/Parallel.h>
#include <ATen/cuda/CUDAGraph.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <torch/script.h>

#include <chrono>
#include <cstring>
#include <mutex>

namespace dsptb::gpu {

namespace {

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Settings that apply to the whole process, once.
void globalSettings(bool benchmark) {
    static std::once_flag once;
    std::call_once(once, [&] {
        at::set_num_threads(1);
        try {
            at::set_num_interop_threads(1); // throws when LibTorch already started its pool
        } catch (const c10::Error&) {
        }
        // The profiling executor would re-specialise the graph after capture.
        torch::jit::setGraphExecutorOptimize(false);
        at::globalContext().setBenchmarkCuDNN(benchmark);
    });
}

torch::ScalarType scalarType(TensorType t) {
    return t == TensorType::Float32 ? torch::kFloat32 : torch::kInt64;
}

} // namespace

struct TorchModel::Impl {
    std::string path;
    std::vector<TensorSpec> inputSpecs;
    TorchOptions options;

    torch::jit::script::Module module;
    std::vector<torch::Tensor> staticIn;
    std::vector<torch::Tensor> outputs; // the graph's static outputs, or the latest eager ones
    std::vector<PinnedBuffer> hIn, hOut;
    std::unique_ptr<at::cuda::CUDAGraph> graph;
    bool graphed = false;
    double steadyMs = 0;

    std::vector<torch::jit::IValue> inputValues() const {
        std::vector<torch::jit::IValue> v;
        for (const torch::Tensor& t : staticIn)
            v.emplace_back(t);
        return v;
    }
    static void collect(const torch::jit::IValue& r, std::vector<torch::Tensor>& out) {
        out.clear();
        if (r.isTensor())
            out.push_back(r.toTensor());
        else if (r.isTuple())
            for (const auto& e : r.toTuple()->elements())
                out.push_back(e.toTensor());
        else
            throw std::runtime_error("the TorchScript model returns neither a tensor nor a tuple of tensors");
    }
    void forwardEager() {
        collect(module.forward(inputValues()), outputs);
        for (torch::Tensor& t : outputs)
            t = t.contiguous();
    }
    void copyOutputs(CudaStream& stream) {
        for (size_t i = 0; i < outputs.size(); ++i)
            DSPTB_CUDA_CHECK(cudaMemcpyAsync(hOut[i].data(), outputs[i].data_ptr(), hOut[i].bytes(),
                                             cudaMemcpyDeviceToHost, stream.get()));
        stream.synchronize();
    }
    void runOnce(CudaStream& stream) {
        c10::InferenceMode inference;
        c10::cuda::CUDAStreamGuard guard(
            c10::cuda::getStreamFromExternal(stream.get(), static_cast<c10::DeviceIndex>(stream.device())));
        for (size_t i = 0; i < staticIn.size(); ++i)
            DSPTB_CUDA_CHECK(cudaMemcpyAsync(staticIn[i].data_ptr(), hIn[i].data(), hIn[i].bytes(),
                                             cudaMemcpyHostToDevice, stream.get()));
        if (graphed)
            graph->replay();
        else
            forwardEager();
        copyOutputs(stream);
    }
};

TorchModel::TorchModel(std::string path, std::vector<TensorSpec> inputs, TorchOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->inputSpecs = std::move(inputs);
    impl_->options = options;
}

TorchModel::~TorchModel() = default;

void TorchModel::prepare(CudaStream& stream) {
    Impl& m = *impl_;
    try {
        DSPTB_CUDA_CHECK(cudaSetDevice(stream.device()));
        globalSettings(m.options.cudnnBenchmark);
        c10::InferenceMode inference;
        const torch::Device device(torch::kCUDA, static_cast<c10::DeviceIndex>(stream.device()));
        if (m.options.loadOnDevice) {
            m.module = torch::jit::load(m.path, device);
        } else {
            m.module = torch::jit::load(m.path, torch::Device(torch::kCPU));
            m.module.to(device);
        }
        m.module.eval();
        for (const TensorSpec& t : m.inputSpecs) {
            m.staticIn.push_back(
                torch::zeros(t.shape, torch::TensorOptions().dtype(scalarType(t.type)).device(device)));
            m.hIn.emplace_back(t.bytes());
            std::memset(m.hIn.back().data(), 0, t.bytes());
        }
        c10::cuda::CUDAStreamGuard guard(
            c10::cuda::getStreamFromExternal(stream.get(), static_cast<c10::DeviceIndex>(stream.device())));
        for (int i = 0; i < 5; ++i)
            m.forwardEager();
        stream.synchronize();
        for (const torch::Tensor& t : m.outputs)
            m.hOut.emplace_back(static_cast<size_t>(t.nbytes()));

        if (m.options.graph) {
            try {
                ScopedCapture
                    capture; // only the capture itself, so other workers keep running while a model loads
                m.graph = std::make_unique<at::cuda::CUDAGraph>();
                m.graph->capture_begin(at::cuda::MempoolId_t{0, 0}, cudaStreamCaptureModeThreadLocal);
                m.forwardEager(); // the outputs are now the graph's static buffers
                m.graph->capture_end();
                stream.synchronize();
                m.graphed = true;
            } catch (const c10::Error& e) {
                logMessage(2, "LibTorch stage " + m.path + ": CUDA graph unavailable (" +
                                  e.what_without_backtrace() + "), running without");
                m.graph.reset();
                m.graphed = false;
                m.forwardEager();
            }
        }
        // Timing of a warmed-up run, for the log.
        m.runOnce(stream);
        const double t0 = nowMs();
        m.runOnce(stream);
        m.steadyMs = nowMs() - t0;
        logMessage(0, "LibTorch stage " + m.path + (m.graphed ? ": graphed" : ": not graphed") +
                          ", steady run " + std::to_string(m.steadyMs) + " ms");
    } catch (const c10::Error& e) {
        throw std::runtime_error(std::string("LibTorch: ") + e.what_without_backtrace());
    }
}

void* TorchModel::hostInput(size_t i) {
    return impl_->hIn.at(i).data();
}
size_t TorchModel::outputCount() const {
    return impl_->hOut.size();
}
size_t TorchModel::outputBytes(size_t i) const {
    return impl_->hOut.at(i).bytes();
}
const void* TorchModel::hostOutput(size_t i) const {
    return impl_->hOut.at(i).data();
}

void TorchModel::run(CudaStream& stream) {
    try {
        impl_->runOnce(stream);
    } catch (const c10::Error& e) {
        throw std::runtime_error(std::string("LibTorch: ") + e.what_without_backtrace());
    }
}

bool TorchModel::graphed() const {
    return impl_->graphed;
}
double TorchModel::steadyRunMs() const {
    return impl_->steadyMs;
}

// ---- TorchStage ----

TorchStage::TorchStage(std::string modelPath, uint32_t windowFrames, uint32_t hopFrames, TorchOptions options)
    : path_(std::move(modelPath)), window_(windowFrames), hop_(hopFrames), options_(options) {}

TorchStage::~TorchStage() = default;

void TorchStage::prepare() {
    const int device = selectedDevice();
    if (device < 0)
        throw std::runtime_error("the GPU runtime is not available");
    stream_ = std::make_unique<CudaStream>(device);
    model_ = std::make_unique<TorchModel>(
        path_,
        std::vector<TensorSpec>{TensorSpec{"x", {1, 1, static_cast<int64_t>(window_)}, TensorType::Float32}},
        options_);
    model_->prepare(*stream_);
    if (model_->outputCount() != 1 || model_->outputBytes(0) != sizeof(float) * hop_)
        throw std::runtime_error("TorchStage: the model output is not [1, 1, hop] floats");
}

void TorchStage::run(const float* window, uint32_t windowFrames, float* out, uint32_t hopFrames) {
    if (windowFrames != window_ || hopFrames != hop_)
        throw std::runtime_error("TorchStage: window or hop differs from the model shape");
    std::memcpy(model_->hostInput(0), window, sizeof(float) * window_);
    model_->run(*stream_);
    std::memcpy(out, model_->hostOutput(0), sizeof(float) * hop_);
}

bool TorchStage::graphed() const {
    return model_ && model_->graphed();
}

void TorchStage::teardown() {
    model_.reset(); // before the stream, on the thread that created them
    stream_.reset();
}

} // namespace dsptb::gpu
