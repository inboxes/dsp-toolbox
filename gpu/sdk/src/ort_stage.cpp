#include <dsptb/gpu/ort_stage.hpp>
#include <dsptb/gpu/runtime_api.hpp>

#include <onnxruntime_cxx_api.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>

namespace dsptb::gpu {

namespace {

// One Ort::Env per process, created on first use.
Ort::Env& ortEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "dsptb-gpu");
    return env;
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct OrtModel::Impl {
    std::string path;
    std::vector<TensorSpec> inputs, outputs;
    OrtOptions options;

    std::vector<DeviceBuffer> dIn, dOut;
    std::vector<PinnedBuffer> hIn, hOut;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::IoBinding> binding;
    bool graphed = false;
    double firstMs = 0, steadyMs = 0;

    std::unique_ptr<Ort::Session> makeSession(CudaStream& stream, bool graph) {
        const OrtApi& api = Ort::GetApi();
        OrtCUDAProviderOptionsV2* opt = nullptr;
        Ort::ThrowOnError(api.CreateCUDAProviderOptions(&opt));
        const std::string device = std::to_string(stream.device());
        std::vector<const char*> keys{"device_id", "cudnn_conv_algo_search", "do_copy_in_default_stream"};
        std::vector<const char*> values{device.c_str(), options.exhaustiveSearch ? "EXHAUSTIVE" : "HEURISTIC", "1"};
        if (graph) {
            keys.push_back("enable_cuda_graph");
            values.push_back("1");
        }
        Ort::ThrowOnError(api.UpdateCUDAProviderOptions(opt, keys.data(), values.data(), keys.size()));
        // Our stream orders the copies, the run and the graph replay. S1 showed it works together with graphs.
        Ort::ThrowOnError(api.UpdateCUDAProviderOptionsWithValue(opt, "user_compute_stream", stream.get()));
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.DisableCpuMemArena();
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // CPU fallback stays allowed: real models keep a few shape-handling nodes on the CPU (ORT puts them there on purpose)
        // and graph capture still works with them. A model that really cannot be captured fails in the first run and
        // prepare() then falls back to a session without graphs.
        const OrtStatus* st = api.SessionOptionsAppendExecutionProvider_CUDA_V2(so, opt);
        api.ReleaseCUDAProviderOptions(opt);
        Ort::ThrowOnError(const_cast<OrtStatus*>(st));
        const std::filesystem::path p{std::u8string(path.begin(), path.end())};
        return std::make_unique<Ort::Session>(ortEnv(), p.c_str(), so);
    }

    void bindAll() {
        binding = std::make_unique<Ort::IoBinding>(*session);
        const Ort::MemoryInfo cuda("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        for (size_t i = 0; i < inputs.size(); ++i) bindOne(cuda, inputs[i], dIn[i], true);
        for (size_t i = 0; i < outputs.size(); ++i) bindOne(cuda, outputs[i], dOut[i], false);
    }

    void bindOne(const Ort::MemoryInfo& mem, const TensorSpec& t, DeviceBuffer& buf, bool input) {
        Ort::Value v = t.type == TensorType::Float32
                           ? Ort::Value::CreateTensor<float>(mem, static_cast<float*>(buf.data()), t.elements(), t.shape.data(), t.shape.size())
                           : Ort::Value::CreateTensor<int64_t>(mem, static_cast<int64_t*>(buf.data()), t.elements(), t.shape.data(), t.shape.size());
        if (input) binding->BindInput(t.name.c_str(), v);
        else binding->BindOutput(t.name.c_str(), v);
    }

    void runOnce(CudaStream& stream) {
        for (size_t i = 0; i < inputs.size(); ++i)
            DSPTB_CUDA_CHECK(cudaMemcpyAsync(dIn[i].data(), hIn[i].data(), inputs[i].bytes(), cudaMemcpyHostToDevice, stream.get()));
        session->Run(Ort::RunOptions{nullptr}, *binding);
        for (size_t i = 0; i < outputs.size(); ++i)
            DSPTB_CUDA_CHECK(cudaMemcpyAsync(hOut[i].data(), dOut[i].data(), outputs[i].bytes(), cudaMemcpyDeviceToHost, stream.get()));
        stream.synchronize();
    }
};

OrtModel::OrtModel(std::string path, std::vector<TensorSpec> inputs, std::vector<TensorSpec> outputs, OrtOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->inputs = std::move(inputs);
    impl_->outputs = std::move(outputs);
    impl_->options = options;
}

OrtModel::~OrtModel() = default;

void OrtModel::prepare(CudaStream& stream) {
    Impl& m = *impl_;
    DSPTB_CUDA_CHECK(cudaSetDevice(stream.device()));
    for (const TensorSpec& t : m.inputs) {
        m.dIn.emplace_back(t.bytes());
        m.hIn.emplace_back(t.bytes());
        std::memset(m.hIn.back().data(), 0, t.bytes());
    }
    for (const TensorSpec& t : m.outputs) {
        m.dOut.emplace_back(t.bytes());
        m.hOut.emplace_back(t.bytes());
    }
    bool graphWanted = true;
    auto warmUp = [&] {
        m.bindAll();
        const double t0 = nowMs();
        {
            // With graphs on, ORT captures on this first run. Everything before it (session creation, loading the model,
            // allocating) runs without the lock, so other workers keep going.
            std::optional<ScopedCapture> capture;
            if (m.options.graph && graphWanted) capture.emplace();
            m.runOnce(stream);
        }
        m.firstMs = nowMs() - t0;
        for (int i = 0; i < 2; ++i) m.runOnce(stream);
        const double t1 = nowMs();
        m.runOnce(stream);
        m.steadyMs = nowMs() - t1;
    };
    bool graph = m.options.graph;
    if (graph) {
        graphWanted = true;
        try {
            m.session = m.makeSession(stream, true);
            warmUp();
            m.graphed = true;
        } catch (const Ort::Exception& e) {
            logMessage(2, "ORT stage " + m.path + ": CUDA graph unavailable (" + e.what() + "), running without");
            m.binding.reset();
            m.session.reset();
            graph = false;
        }
    }
    if (!graph) {
        graphWanted = false;
        m.session = m.makeSession(stream, false);
        warmUp();
        m.graphed = false;
    }
    logMessage(0, "ORT stage " + m.path + (m.graphed ? ": graphed" : ": not graphed") + ", first run " + std::to_string(m.firstMs) +
                      " ms, steady " + std::to_string(m.steadyMs) + " ms");
}

void* OrtModel::hostInput(size_t i) { return impl_->hIn.at(i).data(); }
const void* OrtModel::hostOutput(size_t i) const { return impl_->hOut.at(i).data(); }

void OrtModel::run(CudaStream& stream) {
    try {
        impl_->runOnce(stream);
    } catch (const Ort::Exception& e) {
        throw std::runtime_error(std::string("ONNX Runtime: ") + e.what());
    }
}

bool OrtModel::graphed() const { return impl_->graphed; }
double OrtModel::firstRunMs() const { return impl_->firstMs; }
double OrtModel::steadyRunMs() const { return impl_->steadyMs; }

// ---- OrtStage ----

OrtStage::OrtStage(std::string modelPath, uint32_t windowFrames, uint32_t hopFrames, OrtOptions options)
    : path_(std::move(modelPath)), window_(windowFrames), hop_(hopFrames), options_(options) {}

OrtStage::~OrtStage() = default;

void OrtStage::prepare() {
    const int device = selectedDevice();
    if (device < 0) throw std::runtime_error("the GPU runtime is not available");
    stream_ = std::make_unique<CudaStream>(device);
    TensorSpec in{"x", {1, 1, static_cast<int64_t>(window_)}, TensorType::Float32};
    TensorSpec out{"y", {1, 1, static_cast<int64_t>(hop_)}, TensorType::Float32};
    model_ = std::make_unique<OrtModel>(path_, std::vector<TensorSpec>{in}, std::vector<TensorSpec>{out}, options_);
    model_->prepare(*stream_);
}

void OrtStage::run(const float* window, uint32_t windowFrames, float* out, uint32_t hopFrames) {
    if (windowFrames != window_ || hopFrames != hop_) throw std::runtime_error("OrtStage: window or hop differs from the model shape");
    std::memcpy(model_->hostInput(0), window, sizeof(float) * window_);
    model_->run(*stream_);
    std::memcpy(out, model_->hostOutput(0), sizeof(float) * hop_);
}

bool OrtStage::graphed() const { return model_ && model_->graphed(); }

void OrtStage::teardown() {
    model_.reset(); // before the stream, on the thread that created them
    stream_.reset();
}

} // namespace dsptb::gpu
