# GPU runtime

Code that runs neural networks on an NVIDIA GPU for DSP Toolbox modules: ONNX Runtime and LibTorch on CUDA 13,
with CUDA graphs. Design: [docs/GPU_RUNTIME.md](../docs/GPU_RUNTIME.md).

**Status: development only.** Nothing here is packaged or released. The app and every release zip are built
without it, and a release script refuses to include any of it.

## Licence boundary

Everything in `gpu/` is MIT ([LICENSE](LICENSE)). It is the only part of the repository that touches CUDA, ONNX
Runtime or LibTorch. The GPL host (`app/`, `core/`, `cli/`, `sdk/`) never includes or links them. It loads
`dsptb_gpu.dll` at run time through the plain C header `include/dsptb/gpu_runtime.h`, which is the only GPU
header host code may include. Two checks enforce this: a CMake test of the host targets' link lists, and the
`licence_boundary` ctest, which scans the host sources for forbidden includes.

## Building

You need an NVIDIA GPU with a driver for CUDA 13 (R580 or newer) to run the GPU tests. You do not need the
CUDA toolkit: the build downloads pinned, checksum-verified archives (about 4 GB, once) into `build/_downloads`.

```
cmake --preset windows-release-gpu
cmake --build --preset windows-release-gpu
ctest --preset windows-release-gpu
```

The GPU runtime builds only in Release (PyTorch publishes no Windows debug LibTorch). `rt-check-gpu` is the same
with the audio-thread allocation guard. The default presets download nothing and build only the parts of `gpu/`
that need no GPU.

The staged runtime ends up in `<build>/bin/gpu-runtime/`, with a `gpu-runtime.json` that lists the SHA-256 of
every third-party file.
