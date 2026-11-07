# modules/common: shared DSP libraries

Two small, portable C++20 libraries with the maths the first-party modules use. They know nothing about the plugin ABI, the engine or the UI, and they build on any OS. Each public header documents its units, ranges and thread safety.

| Library | CMake target | What it holds |
|---|---|---|
| `dsp_common/` | `dsptb::dsp_common` | FFT and STFT, loudness and level metering, colormaps, frequency mapping, pitch, onset, mel and MFCC, spectral features, key detection, pitch shifting |
| `spatial_motion/` | `dsptb::spatial_motion` | Spherical coordinates, orbits and keyframe paths |

## Build and test

```sh
cmake -S modules/common -B build-common -DCMAKE_BUILD_TYPE=Release
cmake --build build-common --parallel
ctest --test-dir build-common --output-on-failure
```

Tests live in `tests/`. Per-block calls must not allocate, and the tests check this with `tests/support/alloc_counter.hpp`. The Python scripts in `tools/` generate committed golden and lookup-table files, so they aren't needed to build.
