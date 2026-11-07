# Contributing

Thanks for helping. Bug reports, fixes and new modules are all welcome.

You often don't need to change this repo to add a feature: a module can live in its own repo and be built against the SDK. See [Writing a Module](https://github.com/inboxes/dsp-toolbox/wiki/Writing-a-Module).

## Before you start

For anything bigger than a small fix, open an issue first so we can agree on the approach. Security problems go through [SECURITY.md](SECURITY.md), not a public issue.

## Build and test

The full app needs Windows. See [Building from Source](https://github.com/inboxes/dsp-toolbox/wiki/Building-from-Source).

```powershell
. ./tools/dev-env.ps1
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The DSP libraries in `modules/common/` build on any OS with CMake 3.28 or newer and a C++20 compiler:

```sh
cmake -S modules/common -B build-common -DCMAKE_BUILD_TYPE=Release
cmake --build build-common --parallel
ctest --test-dir build-common --output-on-failure
```

## Rules

- **Real-time code:** `process()`, `reset()` and the engine callback must not allocate, free, lock, log, throw or touch files. Debug builds have a guard that fails the test if they do.
- **Plugin ABI:** `sdk/include/dsptb/plugin_abi.h` is append-only. No C++ types or exceptions cross it.
- **Layering:** `core/` must not depend on ImGui, D3D or Win32 UI. Modules depend only on `sdk/`.
- **Tests:** a behaviour change comes with a test. Test audio is generated at test time, so don't commit audio files.
- **Dependencies:** ask in an issue before adding one.

## Code style

- C++20, warning-free at `/W4 /WX` (MSVC) and `-Wall -Wextra -Wpedantic -Werror` (GCC and Clang in `modules/common/`).
- Format with the repo's `.clang-format`.
- Types are `PascalCase`, functions and variables `camelCase`, constants `kPascalCase`, files `snake_case`.
- Keep functions small, and add a comment that says *why* wherever the code isn't obvious.

## Licensing

The project is GPL-3.0-only, and by contributing you agree your contribution is licensed under it. Only add code and data under a GPLv3-compatible licence: MIT, BSD, Apache-2.0, ISC, CC0, public domain or GPLv3. Don't copy code from GPLv2-only, AGPL or proprietary sources. Writing your own implementation from papers is fine, so cite them in comments.

## Pull requests

- Branch from `main` and open one PR per change.
- Use Conventional Commits, for example `feat(core): add SPSC ring` or `fix(spectrogram): keep the axis on resize`.
- CI must be green. Keep the PR focused, with no unrelated reformatting.
- A maintainer reviews and merges.
