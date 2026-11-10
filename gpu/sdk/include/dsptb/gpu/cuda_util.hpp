// CUDA helpers shared by the ORT and LibTorch stages (docs/GPU_RUNTIME.md section 7.4).
#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace dsptb::gpu {

// Throws on any CUDA error. A context-killing error (a lost device, a launch failure) shows up in the text; the
// pipeline then goes to Failed and only a re-prepare recovers.
inline void cudaCheck(cudaError_t e, const char* call, const char* file, int line) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("CUDA ") + call + " failed: " + cudaGetErrorString(e) + " at " + file + ":" +
                                 std::to_string(line));
}
#define DSPTB_CUDA_CHECK(expr) ::dsptb::gpu::cudaCheck((expr), #expr, __FILE__, __LINE__)

// One stream per pipeline, created with the highest priority. Priority helps against other CUDA work in this
// process; it does not prioritise us over another process's Direct3D work.
class CudaStream {
public:
    explicit CudaStream(int device);
    ~CudaStream();
    CudaStream(const CudaStream&) = delete;
    CudaStream& operator=(const CudaStream&) = delete;

    cudaStream_t get() const { return stream_; }
    int device() const { return device_; }
    // Waits until everything queued on the stream so far is done: an event recorded after the last copy, then
    // cudaEventSynchronize. The event spins or sleeps like the runtime's sync mode (gpu.sync).
    void synchronize();

private:
    int device_;
    cudaStream_t stream_ = nullptr;
    cudaEvent_t event_ = nullptr;
};

// Page-locked host memory, for copies that really are asynchronous.
class PinnedBuffer {
public:
    PinnedBuffer() = default;
    explicit PinnedBuffer(size_t bytes) { allocate(bytes); }
    ~PinnedBuffer() { release(); }
    PinnedBuffer(const PinnedBuffer&) = delete;
    PinnedBuffer& operator=(const PinnedBuffer&) = delete;
    PinnedBuffer(PinnedBuffer&& o) noexcept : ptr_(o.ptr_), bytes_(o.bytes_) { o.ptr_ = nullptr; o.bytes_ = 0; }
    PinnedBuffer& operator=(PinnedBuffer&& o) noexcept {
        if (this != &o) {
            release();
            ptr_ = o.ptr_;
            bytes_ = o.bytes_;
            o.ptr_ = nullptr;
            o.bytes_ = 0;
        }
        return *this;
    }
    void allocate(size_t bytes) {
        release();
        DSPTB_CUDA_CHECK(cudaHostAlloc(&ptr_, bytes, cudaHostAllocDefault));
        bytes_ = bytes;
    }
    void release() noexcept {
        if (ptr_) cudaFreeHost(ptr_);
        ptr_ = nullptr;
        bytes_ = 0;
    }
    void* data() const { return ptr_; }
    size_t bytes() const { return bytes_; }

private:
    void* ptr_ = nullptr;
    size_t bytes_ = 0;
};

// Device memory, freed on destruction.
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    explicit DeviceBuffer(size_t bytes) { allocate(bytes); }
    ~DeviceBuffer() { release(); }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    DeviceBuffer(DeviceBuffer&& o) noexcept : ptr_(o.ptr_), bytes_(o.bytes_) { o.ptr_ = nullptr; o.bytes_ = 0; }
    DeviceBuffer& operator=(DeviceBuffer&& o) noexcept {
        if (this != &o) {
            release();
            ptr_ = o.ptr_;
            bytes_ = o.bytes_;
            o.ptr_ = nullptr;
            o.bytes_ = 0;
        }
        return *this;
    }
    void allocate(size_t bytes) {
        release();
        DSPTB_CUDA_CHECK(cudaMalloc(&ptr_, bytes));
        bytes_ = bytes;
    }
    void release() noexcept {
        if (ptr_) cudaFree(ptr_);
        ptr_ = nullptr;
        bytes_ = 0;
    }
    void* data() const { return ptr_; }
    size_t bytes() const { return bytes_; }

private:
    void* ptr_ = nullptr;
    size_t bytes_ = 0;
};

} // namespace dsptb::gpu
