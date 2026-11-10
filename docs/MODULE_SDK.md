# Module SDK and plugin ABI (ABI version 1)

This is the contract between the host and every module, first-party or third-party. It is **normative**, and `sdk/include/dsptb/plugin_abi.h` matches §3. The ABI is frozen at v1. New fields may only be **appended** to structs, guarded by `struct_size`. Any other change means `DSPTB_ABI_VERSION = 2`.

## 1. Principles

- **C at the boundary.** Plain C structs and function pointers. No C++ types, exceptions, STL or CRT objects cross it. Modules can use any compiler that produces x64 Windows DLLs.
- **Host owns UI and memory it hands out.** Modules never link ImGui. They draw through `dsptb_canvas_api`, and the host gets parameter UI for free from the descriptors. Strings passed to the host are copied by the host before the call returns.
- **Thread contracts are explicit.** Every callback is documented with the thread it runs on (table in §4).
- **Forward compatibility.** Every struct starts with `uint32_t struct_size`. Readers ignore trailing fields they don't know, and treat missing trailing fields as zero or null.

## 2. Package manifest — `module.json`

```json
{
  "schema": 1,
  "id": "org.dsptb.spatializer",
  "name": "3D Spatializer",
  "version": "0.1.0",
  "abi": 1,
  "dll": "spatializer.dll",
  "modules": [
    {"id": "org.dsptb.spatializer", "kind": "processor"}
  ],
  "author": "DSP Toolbox",
  "license": "GPL-3.0-only",
  "description": "Place sources around the listener and render binaurally for headphones.",
  "homepage": "https://github.com/<owner>/<repo>",
  "minHostVersion": "0.1.0",
  "thirdParty": [{"name": "Steam Audio", "license": "Apache-2.0"}]
}
```
- `id`: lowercase reverse-DNS, `[a-z0-9.-]`, unique and stable forever. First-party modules use `org.dsptb.*`. Use a prefix of your own for yours. For a single-module package, the package `id` equals the module `id`.
- `version`: SemVer. `abi`: the integer ABI major version.
- The host validates the manifest against the DLL's descriptors at load.
- `requires` (optional): an object of things the module needs from the host. The only key this host knows is `gpuRuntime`, the minimum GPU runtime ABI number (`"requires": { "gpuRuntime": 1 }`, see [GPU_RUNTIME.md](GPU_RUNTIME.md)). The host loads such a module only when the GPU runtime is installed and working; otherwise its status is `Needs GPU runtime`, with the reason, and the Module Browser shows it greyed out. A key the host does not know makes the package `Incompatible` (`requires <key>, which this host doesn't know`).
- `paramHelp` (optional): an object mapping a parameter `id` to a help sentence. The host shows it as a tooltip on that parameter's control in the generic parameter panel. It is not part of the ABI.

## 3. `plugin_abi.h` (ABI v1)

```c
/* dsptb/plugin_abi.h — DSP Toolbox plugin ABI v1. C99. GPL-3.0-only. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define DSPTB_ABI_VERSION 1u
#if defined(_WIN32)
#  define DSPTB_EXPORT __declspec(dllexport)
#else
#  define DSPTB_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* ---------- enums ---------- */
typedef enum dsptb_kind { DSPTB_KIND_PROCESSOR = 1, DSPTB_KIND_ANALYZER = 2, DSPTB_KIND_OFFLINE = 3 } dsptb_kind;

enum {
  DSPTB_CAP_REALTIME   = 1u << 0, /* processor: safe on audio thread */
  DSPTB_CAP_OFFLINE    = 1u << 1, /* can run in offline render/export */
  DSPTB_CAP_GPU        = 1u << 2, /* wants the D3D11 device */
  DSPTB_CAP_OWN_THREAD = 1u << 3, /* analyzer: run on a dedicated thread */
  DSPTB_CAP_HIDE_AUTO_PARAMS = 1u << 4 /* module draws all its own controls */
};

typedef enum dsptb_tap { DSPTB_TAP_PRE = 0, DSPTB_TAP_POST = 1 } dsptb_tap;
typedef enum dsptb_param_type { DSPTB_PARAM_FLOAT = 0, DSPTB_PARAM_INT = 1, DSPTB_PARAM_BOOL = 2, DSPTB_PARAM_ENUM = 3 } dsptb_param_type;
enum { DSPTB_PARAM_LOG = 1u << 0, DSPTB_PARAM_HIDDEN = 1u << 1 /* not shown in auto UI */ };
typedef enum dsptb_source { DSPTB_SOURCE_MIC = 0, DSPTB_SOURCE_FILE = 1, DSPTB_SOURCE_OFFLINE = 2 } dsptb_source;
typedef enum dsptb_log_level { DSPTB_LOG_DEBUG = 0, DSPTB_LOG_INFO = 1, DSPTB_LOG_WARN = 2, DSPTB_LOG_ERROR = 3 } dsptb_log_level;

/* ---------- parameters ---------- */
typedef struct dsptb_param_desc {
  uint32_t struct_size;
  const char* id;          /* stable snake_case; never rename after release */
  const char* name;        /* display name */
  const char* unit;        /* "dB", "Hz", "m", "deg", "s", "" */
  dsptb_param_type type;
  float min_value, max_value, default_value;
  float smoothing_ms;      /* hint for SmoothedValue; 0 = none */
  uint32_t flags;          /* DSPTB_PARAM_* */
  const char* const* enum_labels; /* type == ENUM */
  uint32_t enum_count;
} dsptb_param_desc;

/* ---------- per-call contexts ---------- */
typedef struct dsptb_prepare_info {
  uint32_t struct_size;
  double   sample_rate;
  uint32_t max_block_frames; /* process() n <= this */
  uint32_t num_channels;     /* v1: always 2 */
  uint32_t offline;          /* 1 when prepared for offline render/export */
} dsptb_prepare_info;

typedef struct dsptb_transport {
  uint32_t struct_size;
  dsptb_source source;
  uint32_t playing;          /* file source playing, or 1 for mic/offline */
  double   position_seconds; /* file position at block start (offline: render position); mic: seconds since source start */
  uint64_t sample_clock;     /* frames since engine start; monotonic */
  uint32_t seeked;           /* 1 on the first block after a seek/loop wrap */
} dsptb_transport;

typedef struct dsptb_process_ctx {
  uint32_t struct_size;
  float* const* channels;    /* planar, in-place, num_channels pointers */
  uint32_t num_channels;
  uint32_t num_frames;
  const float* params;       /* current values, index = descriptor order */
  const dsptb_transport* transport;
} dsptb_process_ctx;

typedef struct dsptb_analyze_ctx {
  uint32_t struct_size;
  const float* const* channels; /* planar copy owned by host, valid during call */
  uint32_t num_channels;
  uint32_t num_frames;
  double   sample_rate;
  uint64_t first_frame_clock;   /* sample_clock of channels[*][0] */
  uint32_t dropped_frames;      /* frames lost since last call (ring overrun) */
  const float* params;
  dsptb_tap tap;
} dsptb_analyze_ctx;

typedef struct dsptb_offline_ctx {
  uint32_t struct_size;
  const float* const* channels; /* whole file, planar */
  uint32_t num_channels;
  uint64_t num_frames;
  double   sample_rate;
  const float* params;
  void*    host_token;
  void (*report_progress)(void* host_token, float fraction01);
  int  (*is_cancelled)(void* host_token);
} dsptb_offline_ctx;

/* ---------- canvas (UI thread only) ---------- */
typedef struct dsptb_vec2 { float x, y; } dsptb_vec2;
typedef uint64_t dsptb_texture; /* 0 = invalid */
enum { DSPTB_TEX_RGBA8 = 0 };

typedef struct dsptb_mouse {
  uint32_t struct_size;
  dsptb_vec2 pos;            /* region-local pixels */
  dsptb_vec2 drag_delta;     /* since last frame, while left button held */
  float wheel;
  uint8_t hovered, active;   /* region hovered / being dragged */
  uint8_t down[3], clicked[3], released[3], double_clicked[3]; /* L, R, M */
  uint8_t ctrl, shift, alt;
} dsptb_mouse;

typedef struct dsptb_canvas_api {
  uint32_t struct_size;
  void* ctx;
  float      (*ui_scale)(void* ctx);
  dsptb_vec2 (*available_size)(void* ctx);
  /* custom drawing region; coordinates inside are region-local, origin top-left */
  int  (*begin_region)(void* ctx, const char* id, float width, float height); /* returns 1 if visible */
  void (*end_region)(void* ctx);
  void (*mouse)(void* ctx, dsptb_mouse* out);
  void (*line)(void* ctx, dsptb_vec2 a, dsptb_vec2 b, uint32_t rgba, float thickness);
  void (*rect)(void* ctx, dsptb_vec2 min, dsptb_vec2 max, uint32_t rgba, float thickness, int filled);
  void (*circle)(void* ctx, dsptb_vec2 center, float radius, uint32_t rgba, float thickness, int filled);
  void (*polyline)(void* ctx, const dsptb_vec2* pts, uint32_t count, uint32_t rgba, float thickness, int closed);
  void (*text)(void* ctx, dsptb_vec2 pos, uint32_t rgba, const char* utf8);
  void (*image)(void* ctx, dsptb_texture tex, dsptb_vec2 min, dsptb_vec2 max, dsptb_vec2 uv0, dsptb_vec2 uv1);
  void (*push_clip)(void* ctx, dsptb_vec2 min, dsptb_vec2 max);
  void (*pop_clip)(void* ctx);
  /* immediate-mode widgets, laid out top-to-bottom; return 1 when value changed / clicked */
  void (*push_id)(void* ctx, const char* id);
  void (*pop_id)(void* ctx);
  void (*same_line)(void* ctx);
  void (*separator)(void* ctx);
  void (*label)(void* ctx, const char* utf8);
  int  (*button)(void* ctx, const char* label);
  int  (*checkbox)(void* ctx, const char* label, int* value);
  int  (*slider_float)(void* ctx, const char* label, float* value, float min, float max, const char* fmt);
  int  (*drag_float3)(void* ctx, const char* label, float value[3], float speed, float min, float max, const char* fmt);
  int  (*input_float)(void* ctx, const char* label, float* value, const char* fmt);
  int  (*combo)(void* ctx, const char* label, int* current, const char* const* items, uint32_t count);
  int  (*selectable)(void* ctx, const char* label, int selected);
  int  (*begin_child)(void* ctx, const char* id, float height); /* scrollable list area */
  void (*end_child)(void* ctx);
  int  (*param_widget)(void* ctx, uint32_t param_index);      /* host-standard control for a param */
  int  (*begin_columns)(void* ctx, const char* id, uint32_t count);
  void (*next_column)(void* ctx);
  void (*end_columns)(void* ctx);
  /* textures (host-owned, freed with instance) */
  dsptb_texture (*texture_create)(void* ctx, uint32_t width, uint32_t height, uint32_t format);
  void (*texture_update)(void* ctx, dsptb_texture tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const void* pixels, uint32_t row_pitch_bytes);
  void (*texture_destroy)(void* ctx, dsptb_texture tex);
} dsptb_canvas_api;

/* ---------- host API (given at create; valid for instance lifetime) ---------- */
typedef struct dsptb_host_api {
  uint32_t struct_size;
  uint32_t abi_version;
  void* host;            /* opaque */
  void* instance_token;  /* opaque per-instance */
  const char* host_version;     /* "0.1.0" */
  const char* module_dir_utf8;  /* package folder, for assets */
  void  (*log)(void* host, void* instance_token, dsptb_log_level level, const char* utf8); /* NOT on audio thread */
  float (*get_param)(void* host, void* instance_token, uint32_t index);           /* UI thread */
  void  (*set_param)(void* host, void* instance_token, uint32_t index, float v);  /* UI thread; creates undo step */
  void  (*mark_state_dirty)(void* host, void* instance_token, const char* undo_label); /* UI thread */
  void* (*gpu_d3d11_device)(void* host); /* ID3D11Device*, NULL if no CAP_GPU or unavailable */
  void  (*gpu_lock)(void* host);
  void  (*gpu_unlock)(void* host);
} dsptb_host_api;

/* ---------- module descriptor ---------- */
typedef struct dsptb_module_desc {
  uint32_t struct_size;
  uint32_t abi_version;          /* = DSPTB_ABI_VERSION */
  const char* id;                /* matches module.json */
  const char* name;
  const char* version;
  const char* vendor;
  const char* description;
  dsptb_kind kind;
  uint32_t caps;                 /* DSPTB_CAP_* */
  dsptb_tap default_tap;         /* analyzers */
  const dsptb_param_desc* params;
  uint32_t num_params;

  void* (*create)(const dsptb_host_api* host);                       /* UI thread */
  void  (*destroy)(void* self);                                      /* UI thread */
  int   (*prepare)(void* self, const dsptb_prepare_info* info);      /* UI/worker thread, audio stopped for this instance; may allocate; 0 = ok */
  void  (*reset)(void* self);                                        /* AUDIO thread (on seek) — must be RT-safe */
  uint32_t (*latency_frames)(void* self);                            /* any thread; constant after prepare */
  uint32_t (*tail_frames)(void* self);                               /* any thread; for export */
  void  (*process)(void* self, const dsptb_process_ctx* ctx);        /* PROCESSOR: audio thread or offline worker */
  void  (*analyze)(void* self, const dsptb_analyze_ctx* ctx);        /* ANALYZER: analysis thread */
  int   (*run_offline)(void* self, const dsptb_offline_ctx* ctx);    /* OFFLINE: worker thread; 0 = ok */
  void  (*draw)(void* self, const dsptb_canvas_api* canvas);         /* UI thread; may be NULL */
  int   (*save_state)(void* self, void (*write)(void* tok, const char* utf8, size_t len), void* tok); /* UI thread; JSON object text */
  int   (*load_state)(void* self, const char* utf8, size_t len);     /* UI thread; before/after prepare */
} dsptb_module_desc;

/* ---------- DLL exports ---------- */
DSPTB_EXPORT uint32_t dsptb_module_count(void);
DSPTB_EXPORT const dsptb_module_desc* dsptb_module_get(uint32_t index, uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif
```

Required callbacks by kind. **Processor**: create, destroy, prepare, reset, latency_frames, process. **Analyzer**: create, destroy, prepare, analyze. **Offline**: create, destroy, prepare, run_offline. The rest are optional (NULL).

## 4. Thread and lifecycle contract

| Callback | Thread | RT-safe required | Notes |
|---|---|---|---|
| create / destroy | UI | no | `destroy` never runs while the instance is in a live snapshot |
| prepare | UI or worker | no | Called again on device or block-size change. Not concurrent with process/analyze for this instance. |
| load_state / save_state | UI | no | `load_state` may be called before or after prepare. The module must hand state to the audio side via `RtSwap`. |
| process | audio, or offline worker | **yes** | In-place. `n` ≤ `max_block_frames`, and `n` can vary per call, including 0. |
| reset | audio | **yes** | Clear delay lines and filters; keep params and state |
| analyze | analysis thread (or own) | should avoid alloc | Publish results with `TripleBuffer` for `draw` |
| run_offline | worker | no | Poll `is_cancelled` at least every 50 ms. In the app, `channels` holds the file at its own sample rate, which can differ from the session's: use `sample_rate`. |
| draw | UI | no | Must not block; read the latest results only |
| latency_frames / tail_frames | any | yes | Constant between prepares |

Lifecycle: `create → load_state? → prepare → (process|analyze)* → [prepare again on config change] → destroy`.

## 5. C++ SDK (`sdk.hpp`)

Header-only. Authors subclass and register:

```cpp
#include <dsptb/sdk.hpp>

class Gain final : public dsptb::Processor<Gain> {
public:
  static constexpr dsptb::ModuleInfo info{
      .id = "org.dsptb.gain", .name = "Gain", .version = "0.1.0", .vendor = "DSP Toolbox",
      .description = "Simple gain with smoothing.", .caps = DSPTB_CAP_REALTIME | DSPTB_CAP_OFFLINE};
  static constexpr auto params = dsptb::params(
      dsptb::Float("gain_db", "Gain", "dB", -60.f, 24.f, 0.f).smoothing(20.f));

  bool prepare(const dsptb::PrepareInfo& p) override { gain_.prepare(p.sampleRate, 20.f); return true; }
  void process(dsptb::ProcessBlock& b) override {
    gain_.setTarget(dsptb::dbToGain(b.param(0)));
    for (uint32_t i = 0; i < b.frames(); ++i) { const float g = gain_.next();
      for (auto ch : b.channels()) ch[i] *= g; }
  }
private:
  dsptb::SmoothedValue gain_;
};
DSPTB_REGISTER_MODULES(Gain)
```

The SDK provides:
- `Processor<T>`, `Analyzer<T>` and `OfflineJob<T>` bases. Their trampolines catch C++ exceptions at the boundary: they log, then return an error or output silence.
- Param builders (`Float`, `Int`, `Bool`, `Enum`), `ProcessBlock`, `AnalyzeBlock` and `Canvas`, which is a thin C++ wrapper over `dsptb_canvas_api` with RAII region and id scopes.
- `rt/` primitives and `dsp/` helpers: `FifoAdapter`, windows (Hann, Blackman-Harris) and dB helpers. Colormaps, FFT, STFT and the other analysis maths live in the `dsptb::dsp_common` static library (`modules/common`). First-party modules link it. It is not part of the ABI.
- `DSPTB_REGISTER_MODULES(A, B, …)`, which generates the two exports. `DSPTB_DEFINE_RT_GUARD()` adds the debug allocation guard.
- `state_json.hpp`: helpers to read and write state using nlohmann/json, which is header-only and bundled with the SDK package.

## 6. Authoring workflow

1. `tools/new-module.ps1 -Name "My Filter" -Id me.myfilter -Kind processor [-OutDir <folder>] [-License <SPDX id>]` copies the module template, names it and fills in `module.json`. `-Kind` is `processor` or `analyzer`.
2. Out of tree, with the SDK zip: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<sdk>`, from a Visual Studio x64 developer prompt. No vcpkg is needed. In the source tree: put it under `modules/`, or in a folder listed in `DSPTB_EXTRA_MODULE_DIRS`.
3. Build. `dsptb_add_module()` places the DLL and `module.json` in `<build>/bin/modules/<id>/`.
4. Run `ctest`, or `dsptb-cli validate <package folder>`. It loads the module, checks descriptors and runs the conformance suite.
5. Copy the package folder to the `modules` folder next to the app and press **Rescan** in the app. The module appears in the Module Browser.
6. To distribute: zip the package folder. Users install it with **Install from .zip...**. In the source tree, `tools/make-release.ps1 -Version <version> -Module <id> -SkipBuild` does this for first-party modules.

### Licensing

We encourage open-source modules. `new-module.ps1` sets `GPL-3.0-only` unless you pass `-License`. The SDK is GPL-3.0-only with an additional permission for modules that use only the public ABI and SDK: read `MODULE-EXCEPTION.md` before you release under another licence. Commercial use of a closed-source module needs written permission.

The SDK package has what a module needs. The `dsptb::dsp_common` library used by the first-party modules isn't part of it, so bring your own FFT and analysis code, or build in the source tree.

## 7. CMake function

```cmake
dsptb_add_module(<target>
  ID org.dsptb.gain
  SOURCES src/gain.cpp
  [MANIFEST module.json]          # default: ./module.json
  [DEPENDENCY_DLLS <paths...>]    # copied next to the DLL (e.g. phonon.dll)
  [ASSETS <dir>]
  [PRIVATE_MODULE])               # never packaged into public releases
```
It sets `/W4 /WX`, links `dsptb::sdk`, copies the manifest (setting `version` from the target's version), and adds a `validate_<target>` test that runs `dsptb-cli validate`.

## 8. Notes for module authors: the host and the chain
- **Layout checks.** The shipped `plugin_abi.h` ends with `static_assert`s (C11 `_Static_assert` in C) for the x64 size of every struct and the offsets of the key fields. Appending a field means updating the size there.
- **Optional debug exports.** `DSPTB_REGISTER_MODULES` also exports `dsptb_rt_guard_violations()` and `dsptb_rt_guard_reset()`. They are not required by the ABI; the host and `dsptb-cli validate` use them to read the module's own allocation-guard counter (each DLL has its own `operator new`). A hand-written module may omit them.
- **Exceptions.** The SDK's `process()` trampoline turns an escaping C++ exception into a silenced block plus a structured exception, which the host treats as a fault (module Faulted, bypassed, banner).
- **Chain behaviour a module can rely on.** `process()` is called with `n <= max_block_frames` (including 0) in place on planar stereo; `reset()` is called after a seek and when the slot leaves bypass; a bypassed module is not called but its dry signal is delayed by `latency_frames()`, so latency is constant; `prepare()` is called again when the device sample rate or block size changes, never while the module is processing.
- **CMake.** `dsptb_add_module` also accepts `VERSION <semver>` and `TEST_MODULE` (test fixtures: output `bin/test-modules/<id>`, no `validate_` test).

## 9. Notes for module authors: analyzers
- **What the host gives `analyze()`.** Chunks of 1 to 4096 frames of planar stereo (never the engine's callback block), on the shared analysis thread (about 200 Hz) or, with `DSPTB_CAP_OWN_THREAD`, on a thread of the module's own. `first_frame_clock` continues where the previous chunk ended, plus the frames reported in `dropped_frames`. `dropped_frames` is also 1 after the analyzer was disabled for a while or its tap changed, so draw a gap. The module is prepared with `max_block_frames = 4096`.
- **Samples can be NaN or Inf** (a PRE tap carries the source as it arrives). Scrub them before they reach a filter or a running sum.
- **Threads.** `analyze()` and `draw()` run on different threads: share data through `rt::SpscRing`, `rt::TripleBuffer` or atomics (the Spectrogram and Level Meter modules show both). `analyze()` may allocate (rebuilding an FFT when a parameter changes is fine) but should not do it per call.
- **Offline dump.** Override `ModuleBase::dumpFrames()` to return a JSON document of what the analyzer has produced; `DSPTB_REGISTER_MODULES` exports it as the optional symbol `dsptb_dump_frames`, and `dsptb-cli analyze --module <id> --in file.wav --dump-frames out.json` writes it. It is called on the thread that fed `analyze()`, after the last chunk. `prepare()` receives `offline = true` in that case, so a module can keep per-frame records only then.
- **CMake.** `dsptb_add_module` also accepts `LINK_LIBRARIES <targets>` (for example `dsptb::dsp_common`).

## 10. Planned appends

These aren't in the current header. They would be **appends** to the end of existing structs, so `DSPTB_ABI_VERSION` stays 1, and old modules and old hosts keep working.

**Rule for modules:** before calling a new function or reading a new field, check `struct_size` (for example `host->struct_size >= offsetof(dsptb_host_api, release_working_file) + sizeof(void*)`). If it's too small, behave as if the feature isn't there. The SDK wrappers do this for you.

### 10.1 Working-file access

```c
/* new struct */
typedef struct dsptb_file_view {
  uint32_t struct_size;
  const float* const* channels; /* planar, read-only; valid until release_working_file */
  uint32_t num_channels;        /* 2 */
  uint64_t num_frames;
  double   sample_rate;         /* the engine rate (the file is already resampled) */
  uint64_t generation;          /* matches working_file_generation() at acquire time */
  void*    handle;              /* host-owned; pass back unchanged */
} dsptb_file_view;

/* appended to the END of dsptb_host_api */
  uint64_t (*working_file_generation)(void* host);  /* ANY thread, incl. audio; 0 = no file; changes on every load/unload */
  int  (*acquire_working_file)(void* host, void* instance_token, dsptb_file_view* out);   /* any NON-audio thread; 1 = ok, 0 = no file */
  void (*release_working_file)(void* host, void* instance_token, dsptb_file_view* view); /* any NON-audio thread */
```
- The host keeps a reference (`shared_ptr<const LoadedFile>`) alive until release, even if a new file is loaded meanwhile.
- In offline renders (export, `dsptb-cli render`) the working file is the render input, with generation 1.
- A module must release every view before `destroy` returns. The host frees any leftovers at destroy and logs a warning.
- `validate`'s fake host implements all three, and runs the lifecycle suite once with a file and once without.
- SDK: `Host::workingFileGeneration()`, `Host::acquireWorkingFile(dsptb_file_view&)` (returns `false` when unsupported) and an RAII `dsptb::WorkingFile` that releases in its destructor.

### 10.2 Backing samples per block

```c
/* appended to the END of dsptb_process_ctx */
  const float* const* backing;   /* Mic + track mode while the track plays: this block's backing samples
                                    (planar, num_frames long, before Backing volume). NULL otherwise. */
  uint32_t backing_num_channels; /* 2, or 0 when backing is NULL */
```
- Read-only, valid only during the `process()` call. Offline renders always pass NULL.
- SDK: `ProcessBlock::backing()` returns an empty range when not available.

### 10.3 Module data folder and param gestures

Not approved yet. Like §10.1 and §10.2 it would be an **append** to the end of `dsptb_host_api` (after the §10.1 fields), so `DSPTB_ABI_VERSION` stays 1.

```c
/* appended to the END of dsptb_host_api, after release_working_file */
  const char* module_data_dir_utf8;  /* per-user folder for this module id, created by the host before create():
                                        %LOCALAPPDATA%/<DataDir>/module-data/<module id>/   NULL = not supported */
  void (*begin_param_gesture)(void* host, void* instance_token, const char* undo_label); /* UI thread */
  void (*end_param_gesture)(void* host, void* instance_token);                           /* UI thread */
```
- **Gesture:** every `set_param` between begin and end becomes **one undo step** with that label. Values still reach the audio thread at once. No nesting: a second begin ends the first. The host ends an open gesture when the panel closes or the instance is removed.
- **Data folder:** the module may create sub-folders and files in it on the UI thread or its own worker threads (never the audio thread).
- SDK: `Host::moduleDataDir()` (empty path when unsupported), `Host::beginGesture(label)` / `endGesture()` (no-ops when unsupported) and an RAII `dsptb::ParamGesture`.
- `validate`'s fake host implements all three (the data folder is a temp dir) and runs the lifecycle suite once with and once without them.

## 11. Notes for module authors: re-blocking, time and DLLs
- **FifoAdapter** (`dsptb/dsp/fifo_adapter.hpp`): re-blocks planar stereo to a fixed frame size with exactly that much latency (report it from `latency_frames`). The block callback gets `(in, out, firstInputOffset)`, the offset of its first frame relative to the first frame of the current `process()` call (negative when it began in an earlier call), so a module can place a block on the transport timeline: `position_seconds + offset / sampleRate`.
- **Transport time and seeks.** `transport.position_seconds` is the position of the first frame of the block (offline: the render position). `reset()` is called after a seek; `seeked` is 1 on that block.
- **Libraries that ship a DLL.** `dsptb_add_module(... DEPENDENCY_DLLS <dll>)` copies it into the package folder, which is on the DLL search path of the module (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`), and `EXTRA_FILES <paths>` copies licenses and notices next to it.
- **Undo for custom UIs.** `mark_state_dirty` records one undo step per call, so a module that edits state while a slider or a dot is dragged should call it once when the edit has settled (the Spatializer waits 8 frames without edits).
- **Hosts at higher sample rates.** A module that depends on a library with a fixed set of rates can run at a divisor of the host rate and convert (see `modules/spatializer/src/halfband.hpp`); the converters' delay belongs in `latency_frames`.

## 12. GPU modules (development only)

A module can run a neural network on an NVIDIA GPU through the GPU runtime in `gpu/` (design and numbers: [GPU_RUNTIME.md](GPU_RUNTIME.md)). The runtime is **dev-only for now**: it is built from source with `-DDSPTB_GPU_RUNTIME=ON` (preset `windows-release-gpu`), never packaged or released, and a release script refuses a GPU module. Without the runtime installed, a GPU module simply shows as unavailable.

**Manifest.** Declare the need in `module.json`:
```json
"requires": { "gpuRuntime": 1 }
```
The number is the minimum GPU runtime ABI. The host loads the module only when the runtime is installed and working; otherwise the package status is `Needs GPU runtime` with the reason (no driver, driver too old for CUDA 13, a DLL missing, ...) and the Module Browser shows it greyed out. A `requires` key the host does not know makes the package `Incompatible`.

**Build.** Link `dsptb::gpu_sdk` (it brings ONNX Runtime, LibTorch and the CUDA headers; the module never imports `dsptb_gpu.dll`, the SDK finds the instance the host loaded). Build in Release (`/MD`): there is no Windows debug LibTorch. `gpu/test-module/` (`test.gpu-probe`) is a complete example: `dsptb_add_module(... LINK_LIBRARIES dsptb::gpu_sdk)` and the models as `ASSETS`.

**Use.** Do not call ONNX Runtime, LibTorch or CUDA from your own threads. Give a `dsptb::gpu::Pipeline` a `Stage` and let it run the stage on its worker thread:
```cpp
dsptb::gpu::PipelineConfig cfg;           // rate, hop, context, lookahead, budget, max block
pipe.setCaptureLock(dsptb::gpu::runtimeCaptureLock());
pipe.configure(cfg, std::make_unique<dsptb::gpu::OrtStage>(model, window, hop), p.offline);   // prepare()
pipe.start();                             // returns at once; the model loads on the worker
// process():  pipe.push(mono, n);  pipe.pull(out, n, dryForMiss);
// reset():    pipe.reset();   latency_frames(): pipe.latencyFrames();   destroy: pipe.stop() first
```
`OrtStage` and `TorchStage` run one model with an audio input `[1, 1, window]` and output `[1, 1, hop]`; for a chain of several networks, write a `Stage` that owns `OrtModel` and `TorchModel` objects (fixed shapes, CUDA graphs, pinned host buffers) and runs them in order on one `CudaStream`, with any CPU glue between them. Models for LibTorch are TorchScript files (`torch.jit.trace`).

**Rules** (the RT guard and code review check the first two):
1. `process()` calls only `Pipeline::push`, `pull` and `reset`. Never ONNX Runtime, LibTorch or CUDA.
2. `draw()` never calls them either. It reads `stats()` and `state()`.
3. `prepare()` calls `Pipeline::configure` and `start`. The heavy work (loading, warm-up, graph capture) happens on the worker: `state()` is `Loading` until it is done, the audio runs on your dry signal meanwhile, and `latency_frames()` is the same before and after.
4. `latency_frames()` returns `pipeline.latencyFrames()` (plus any resampler delay of your own). It is constant until the next `prepare`.
5. A setting that changes shapes (hop, window, lookahead, model) is read in `prepare()` only, never changed live.
6. `destroy()` calls `stop()` first, which joins the worker before any model or CUDA object is freed. A stage frees its own CUDA and model objects in `Stage::teardown()`, which the pipeline calls on the worker thread that created them.
7. Choose the compute budget with headroom: `budgetFrames >= compute p99 + 2 audio blocks + 1 ms`, and look at the maximum too (a graphed model with a 5 ms p99 had an 8-9 ms maximum, and 15 ms was too tight).
8. A miss (a result that is not ready when due) plays your dry signal through a 5 ms equal-power crossfade and is counted in `stats().misses`; ten in a row set `stats().tooSlow`, which you should show to the user.

**Validation.** `dsptb-cli validate <package> --allow-skip` prints `SKIPPED: needs the GPU runtime (<reason>)` and exits 0 when the runtime is missing, so a build without a GPU still passes. With the runtime it runs the normal suite; the memory check for a GPU module warms up longer and allows 16 MB, because CUDA, cuDNN and kernels load lazily.

**Other tips.** Hold the capture lock (`ScopedCapture`) only around a CUDA graph capture, never around a model load. Run inference on the worker, not on the UI thread. Do not copy a model into the repository; keep it next to your module.
