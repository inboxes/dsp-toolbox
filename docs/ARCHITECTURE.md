# Architecture

A short map for people reading the code. The plugin contract is in [MODULE_SDK.md](MODULE_SDK.md).

```
Source (Mic | File) ─► PRE tap ─► [P1] ─► [P2] ─► … ─► [Pn] ─► POST tap ─► Safety ─► Output device
                         │                                       │
                         └──► SPSC rings ──► Analysis thread ──► triple buffers ──► UI panels
```

| Folder | Builds | What it holds |
|---|---|---|
| `sdk/` | `dsptb_sdk` (headers) | The C plugin ABI and C++ helpers. The only thing a module includes. |
| `core/` | `dsptb_core` (static) | Engine, audio backends, sources, chain, module loader, sessions, offline renderer, recorder. No UI. |
| `app/` | `DSPToolbox.exe` | Win32, D3D11 and Dear ImGui: panels and the module canvas. |
| `cli/` | `dsptb-cli.exe` | Test signals, analysis, offline render, module validation, benchmarks. |
| `modules/` | Module DLLs | First-party modules, built the same way as third-party ones. |
| `modules/common/` | Static libraries | Shared DSP maths: FFT, loudness, pitch, key detection, pitch shifting and more. |
| `gpu/` | `dsptb_gpu.dll`, `dsptb_gpu_sdk`, `dsptb_gpu_pipeline`, `dsptb-gpu-bench.exe`, test module `test.gpu-probe` | GPU runtime for modules that run neural networks: ONNX Runtime and LibTorch on CUDA, with CUDA graphs. MIT. Built only with `DSPTB_GPU_RUNTIME=ON` (the latency pipeline and its tests always build), development only, never packaged or released. The host reaches it only through `gpu/include/dsptb/gpu_runtime.h`: `core/src/gpu_runtime_loader.*` loads it at startup before the module scan, and a CMake check and the `licence_boundary` test keep CUDA, ONNX Runtime and LibTorch out of the host. See [GPU_RUNTIME.md](GPU_RUNTIME.md). |
| `tools/`, `tests/` | | Build and release scripts; unit and integration tests. |

## Threads

- **Audio thread:** the backend callback runs the source, the processors and the safety stage. It never allocates, locks, logs or blocks.
- **UI thread:** owns the session. Each change builds a new immutable chain snapshot and publishes it with `RtSwap`; the old one is freed later, off the audio thread.
- **Analysis thread:** reads SPSC rings fed by the PRE and POST taps, so analyzers never slow down audio.
- **Worker threads:** file loading, export and offline jobs.

Threads share data only through the primitives in `sdk/include/dsptb/rt/`: SPSC rings, atomics, `RtSwap` and triple buffers.

## Modules

A module package is a folder with `module.json` and a DLL. The host scans `<exe dir>/modules`, where bundled and installed modules both live, reads the manifests, and loads a DLL the first time one of its modules is used. Calls into a module are wrapped in SEH handlers, so a crash marks the module Faulted and silences it without taking down the app.

**Bundled** modules (`DISTRIBUTION BUNDLED`) ship in the app zip and follow the app version. Everything else is **separate**: it has its own version, zip and release tag, `module/<short>/vX.Y.Z`.

## Offline rendering

Export and `dsptb-cli render` use the same chain code as the real-time engine, with a fixed block size, so a render is deterministic and matches what you hear.
