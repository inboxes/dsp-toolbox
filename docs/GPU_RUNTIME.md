# GPU runtime: ONNX Runtime + LibTorch on CUDA, with CUDA graphs (M10)

Status: **built and measured 2026-11-09** (M10; see section 15 for what the build changed). Replaces an earlier plan for a D3D11-compute module, which is deferred.

> **Read this first.** This milestone gives modules a way to run neural networks on an NVIDIA GPU in real time. It does **not** build any particular neural module; those come later, each with its own spec. This work ends when a test module proves the whole path, and a C++ benchmark reproduces the maintainer's Python measurements on a reference chain of three networks (§1).
>
> Three rules matter more than everything else in this file:
> 1. **The GPL host never includes or links CUDA, ONNX Runtime or LibTorch.** All of that lives in `gpu/` (MIT), and the host talks to it only through the small C header in §5.
> 2. **No GPU call ever runs on the audio thread.** `process()` only moves samples through lock-free rings. Inference runs on a worker thread, with a fixed, declared latency (§7).
> 3. **Measure before building (§10).** Three questions were answered by measurement before any production code was written; §10 records the answers.

## 1. Purpose

The maintainer's Python measurements of a reference chain of three speech networks (RTX 3090, 2026-11-08, a 200 ms window) showed:
- Eager PyTorch: 29.5 ms per 200 ms window on the RTX 3090. **ONNX Runtime: 16.9 ms. ORT + CUDA graphs: 15.9 ms** (one of the three networks couldn't be graphed). **PyTorch + CUDA graphs: 13.6 ms.**
- The cost is **kernel-launch overhead**, not arithmetic: it barely changes with window length, and fp16 doesn't help. CUDA graphs remove most of that overhead.
- The latency target for a neural voice effect is **under 60 ms**, and it should use **as little GPU as possible**, so a game can run beside it.

So the app needs both runtimes (ONNX Runtime for portability, LibTorch for the fastest graphed path), with CUDA graphs on both, and a real-time-safe way to feed them audio.

## 2. Decisions already made (2026-11-09)

| Topic | Decision |
|---|---|
| Target hardware | **NVIDIA CUDA only** for now (no AMD GPU to test on). Reference machine: RTX 3090 (compute capability 8.6), Ryzen 5 7600X. |
| Runtimes | **ONNX Runtime (CUDA EP)** and **LibTorch (CUDA)**, both with **CUDA graphs**. Both approved as dependencies. |
| Where the code lives | A **separate GPU runtime** (`gpu/` folder, **MIT**). It holds every piece that touches CUDA, ORT or LibTorch. The GPL host loads one small DLL from it (`dsptb_gpu.dll`) through a C ABI. |
| How modules use it | **Modules link ORT and LibTorch directly** (their C/C++ APIs), from the shared runtime folder. M10 gives them a helper library (`dsptb::gpu_sdk`, MIT) for the hard parts: the inference thread, the latency pipeline, graph capture and error handling. |
| Distribution | **Dev-only for now.** CMake downloads pinned, checksum-verified official archives for local builds. Public distribution is designed later, after the first GPU module is proven. Nothing from the runtime goes into the app zip or a release. |
| Old M10 (D3D11 compute spectrogram/CQT) | **Deferred.** |

## 3. Background (why the design looks like this)

You don't need to read the sources to build this. This explains the choices.

- **CUDA graphs.** A model runs as hundreds of small GPU kernels. Launching each one from the CPU costs a few microseconds, and these networks are dominated by that cost. A CUDA graph records the whole sequence of launches once ("capture") and replays it with one call. Requirements: fixed tensor shapes, fixed memory addresses for inputs and outputs, and no CPU work or memory allocation inside the captured region. You change the input by copying new data **into the same buffer**, then replay.
- **ONNX Runtime CUDA graphs.** Enabled with the CUDA EP option `enable_cuda_graph=1`. It works only when **every node** runs on the CUDA EP, and inputs/outputs must be bound once with IOBinding to fixed device buffers. The first `Run()` captures, and later `Run()`s replay. A model with nodes on the CPU can't be graphed as is; one of the test networks was like that.
- **LibTorch CUDA graphs.** `at::cuda::CUDAGraph`: `capture_begin()`, run the model, `capture_end()`, then `replay()`. Capture must happen on a non-default stream, after a few warm-up runs.
- **LibTorch model format.** TorchScript (`torch.jit.trace`/`script`, loaded with `torch::jit::load`) is **deprecated** in favour of `torch.export` + **AOTInductor** (`.pt2`, loaded with `torch::inductor::AOTIModelPackageLoader`, LibTorch 2.9+). AOTInductor with CUDA **on Windows** isn't clearly supported (it needs a working Inductor/Triton toolchain at export time). **Outcome of the format test (2026-11-09, §10): TorchScript.** AOTInductor failed to export here (`TritonMissing`: Triton is not available on Windows, and adding it would be a new dependency); TorchScript with a CUDA graph works and matches Python exactly.
- **One CUDA major version.** ORT GPU packages are built with CUDA 13.0 by default from ORT 1.27 on. LibTorch has Windows release zips for cu130. Both load `cudnn64_9.dll` and cuBLAS by the same file names, so **one process can only hold one copy**. Everything in the runtime uses **CUDA 13.0**. The Python test setup mixed ORT 1.23 (CUDA 12) with torch cu130; that worked only because they were never loaded into one process together. Ampere (sm_86) is supported by CUDA 13.
- **DLL loading on Windows.** cuDNN 9 loads its engine sub-libraries by name at run time, so they must be findable. The Python benchmark found that the **load order matters**: loading cuDNN's engine DLLs before nvrtc/nvJitLink fails with error 127. §6.2 gives the working order.
- **Debug builds.** PyTorch stopped publishing Windows **debug** LibTorch zips at 2.12.1. A module built with the debug CRT (`/MDd`) can't use the release LibTorch C++ API safely (STL types cross the boundary). So everything that links LibTorch builds as **Release** (`/MD`), even when the rest of the tree is Debug. ORT's C API doesn't have this problem, but we keep one rule for both (§8.4).
- **Blocking vs spinning.** By default CUDA spins a CPU core while waiting for the GPU. The Python ORT stream test used 45–91 % of one core. `cudaDeviceScheduleBlockingSync` sleeps instead, at the cost of some wake-up delay. §10 has the measurement.
- **Licensing.** The host is GPLv3 with a module exception (`MODULE-EXCEPTION.md`). CUDA, cuDNN and cuBLAS are proprietary NVIDIA libraries. Keeping every CUDA-linked binary out of the GPL host (in the MIT `gpu/` folder, reached through a C ABI) keeps the host clean. The exception text currently names only `plugin_abi.h`, so it needs one sentence for the new C header (§12, **the maintainer approves the wording**).

## 4. Overview

```
DSPToolbox.exe / dsptb-cli.exe (GPL, no CUDA)
   │  LoadLibrary("gpu-runtime/dsptb_gpu.dll") + GetProcAddress     (C ABI, §5)
   ▼
dsptb_gpu.dll (MIT)  ── preloads CUDA/cuDNN/ORT/LibTorch DLLs in order, adds the DLL directory,
   │                    sets device flags, reports device info, owns the capture lock (§6)
   │
   ├── module DLL that needs the GPU (e.g. test.gpu-probe, later org.dsptb.neuralvc)
   │      links onnxruntime.lib and/or torch*.lib directly, plus dsptb::gpu_sdk (MIT, §7)
   │      process()  ──SPSC ring──►  inference worker thread  ──SPSC ring──►  process()
   │                                   (ORT/LibTorch, CUDA graphs, high-priority stream)
   └── shared DLL folder: onnxruntime*.dll, torch*.dll, c10*.dll, cudart, cublas, cudnn*, …
```

Folder in the public repo (all MIT, with its own `gpu/LICENSE`):
```
gpu/
  LICENSE                       MIT
  README.md                     what this is, how to build it, the licence boundary
  include/dsptb/gpu_runtime.h   C ABI between the host and dsptb_gpu.dll (§5); pure C, no CUDA types
  runtime/                      dsptb_gpu.dll (§6)
  sdk/include/dsptb/gpu/        helper library for modules (§7): pipeline.hpp, worker.hpp, ort_stage.hpp, torch_stage.hpp, ...
  sdk/src/                      the parts of the helpers that need CUDA/ORT/LibTorch (static lib dsptb_gpu_sdk)
  tests/                        unit tests (CPU-only part runs in CI) and GPU tests (local only, label "gpu")
  test-module/                  test.gpu-probe (§9)
  tools/bench/                  dsptb-gpu-bench.exe (§9.3)
  tools/models/make_test_models.py   generates the tiny test models (committed, a few KB)
cmake/FetchGpuRuntime.cmake     downloads and stages the pinned archives (§8)
```

## 5. Host ↔ runtime C ABI (`gpu/include/dsptb/gpu_runtime.h`)

Plain C, MIT, no CUDA types. **Not** part of `plugin_abi.h`. The plugin ABI does not change in M10.

```c
/* dsptb/gpu_runtime.h — DSP Toolbox GPU runtime ABI 1. C99. MIT License. */
#pragma once
#include <stdint.h>
#define DSPTB_GPU_RUNTIME_ABI 1u

enum { DSPTB_GPU_OK = 0, DSPTB_GPU_NO_DRIVER = 1, DSPTB_GPU_NO_DEVICE = 2, DSPTB_GPU_DLL_MISSING = 3,
       DSPTB_GPU_VERSION_MISMATCH = 4, DSPTB_GPU_INIT_FAILED = 5 };
enum { DSPTB_GPU_SYNC_SPIN = 0, DSPTB_GPU_SYNC_BLOCKING = 1 };

typedef struct dsptb_gpu_device_info {
  uint32_t struct_size;
  int32_t  index;
  char     name[128];            /* UTF-8, e.g. "NVIDIA GeForce RTX 3090" */
  int32_t  cc_major, cc_minor;   /* compute capability */
  uint64_t vram_total_bytes, vram_free_bytes;
} dsptb_gpu_device_info;

typedef struct dsptb_gpu_runtime_info {
  uint32_t struct_size;
  uint32_t abi_version;          /* DSPTB_GPU_RUNTIME_ABI */
  int32_t  status;               /* DSPTB_GPU_* */
  char     status_text[256];     /* human-readable reason when status != OK */
  char     runtime_dir[520];     /* UTF-8 absolute path of the gpu-runtime folder */
  int32_t  driver_cuda_version;  /* cudaDriverGetVersion, e.g. 13000 */
  int32_t  runtime_cuda_version; /* cudaRuntimeGetVersion */
  int32_t  cudnn_version;        /* cudnnGetVersion */
  char     ort_version[32];      /* OrtGetApiBase()->GetVersionString() */
  char     torch_version[32];    /* from gpu-runtime.json (LibTorch has no C version call) */
  int32_t  device_count;
  int32_t  selected_device;      /* -1 when none */
  int32_t  sync_mode;            /* DSPTB_GPU_SYNC_* */
} dsptb_gpu_runtime_info;

/* All functions: any non-audio thread. Exported by dsptb_gpu.dll with __declspec(dllexport). */
int  dsptb_gpu_init(const char* runtime_dir_utf8, int32_t device_index, int32_t sync_mode, dsptb_gpu_runtime_info* out);
void dsptb_gpu_get_info(dsptb_gpu_runtime_info* out);           /* status after init (or NOT_INITIALIZED text) */
int  dsptb_gpu_device_info(int32_t index, dsptb_gpu_device_info* out);  /* 1 = ok */
int32_t dsptb_gpu_selected_device(void);                         /* modules call this; auto-inits with defaults */
/* Process-wide capture lock (§6.4). Workers hold it SHARED around each inference; graph capture holds it EXCLUSIVE. */
void dsptb_gpu_lock_shared(void);   void dsptb_gpu_unlock_shared(void);
void dsptb_gpu_lock_exclusive(void); void dsptb_gpu_unlock_exclusive(void);
void dsptb_gpu_shutdown(void);      /* host calls it once, after every module is destroyed */
```

- The host **only** uses `LoadLibraryExW` + `GetProcAddress` for these. It never links `dsptb_gpu.lib`.
- Modules do **not** import `dsptb_gpu.dll` either: `dsptb::gpu_sdk` resolves it at run time (`gpu/sdk/include/dsptb/gpu/runtime_api.hpp`) with `GetModuleHandleW` + `GetProcAddress`. The host (or, in a test tool, `loadRuntime()`) has already loaded it from the runtime folder, so the module gets the same instance, the same selected device and the same lock. An import library would make a module DLL or a tool fail to start whenever the runtime folder is not already on the DLL search path, and a tool in `gpu-runtime/bin/` could not find `dsptb_gpu.dll` one folder up.
- `dsptb_gpu_init` is idempotent. A second call with the same arguments returns the stored result. Different arguments after the first successful init are ignored and logged (the device can't change without a restart).
- `dsptb_gpu_selected_device()` calls `dsptb_gpu_init(<its own folder>, 0, SPIN, ...)` if nobody did yet, so a module also works under a host that never initialised the runtime (for example a third-party test harness).

## 6. `dsptb_gpu.dll` (runtime, MIT)

Links only `cudart` (CUDA runtime API), `cudnn` (for the version) and `onnxruntime` (for the version string). It does **not** create ORT sessions or load LibTorch.

### 6.1 Folder layout and manifest

At runtime the GPU runtime is the folder `<exe dir>/gpu-runtime/` (override: `settings.json` → `gpu.runtimeDir`).
```
gpu-runtime/
  gpu-runtime.json    versions, file list with SHA-256, licence files (written by FetchGpuRuntime.cmake)
  dsptb_gpu.dll
  bin/                every third-party DLL: onnxruntime*.dll, torch*.dll, c10*.dll, cudart64_13.dll, cublas*, cudnn*, ...
  licenses/           ORT LICENSE + ThirdPartyNotices, LibTorch LICENSE + NOTICE, NVIDIA licence texts
```
`gpu-runtime.json`:
```json
{ "schema": 1, "runtimeAbi": 1, "cuda": "13.0", "cudnn": "9.x.y", "onnxruntime": "1.xx.y", "libtorch": "2.xx.y+cu130",
  "files": [{"path": "bin/cudnn64_9.dll", "sha256": "..."}, ...] }
```

### 6.2 `dsptb_gpu_init` step by step

1. If `runtime_dir/gpu-runtime.json` is missing or unreadable → status `DLL_MISSING`, text `GPU runtime not found in <dir>`. Return.
2. Check `runtimeAbi == 1`. Otherwise `VERSION_MISMATCH`.
3. `AddDllDirectory(runtime_dir\bin)` (keep the cookie) and `SetEnvironmentVariableW("PATH", runtime_dir\bin + ";" + old PATH)` for this process. **Why both:** the host loads modules with `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, which includes `AddDllDirectory` folders; cuDNN loads its sub-libraries with a plain `LoadLibrary`, which searches `PATH`.
4. Preload these DLLs **in this order** with `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`, using the exact names the pinned archives contain (the coexistence test in §10 recorded them; CUDA 13 file names differ from the CUDA 12 ones in the Python test script):
   `cudart` → `cublasLt` → `cublas` → `nvrtc` → `nvJitLink` → `cufft` → `curand` → `cudnn` → `cudnn_graph` → `cudnn_engines_precompiled` → `cudnn_engines_runtime_compiled` → `cudnn_heuristic` → (any other `cudnn_*`) → `onnxruntime` → `onnxruntime_providers_shared`. **Not** `onnxruntime_providers_cuda.dll`: loading it ourselves fails with Windows error 1114 (§10); ORT loads it itself from next to `onnxruntime.dll`.
   LibTorch DLLs are **not** preloaded. Modules pull them in when they load.
   If one fails: status `DLL_MISSING`, text names the file and the Windows error. Return.
5. `cudaDriverGetVersion`. If 0 or below 13000 → `NO_DRIVER`, text: `NVIDIA driver too old for CUDA 13.0 (needs R580 or newer); found <x>`.
6. `cudaGetDeviceCount`. If 0 → `NO_DEVICE`. Clamp `device_index` to a valid device. Use device 0 by default.
7. `cudaSetDevice(selected)`. If `sync_mode == BLOCKING`, `cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync)`. **This must happen before anything creates the CUDA context**, so `dsptb_gpu_init` must run before any GPU module is loaded (§6.5).
8. Fill versions (cudart, cuDNN, ORT; LibTorch from the JSON). Status `OK`.
9. Log one INFO line through the out struct (the host logs it): `GPU: NVIDIA GeForce RTX 3090 (8.6, 24 GB), driver CUDA 13.0, cuDNN 9.x, ORT 1.xx, LibTorch 2.xx, sync spin`.

Wrap the whole function in `__try/__except` (a broken driver can crash inside `cudart`). On a structured exception: status `INIT_FAILED`, text `crash during GPU init (0x<code>)`.

### 6.3 Device selection and sync mode
Settings (`settings.json` → `gpu`): `runtimeDir` (default empty = `<exe dir>/gpu-runtime`), `device` (default 0), `sync` (`"spin"` or `"blocking"`, default chosen by the sync measurement in §10). Changes apply after a restart. The Settings UI says so.

### 6.4 Capture lock
Some CUDA calls are not allowed on **any** thread while another thread captures a graph in global mode (allocation, synchronous copies, some sync calls). ORT's capture mode is not under our control. So:
- Every inference iteration in every worker runs inside `dsptb_gpu_lock_shared()` / `unlock_shared()` (an SRW lock; shared mode is cheap and never blocks other workers).
- Every graph capture (ORT's first run with graphs on, or LibTorch `capture_begin`…`capture_end`) runs inside `lock_exclusive()` / `unlock_exclusive()`, taken by the stage itself (`ScopedCapture` in `runtime_api.hpp`). **Only the capture**, not the whole `prepare()`: holding the lock while a model loaded (hundreds of ms) made a running probe miss 585 hops during 20 re-prepares; with the lock around the capture only, it missed none.
- Result: while one module captures (only in `prepare` or after a config change), other GPU modules wait at most one capture (~tens of ms) and may miss one deadline. They handle that like any miss (§7.1).
- LibTorch captures use `cudaStreamCaptureModeThreadLocal` in addition.

### 6.5 Host side (GPL, in `core/` and `app/`)
- New file `core/src/gpu_runtime_loader.cpp` (+ header): finds the folder, `LoadLibraryExW(<dir>/dsptb_gpu.dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`, `GetProcAddress` for each function in §5, calls `dsptb_gpu_init`, stores the info. No CUDA headers: it includes only `gpu_runtime.h`.
- Order at startup (app and CLI): settings → **GPU runtime loader** → module registry scan. A missing runtime is **not** an error: log INFO `GPU runtime not installed`, status available to the UI.
- **Module gating** (manifest, §9.1): a package that declares `requires.gpuRuntime` is loaded only if the runtime status is `OK` and its `runtimeAbi` is high enough. Otherwise its status is `NeedsGpuRuntime` with the reason (e.g. `Needs the GPU runtime: NVIDIA driver too old …`). The Module Browser shows it greyed out with that reason.
- **Shutdown:** after all instances are destroyed and modules unloaded, call `dsptb_gpu_shutdown()`, then `FreeLibrary`.
- **UI:** a **GPU** section at the bottom of the Audio Settings window: status line, device combo (names from `dsptb_gpu_device_info`), VRAM total/free, driver/CUDA/cuDNN/ORT/LibTorch versions, sync mode combo, runtime folder with an "Open folder" button, and "Restart to apply". Test action `settings:gpu` opens it for a screenshot.
- **CLI:** `dsptb-cli gpu-info [--json]` prints the runtime info. `validate` on a package that needs the runtime, when the runtime isn't OK, prints `SKIPPED: needs the GPU runtime (<reason>)` and exits 0 if `--allow-skip` is given, 1 otherwise. The ctest `validate_<target>` entries for GPU modules pass `--allow-skip`.

## 7. Helper library for modules (`dsptb::gpu_sdk`, MIT)

Modules link ORT and LibTorch themselves (a design decision). The helper library makes the dangerous parts the same in every module. It has two layers:

| Layer | Needs CUDA? | Built in CI? | Contents |
|---|---|---|---|
| `dsptb/gpu/pipeline.hpp`, `worker.hpp` (header-only) | **No** | **Yes**, always | Inference worker thread, latency pipeline, deadline/miss handling, async loader. Works with any `Stage` (a fake one in tests). |
| `dsptb_gpu_sdk` static lib (`ort_stage.*`, `torch_stage.*`, `cuda_util.*`) | Yes | No (only with `DSPTB_GPU_RUNTIME=ON`) | ORT and LibTorch stages with CUDA graphs, pinned buffers, high-priority stream. |

### 7.1 The pipeline (backend-free)

```cpp
namespace dsptb::gpu {
struct PipelineConfig {
  double   sampleRate;        // the rate the stages work at (the module resamples before and after)
  uint32_t hopFrames;         // new input frames per inference, e.g. 20 ms
  uint32_t contextFrames;     // past frames prepended to each window (left context), e.g. 180 ms
  uint32_t lookaheadFrames;   // future frames the model needs (right context), 0 or more
  uint32_t budgetFrames;      // time allowed for compute, in frames: the declared compute latency
  uint32_t maxBlockFrames;    // host max_block_frames
};
// latencyFrames() == hopFrames + lookaheadFrames + budgetFrames   (constant after configure)

class Stage {                 // implemented by OrtStage, TorchStage, or a test fake
public:
  virtual ~Stage() = default;
  virtual void prepare() = 0;                         // worker thread; may allocate, load, warm up, capture
  virtual void run(const float* window, uint32_t windowFrames, float* out, uint32_t hopFrames) = 0; // worker thread
};

class Pipeline {
public:
  void configure(const PipelineConfig&, std::unique_ptr<Stage>);  // UI thread (from module prepare); allocates rings
  void start(); void stop();                                       // UI thread; start/stop the worker
  // AUDIO THREAD (process): no allocation, no locks, no waiting.
  void push(const float* mono, uint32_t n) noexcept;              // input samples at sampleRate
  void pull(float* mono, uint32_t n, const float* dryForMiss) noexcept; // output delayed by exactly latencyFrames
  void reset() noexcept;                                           // audio thread; flushes rings, marks a gap
  uint32_t latencyFrames() const noexcept;
  Stats stats() const noexcept;   // atomics: runs, misses, consecutive misses, p50/p99/max compute µs (rolling)
  State state() const noexcept;   // Loading, Running, Failed (with text via lastError() on the UI thread)
};
}
```

**Worker loop** (one thread per pipeline):
1. Raise priority: `AvSetMmThreadCharacteristicsW(L"Audio", &idx)` and `AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH)`. (The audio callback thread uses Pro Audio; this one is one step below it.)
2. Wait with a high-resolution waitable timer (`CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`), 1 ms period, until the input ring holds `hop + lookahead` new frames. The audio thread never signals anything: **polling keeps every Win32 call off the audio thread**.
3. Build the window: `context + hop + lookahead` frames (keep the context in a worker-owned history buffer).
4. `dsptb_gpu_lock_shared()`, `stage->run(...)`, `unlock_shared()`. Time it with QPC.
5. Write the `hop` output frames to the output ring, tagged with the input frame index they belong to.
6. Any C++ exception from `run` → state `Failed`, store the message, stop the loop. (The audio side then outputs dry; see `pull` below.)

**How much of the budget is really compute time.** The audio thread pushes and pulls whole blocks, and the worker polls every 1 ms. So the last input frame of a hop can arrive up to one block late, and its output is needed up to one block early. The time left for `stage->run` is about `budget − 2 × device block − 1 ms`. Example: budget 15 ms, 128-frame blocks at 48 kHz (2.7 ms) → about 8.7 ms of compute. Modules choose `budgetFrames ≥ compute p99 + 2 blocks + 1 ms`, and say so in their spec. **Leave headroom for the maximum, not only p99:** the probe's ManyKernels model has compute p99 5-7 ms and max 8-9 ms on the RTX 3090 (the median is 2.5 ms when the GPU idles between hops, 0.5 ms when it is kept busy), so 15 ms missed 5 to 7 hops a minute and 25 ms had no steady-state misses in 45 s runs.

**Audio side (`pull`)**: output frame `k` is the result for input frame `k − latencyFrames`. If that result is in the output ring, use it. If not (a **miss**), use `dryForMiss` for those frames and count a miss. Every switch between model output and fallback is a **5 ms equal-power crossfade**. After 10 consecutive misses, `stats()` reports `tooSlow = true`. The module shows `GPU too slow for this latency setting`.

**Async loading.** `configure` returns immediately. The worker's first task is `stage->prepare()` (load models, warm up, capture graphs; the stage takes `lock_exclusive` only around the capture, see §6.4). When it succeeds the worker starts at the first hop whose result is still due in the future, so results arrive on time. Until then, state is `Loading` and `pull` returns the fallback. **The latency is the same during loading**, so timing never jumps. The module shows `Loading model… 2.3 s`.

**Offline renders** (`prepare_info.offline == 1`): no worker thread. `push` runs the stage synchronously on the calling (offline worker) thread whenever a hop is complete, so renders never miss. The latency stays `hop + lookahead + budget`, so a live session and its export line up.

### 7.2 `OrtStage` (ONNX Runtime + CUDA EP + CUDA graph)

Use the **C++ wrapper over the ORT C API** (`onnxruntime_cxx_api.h`).
1. **One `Ort::Env` per process**, created lazily, logging level WARNING, routed into the module log when possible.
2. Session options: `SetIntraOpNumThreads(1)`, `DisableCpuMemArena()`, `SetGraphOptimizationLevel(ORT_ENABLE_ALL)`, and **no** `session.disable_cpu_ep_fallback`: real speech models keep a few shape-handling nodes on the CPU on purpose, which that setting rejects, and graph capture works with them. A model that really cannot be captured fails in its first run and `prepare()` falls back (point 7).
3. CUDA EP via `OrtCUDAProviderOptionsV2`: `device_id` = `dsptb_gpu_selected_device()`, `enable_cuda_graph` = 1 (when graphs are on), `cudnn_conv_algo_search` = `EXHAUSTIVE` by default (decided in §10; it makes the first run slow, which is fine in `prepare`), `do_copy_in_default_stream` = 1, and `user_compute_stream` = our high-priority stream (§7.4) (§10 shows it works together with `enable_cuda_graph`). Otherwise leave ORT's own stream.
4. Fixed shapes: every input and output has the shape the config implies. Allocate device buffers once (`Ort::Value::CreateTensor` on CUDA memory from `Ort::MemoryInfo("Cuda", ...)`). Bind them once with `Ort::IoBinding`. Keep **pinned** host staging buffers (`cudaHostAlloc`).
5. Warm-up: 3 `Run`s with zeros. With graphs on, ORT captures on the first run after warm-up; check that the run after that is faster (log both times).
6. `run()`: `cudaMemcpyAsync` host→device into the bound input buffers, `session.Run(Ort::RunOptions{}, binding)`, `cudaMemcpyAsync` device→host from the bound outputs, then wait (event, §7.4).
7. **If graph capture fails** (a CPU node, a dynamic shape): rebuild the session without `enable_cuda_graph`, log one WARN `ORT stage <name>: CUDA graph unavailable (<reason>), running without`, and continue. Report it in `stats().graphed = false`. One of the test networks hits this case.

### 7.3 `TorchStage` (LibTorch + `at::cuda::CUDAGraph`)

The model format is **TorchScript** (`.ts`, from `torch.jit.trace`), decided in §10.
1. Global settings, once per process: `at::set_num_threads(1)`, `at::set_num_interop_threads(1)` (wrap in try/catch: the second call can throw), `torch::jit::setGraphExecutorOptimize(false)` (stops the profiling executor from re-specialising the graph after capture), cuDNN benchmark per §10.
2. Load: `torch::jit::load(path, torch::Device(torch::kCPU))`, then `module.to(cuda)` and `module.eval()`, under `c10::InferenceMode`. **Load on the CPU first**: loading straight onto the GPU puts the traced shape constants on the GPU, and `int(tensor)` then synchronises inside graph capture (`CUDA_ERROR_STREAM_CAPTURE_UNSUPPORTED`). The exception is a model with GRU or LSTM layers: `module.to()` splits cuDNN's contiguous weight block (a warning and a copy on every call). Export such a model after `flatten_parameters()` and set `TorchOptions::loadOnDevice` (it must then contain no traced shape arithmetic). Such a model also has to be traced from a module that registers every sub-network as a submodule, or their tensors become traced constants.
3. Static tensors: allocate the input and output tensors once on the device, with the fixed shapes.
4. Warm-up: 5 forward passes on a side stream (`at::cuda::getStreamFromPool(/*isHighPriority=*/true, dev)`, `at::cuda::CUDAStreamGuard`), then `synchronize`.
5. Capture on that stream: `graph.capture_begin(at::cuda::graph_pool_handle(), cudaStreamCaptureModeThreadLocal)`, `out = module.forward({staticIn}).toTensor()`, `graph.capture_end()`. Keep `out` alive: it is the static output buffer.
6. `run()`: `staticIn.copy_(pinnedHostTensor, /*non_blocking=*/true)`, `graph.replay()`, `pinnedOut.copy_(out, true)`, wait (§7.4).
7. Every LibTorch call is inside `try { ... } catch (const c10::Error& e)`. Convert to `std::runtime_error` with `e.what_without_backtrace()`.
8. Multi-model chains (for example feature extractor → pitch estimator → synthesizer) are **several stages inside one `Stage`** subclass in the module, each with its own graph, sharing one stream. CPU glue between them (for example a pitch decode) runs on the worker thread between replays.

### 7.4 CUDA details shared by both stages (`cuda_util.*`)
- **Stream:** one stream per pipeline, created with `cudaStreamCreateWithPriority(&s, cudaStreamNonBlocking, greatestPriority)` (from `cudaDeviceGetStreamPriorityRange`). Priority helps against other CUDA work in this process. It does **not** prioritise us over a game's Direct3D work; §11 measures that.
- **Waiting:** record a `cudaEvent` (created with `cudaEventDisableTiming`, plus `cudaEventBlockingSync` when sync mode is blocking) after the last copy, and `cudaEventSynchronize` it.
- **Errors:** every CUDA call goes through `DSPTB_CUDA_CHECK(expr)`, which throws `std::runtime_error("CUDA <call> failed: <cudaGetErrorString> at <file>:<line>")`. `cudaErrorDevicesUnavailable`, `cudaErrorLaunchFailure` and similar mean the context is dead: the pipeline goes to `Failed` and only a re-prepare (after the user fixes it) recovers.
- **Pinned memory:** `cudaHostAlloc(..., cudaHostAllocDefault)` for staging buffers. Free in the stage destructor.

### 7.5 Rules for GPU modules (also go into MODULE_SDK §12)
1. `process()` calls only `Pipeline::push`, `pull` and `reset`. **Never** ORT, LibTorch or CUDA. The RT guard and code review check this.
2. `draw()` never calls them either. It reads `stats()` and `state()`.
3. `prepare()` calls `Pipeline::configure` and `start`. Heavy work happens on the worker (async loading).
4. `latency_frames()` returns `pipeline.latencyFrames()` (plus the module's own resampler delays). It's constant until the next `prepare`.
5. Changing a setting that changes shapes (hop, window, lookahead, model) is a **re-prepare**, never a live change. The SDK's existing "param read only in prepare" pattern applies (like Autotune's hidden `shifter_mode`).
6. `destroy()` calls `stop()` first, which joins the worker before any model or CUDA object is freed.
7. A module that needs the runtime declares it in `module.json` (§9.1) and links `dsptb::gpu_sdk` (which brings ONNX Runtime, LibTorch and the CUDA headers; there is no `dsptb_gpu.lib`, see §5). A stage that owns CUDA or ORT objects frees them in `Stage::teardown()`, which the pipeline calls on the worker thread that created them (LibTorch leaked about 2 MB per thread when a graphed stage died on another thread).

## 8. Build (`cmake/FetchGpuRuntime.cmake`)

### 8.1 Option and presets
- New CMake option `DSPTB_GPU_RUNTIME` (default **OFF**). With OFF, nothing in this document is downloaded or built except the backend-free pipeline headers and their unit tests (they always build and run in CI).
- New presets: `windows-release-gpu` (Release, `DSPTB_GPU_RUNTIME=ON`) and `rt-check-gpu` (Release, `DSPTB_GPU_RUNTIME=ON`, `DSPTB_RT_GUARD=ON`). Matching build and test presets.
- `DSPTB_GPU_RUNTIME=ON` with `CMAKE_BUILD_TYPE=Debug` → `message(FATAL_ERROR "The GPU runtime builds only in Release: PyTorch publishes no Windows debug LibTorch. Use windows-release-gpu or rt-check-gpu.")`.
- CI (`ci.yml`) keeps building with the option OFF. No GPU job is added: GitHub's Windows runners have no NVIDIA GPU, and the archives are several GB.

### 8.2 What the script does
Same pattern as `cmake/FetchSteamAudio.cmake` (read it first):
1. Pinned versions and SHA-256 at the top of the file: LibTorch 2.14.1+cu130 (the version the models are exported with, so exported models and LibTorch agree), ONNX Runtime 1.31.1 Windows x64 GPU (CUDA 13) zip from the GitHub release, and the NVIDIA redist zips `cuda_cudart` and `cuda_crt` (headers and `cudart.lib` only).
2. Download into `build/_downloads/` once (shared by presets), verify SHA-256, extract into `build/_deps/gpu-runtime-<ortver>-<torchver>/`. To work offline, put the archives in `build/_downloads/` by hand (same message as Steam Audio).
3. Imported targets: `GpuRuntime::onnxruntime` (headers + `onnxruntime.lib`), `GpuRuntime::cudart` (CUDA runtime headers + import lib from the NVIDIA `cuda_cudart` redist zip) and `GpuRuntime::torch`. LibTorch's own CMake config requires the CUDA toolkit (nvcc), so the Torch targets are written by hand from the extracted headers and libraries, and no toolkit install is needed.
4. Stage the runtime: copy every DLL the runtime needs (ORT, LibTorch `lib/*.dll`, CUDA, cuDNN, cuBLAS) into `<build>/bin/gpu-runtime/bin/`, `dsptb_gpu.dll` into `<build>/bin/gpu-runtime/`, licence files into `licenses/`, and write `gpu-runtime.json` with the SHA-256 of every staged file.
5. **Never** stage these files next to `DSPToolbox.exe` or into `bin/modules/`. Only `bin/gpu-runtime/`.

### 8.3 Packaging
- `Packaging.cmake`, `tools/make-release.ps1` and `release.yml` **must not** include `gpu/` outputs or `bin/gpu-runtime/`. Add a check like the ASIO/private-module ones: `make-release.ps1` fails if any file from `gpu-runtime` or any package with `requires.gpuRuntime` ends up in an app or module zip.
- `THIRD_PARTY_NOTICES.md` is unchanged for the app (the runtime isn't distributed).

### 8.4 CRT and compiler rules for `gpu/` targets
- `/MD` (release CRT), `_ITERATOR_DEBUG_LEVEL=0`, C++20, `/W4 /WX /permissive- /utf-8` for **our** code. Third-party headers (ORT, LibTorch, CUDA) are included as SYSTEM so their warnings don't fail the build.
- LibTorch headers need `/Zc:__cplusplus` and may need `NOMINMAX`; set them on targets that include LibTorch only.
- The host targets (`dsptb_core`, `DSPToolbox`, `dsptb-cli`) must not link any `GpuRuntime::*` or Torch target. Add a CMake check at the end of the top-level `CMakeLists.txt` that reads `LINK_LIBRARIES` of the three host targets and fails if one appears, and a ctest `licence_boundary` that greps `app/`, `core/`, `cli/`, `sdk/` for `#include` of `cuda`, `onnxruntime`, `torch` or `c10` and fails on any match (the only allowed GPU header in host code is `dsptb/gpu_runtime.h`).

## 9. Modules and tests

### 9.1 Manifest field
`module.json` gains an optional field (MODULE_SDK §2):
```json
"requires": { "gpuRuntime": 1 }
```
The number is the minimum `DSPTB_GPU_RUNTIME_ABI`. The host's manifest parser accepts and stores it. Unknown keys inside `requires` make the package `Incompatible` with the text `requires <key>, which this host doesn't know` (forward safety).

### 9.2 Test module `test.gpu-probe` (TEST_MODULE: never packaged)
A processor that runs a tiny network through the pipeline, so every part of §7 is exercised without a real model.
- Params: `backend` (enum `ORT, Torch`), `graphs` (bool), `hop_ms` (enum 10, 20, 40), `model` (enum `Gain, ManyKernels`), `fail_next` (int, test only: 1 = the next `run` throws, 2 = it makes a real CUDA error), `load_delay_ms` (test only: artificial load time) and `budget_ms` (compute budget).
- Models (generated by `gpu/tools/models/make_test_models.py`, committed, each a few KB):
  - `Gain`: `y = 0.5·x` as a 1×1 Conv1d over a `[1, 1, window]` tensor, returning the last `hop` samples. Exact, so outputs can be compared bit for bit with the CPU reference (allow 1e-6 for TF32: set `use_tf32=0` / `torch.backends.cuda.matmul.allow_tf32 = False` in the export and the stage).
  - `ManyKernels`: 200 tiny elementwise and 1×1 conv layers whose net effect is `y = 0.5·x` (alternating ×2, ×0.5, +c, −c). Launch-bound on purpose, to show the graph gain.
  - Each model exists as `.onnx` and as a TorchScript file, with the same weights.
- `process()`: mono mix → `pipeline.push` → `pipeline.pull` (fallback = dry) → both channels. `latency_frames` = `pipeline.latencyFrames()`. The compute budget is the `budget_ms` parameter, default 25 ms (see §7.1: 15 ms was too tight).
- `draw()`: state, backend, graphed yes/no, runs, misses, p50/p99/max compute.
- Model files are found through the package folder (`module_dir_utf8`) and copied there with `ASSETS`.

### 9.3 Benchmark tool `dsptb-gpu-bench.exe` (MIT, in `gpu/tools/bench/`)
Reproduces Python measurements in C++ with the production stage classes, so the numbers are comparable.
```
dsptb-gpu-bench --config chain.json [--iterations 300] [--warmup 30] [--graphs on|off|both] [--sync spin|blocking]
                [--paced-hop-ms 40 --seconds 25] [--gpu-load 0..100] [--json]
```
- `chain.json` lists stages in order: `{ "name": "encoder", "backend": "ort", "model": "<path>/encoder.onnx", "inputs": {"audio": [1, 3200]}, "graph": true }`, and so on. Inputs are filled with deterministic noise (`lcgNoise`, seed 1). Glue between stages is out of scope for the tool: every stage gets its own fixed input. (The real chain with glue is a module's job; this measures the runtime.)
- Output per stage and total: median, p95, p99, max ms, graphed yes/no, and process CPU % of one core (`GetProcessTimes`) over the run.
- `--paced-hop-ms`: runs one iteration per hop on a high-resolution timer for `--seconds`, like the real worker, and reports the same numbers plus misses against a budget given by `--budget-ms`.
- `--gpu-load N`: starts a second thread on a separate CUDA stream with a busy kernel tuned to keep the GPU about N % busy (measured with NVML `nvmlDeviceGetUtilizationRates` if available, otherwise by timing), to see how much other GPU work delays us. (A real game is a manual check.)
- Real model files and the configs that point to them stay on the developer's machine; they are never committed.

### 9.4 Tests (acceptance list in §11)
- `gpu/tests/test_pipeline.cpp` (doctest, always built, **runs in CI**): fake stages that sleep a given time or throw.
- `gpu/tests/test_gpu_stages.cpp` (only with `DSPTB_GPU_RUNTIME=ON`, ctest label `gpu`): real ORT and LibTorch stages on the test models. If no CUDA device is present, every case is skipped with a message (doctest `skip`), so the test passes on a machine without a GPU.
- Integration tests in `tests/integration/test_gpu_runtime.cpp` (host side): loader with a missing folder, a fake `dsptb_gpu.dll` (test fixture that returns each status), gating, `gpu-info`, `validate --allow-skip`.

## 10. Measurements that fixed the design

Three questions were answered by measurement before the production code was written. The throwaway code that did it is not kept; the answers are.

**Can ONNX Runtime and LibTorch share one CUDA 13 process?** Yes.
- Pins: ONNX Runtime 1.31.1 (CUDA 13 zip, contains `onnxruntime_providers_cuda.dll`), LibTorch 2.14.1+cu130, NVIDIA `cuda_cudart` 13.3.29 and `cuda_crt` 13.3.33 for headers and `cudart.lib`. No CUDA toolkit is needed (LibTorch's own CMake config asks for it, so `FetchGpuRuntime.cmake` defines the Torch targets by hand). The staged runtime is 40 files, 2.9 GB.
- Both share LibTorch's cuDNN 9.24. `user_compute_stream` works together with `enable_cuda_graph`.
- Preloading `onnxruntime_providers_cuda.dll` ourselves fails with Windows error 1114, so the preload stops at `onnxruntime_providers_shared` and ORT loads the CUDA provider itself.

**Which LibTorch model format?** TorchScript (`torch.jit.trace`). AOTInductor failed to export (`TritonMissing`: Triton is not available on Windows). A traced model loaded on the CPU and then moved with `module.to(cuda)` captures into a CUDA graph and matches Python exactly. Loading straight onto the GPU fails capture (§7.3).

**Spinning or blocking sync, and the cuDNN algorithm search?** Blocking sync costs nothing measurable and the worker uses about 4 % of a core instead of 35 %. `EXHAUSTIVE` algorithm search only costs time in `prepare()`. Both are the defaults.

## 11. Acceptance tests

| # | Test | How | Pass condition |
|---|---|---|---|
| G1 | Default build unchanged | `cmake --preset windows-release` + `ctest`; CI | All existing tests pass; no download of GPU archives; app zip size within 1 % of before; `test_pipeline` runs and passes |
| G2 | Licence boundary | `ctest -R licence_boundary`; CMake link check | No CUDA/ORT/LibTorch include or link in `app/ core/ cli/ sdk/` |
| G3 | Runtime fetch and staging | `cmake --preset windows-release-gpu` from a clean `build/` | Archives verified by SHA-256; `bin/gpu-runtime/` has `dsptb_gpu.dll`, `bin/`, `licenses/`, `gpu-runtime.json`; every listed hash matches the file |
| G4 | Runtime info | `dsptb-cli gpu-info --json` on the reference PC | status OK, device "NVIDIA GeForce RTX 3090", cc 8.6, CUDA 13.0, cuDNN 9.x, ORT and LibTorch versions as pinned |
| G5 | Missing / broken runtime | integration tests with: no folder, a folder without `cudnn64_9.dll`, fake `dsptb_gpu.dll` returning each status | App starts; GPU module shows `NeedsGpuRuntime` with the right reason; `validate --allow-skip` exits 0 with `SKIPPED`; other modules unaffected |
| G6 | Settings screenshot | `--screenshot` with `--test-action settings:gpu` | Look at the PNG: status line, device name, VRAM, all versions, sync mode, runtime folder |
| G7 | Correctness | `test_gpu_stages`: `Gain` and `ManyKernels`, both backends, graphs on and off | Max abs diff to the CPU reference ≤ 1e-6 |
| G8 | Graph gain | same test, `ManyKernels`, 300 runs each | Graph replay median ≤ 50 % of the non-graph median, for both backends (prints both) |
| G9 | Both runtimes in one process | `test_gpu_stages`: one ORT and one LibTorch stage running alternately on two worker threads for 60 s | No errors; results still match G7 |
| G10 | Exact latency | `render` of an impulse through `test.gpu-probe` (`Gain`), hop 10/20/40 ms, both backends, offline and paced live (NullBackend real-time) | Impulse at exactly `latency_frames` (±0) with amplitude 0.5 ± 1e-6 |
| G11 | Real-time steady state | NullBackend paced at 48 kHz / 128 frames, 10 min, `test.gpu-probe` `ManyKernels`, hop 20 ms, graphs on, each backend | 0 misses; p99 compute recorded; 0 RT guard hits (`rt-check-gpu`) |
| G12 | Capture lock | two `test.gpu-probe` instances running; re-prepare one 20 times (change `hop_ms`) | 0 CUDA errors; the running instance misses at most 1 hop per re-prepare |
| G13 | Failure handling | `fail_next` = 1 while running; also stop the GPU worker with an injected CUDA error | Output switches to dry with a crossfade within one hop; state Failed with text; no crash; other modules keep processing |
| G14 | Offline determinism | `render` of 10 s of pink noise through `test.gpu-probe` (`ManyKernels`) twice per backend | Byte-identical (`compare --tolerance 0`). If not achievable: **stop and ask the maintainer** (proposal: tolerance 1e-6 for GPU modules, decision log entry) |
| G15 | Async loading | `test.gpu-probe` with an artificial 2 s load delay (test param) | Audio keeps running (dry) during loading; latency unchanged before and after; UI shows `Loading…` |
| G16 | Reference chain parity (local models) | `dsptb-gpu-bench` with a three-stage chain (feature extractor, pitch estimator, synthesizer) at the 200 ms window (ONNX files as in the Python test; LibTorch files exported in the TorchScript form for all three networks, the pitch estimator without its CPU decode) | ORT no graphs median ≤ 19.5 ms (Python 16.9); ORT with graphs where possible ≤ 18.5 ms (Python 15.9); LibTorch with graphs ≤ 16 ms (Python 13.6). Paced hop 40 ms, 25 s: p99 recorded. Numbers recorded |
| G17 | GPU load | `dsptb-gpu-bench --paced-hop-ms 40 --gpu-load 80` | No bar. Median/p99/max with and without load recorded |
| G18 | Packaging guard | `make-release.ps1` on a `windows-release-gpu` build | Fails with a clear message if any runtime file or GPU module would be packaged; passes on `windows-release` |

**Manual checks for the maintainer:**
- [ ] Approve the pinned runtime set (versions, size, any extra install).
- [ ] Approve the `MODULE-EXCEPTION.md` wording (§12).
- [ ] Run a game while `dsptb-gpu-bench --paced-hop-ms 40 --seconds 300` runs with the chain configs: does the game's frame rate drop? Does p99 stay under budget?
- [ ] Look at the GPU settings screenshot.

## 12. Licence notes

- `gpu/` is **MIT** (`gpu/LICENSE`, copyright the project maintainer). It's the maintainer's own code; no contributor code from `modules/common` goes into it.
- `gpu/include/dsptb/gpu_runtime.h` is MIT and pure C. The GPL host may include it (MIT is GPL-compatible).
- **Proposed one-sentence addition to `MODULE-EXCEPTION.md`** (the maintainer approves the exact text before it's committed; the builder shows the diff, as with ABI changes): add to the definition of a Module, "The GPU runtime in `gpu/`, which the host reaches only through `gpu/include/dsptb/gpu_runtime.h`, is treated the same way." The maintainer decides whether this is needed at all, since nothing is distributed yet.
- No NVIDIA, ORT or LibTorch file is committed or released in M10. When public distribution is designed later, the cuDNN and CUDA licence terms for redistribution apply (notices, passing down terms); that's a separate decision.

## 13. Risks

- **ORT packaging change.** ORT is moving the CUDA runtime to a dedicated EP package. If a pinned zip no longer contains the CUDA EP, stop and ask the maintainer.
- **LibTorch format.** If neither TorchScript nor AOTInductor captures cleanly on Windows, the LibTorch path is limited to eager mode (no gain over ORT). The format test in section 10 found out before anything was built on it.
- **cuDNN mismatch.** ORT may need a newer cuDNN 9 minor than LibTorch ships. Then both use the newer NVIDIA redist DLLs (cuDNN 9 is backward compatible within the major); in the end ORT ran on LibTorch's own cuDNN, so no redist was needed (section 10).
- **Game contention.** CUDA stream priority doesn't affect Direct3D work from another process. If a game pushes p99 over budget, the remedies are a larger budget (latency) or less GPU work per hop; G17 and the manual check give the numbers.
- **Disk size.** Expect several GB in `build/_deps` and `bin/gpu-runtime` per preset. `bin/gpu-runtime` is a copy per build preset; if disk becomes a problem, use hard links (`file(CREATE_LINK ... COPY_ON_ERROR)`).

## 14. Sources
- ONNX Runtime CUDA EP (CUDA 13 default from 1.27, `enable_cuda_graph`, preload DLLs) — https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html
- ORT 1.26 release notes (CUDA 12 removed in 1.27; CUDA runtime moving to a dedicated EP) — https://newreleases.io/project/pypi/onnxruntime/release/1.26.0
- ORT I/O binding — https://www.onnxruntime.ai/docs/performance/tune-performance/iobinding.html
- Using CUDA Graphs in the PyTorch C++ API — https://docs.pytorch.org/tutorials/advanced/cpp_cuda_graphs.html
- TorchScript deprecation notice — https://docs.pytorch.org/docs/2.9/jit_unsupported.html
- AOTInductor (`aoti_compile_and_package`, `AOTIModelPackageLoader`) — https://docs.pytorch.org/docs/2.11/user_guide/torch_compiler/torch.compiler_aot_inductor.html
- AOTInductor on Windows (open question) — https://discuss.pytorch.org/t/aotinductor-on-windows/224382
- LibTorch Windows CUDA 13.0 release zips and end of Windows debug zips — https://dev-discuss.pytorch.org/t/pytorch-2-13-rc1-produced-for-pytorch-torchvision/3401 · https://dev-discuss.pytorch.org/t/important-note-stop-publishing-prebuilt-windows-libtorch-debug-binaries/3400
- cuDNN Software License Agreement — https://docs.nvidia.com/deeplearning/cudnn/sla/
- GPL-3.0 linking exception (SPDX) — https://spdx.org/licenses/GPL-3.0-linking-exception.json

## 15. What the build found (2026-11-09)

The measurements and the build changed the design in these places; the sections above are corrected accordingly.
- **Pins:** ONNX Runtime 1.31.1 (CUDA 13 zip, contains `onnxruntime_providers_cuda.dll`), LibTorch 2.14.1+cu130, NVIDIA `cuda_cudart` 13.3.29 and `cuda_crt` 13.3.33 for headers and `cudart.lib`. No CUDA toolkit is needed: LibTorch's own CMake config requires it, so `FetchGpuRuntime.cmake` defines the Torch targets by hand. The staged runtime is 40 files, 2.9 GB.
- **ORT and LibTorch coexist in one process** and share LibTorch's cuDNN 9.24; `user_compute_stream` works together with `enable_cuda_graph`.
- **Defaults (§10):** blocking sync (costs nothing measurable, the worker uses 4 % of a core instead of 35 %), cuDNN algorithm search `EXHAUSTIVE` (only costs time in `prepare`).
- **Capture lock** around the capture only (§6.4); **`Stage::teardown`** on the worker thread (§7.5); **no import library** (§5); **TorchScript** (§3, §7.3).
- **`validate` memory check:** a package with `requires.gpuRuntime` warms up 40 cycles and may grow 16 MB (not 5 cycles and 1 MB): CUDA, cuDNN and kernel modules load lazily, a one-time step of up to about 200 MB (ORT, eager mode) rather than a leak per instance.
- **Open:** LibTorch keeps about 1 MB (eager) to 2.3 MB (graphed) of host memory per model load after the model is destroyed (running a model grows nothing); G11 over 10 minutes is being re-run after a fix (a hop playing dry while a late model load finished was counted as a miss).
- **Measured (RTX 3090, 200 ms window, three stages, no glue):** ORT without graphs 16.7 ms, ORT with graphs where possible 13.7 ms, LibTorch with graphs 11.9 ms (the Python measurements were 16.9, 15.9 and 13.6). Paced at a 40 ms hop with a 25 ms budget: no hop over budget without load or with a load keeping the GPU 30 % busy; a synthetic load keeping it 80 % busy pushes p99 to 90-100 ms.
