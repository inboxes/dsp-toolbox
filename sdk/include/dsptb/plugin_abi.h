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

/* ---------- layout checks (x64) ----------
 * ABI v1 is frozen. These fail the build if a field is moved or resized by accident.
 * New fields may only be appended to a struct, which means updating the size here. */
#if defined(_WIN64) || defined(__x86_64__)
#  if defined(__cplusplus)
#    define DSPTB_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#  else
#    define DSPTB_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#  endif
#  define DSPTB_ABI_OFFSET(T, F, V) DSPTB_ABI_ASSERT(offsetof(T, F) == (V), #T "." #F " moved")
#  define DSPTB_ABI_SIZE(T, V) DSPTB_ABI_ASSERT(sizeof(T) == (V), #T " changed size")
DSPTB_ABI_SIZE(dsptb_kind, 4);
DSPTB_ABI_SIZE(dsptb_vec2, 8);
DSPTB_ABI_SIZE(dsptb_param_desc, 72);
DSPTB_ABI_OFFSET(dsptb_param_desc, id, 8);
DSPTB_ABI_OFFSET(dsptb_param_desc, type, 32);
DSPTB_ABI_OFFSET(dsptb_param_desc, min_value, 36);
DSPTB_ABI_OFFSET(dsptb_param_desc, flags, 52);
DSPTB_ABI_OFFSET(dsptb_param_desc, enum_labels, 56);
DSPTB_ABI_OFFSET(dsptb_param_desc, enum_count, 64);
DSPTB_ABI_SIZE(dsptb_prepare_info, 32);
DSPTB_ABI_OFFSET(dsptb_prepare_info, sample_rate, 8);
DSPTB_ABI_OFFSET(dsptb_prepare_info, offline, 24);
DSPTB_ABI_SIZE(dsptb_transport, 40);
DSPTB_ABI_OFFSET(dsptb_transport, position_seconds, 16);
DSPTB_ABI_OFFSET(dsptb_transport, sample_clock, 24);
DSPTB_ABI_OFFSET(dsptb_transport, seeked, 32);
DSPTB_ABI_SIZE(dsptb_process_ctx, 40);
DSPTB_ABI_OFFSET(dsptb_process_ctx, channels, 8);
DSPTB_ABI_OFFSET(dsptb_process_ctx, num_frames, 20);
DSPTB_ABI_OFFSET(dsptb_process_ctx, params, 24);
DSPTB_ABI_OFFSET(dsptb_process_ctx, transport, 32);
DSPTB_ABI_SIZE(dsptb_analyze_ctx, 64);
DSPTB_ABI_OFFSET(dsptb_analyze_ctx, sample_rate, 24);
DSPTB_ABI_OFFSET(dsptb_analyze_ctx, first_frame_clock, 32);
DSPTB_ABI_OFFSET(dsptb_analyze_ctx, params, 48);
DSPTB_ABI_OFFSET(dsptb_analyze_ctx, tap, 56);
DSPTB_ABI_SIZE(dsptb_offline_ctx, 72);
DSPTB_ABI_OFFSET(dsptb_offline_ctx, num_frames, 24);
DSPTB_ABI_OFFSET(dsptb_offline_ctx, sample_rate, 32);
DSPTB_ABI_OFFSET(dsptb_offline_ctx, host_token, 48);
DSPTB_ABI_OFFSET(dsptb_offline_ctx, is_cancelled, 64);
DSPTB_ABI_SIZE(dsptb_mouse, 44);
DSPTB_ABI_OFFSET(dsptb_mouse, pos, 4);
DSPTB_ABI_OFFSET(dsptb_mouse, wheel, 20);
DSPTB_ABI_OFFSET(dsptb_mouse, hovered, 24);
DSPTB_ABI_OFFSET(dsptb_mouse, down, 26);
DSPTB_ABI_OFFSET(dsptb_mouse, ctrl, 38);
DSPTB_ABI_SIZE(dsptb_canvas_api, 288);
DSPTB_ABI_OFFSET(dsptb_canvas_api, ctx, 8);
DSPTB_ABI_OFFSET(dsptb_canvas_api, ui_scale, 16);
DSPTB_ABI_OFFSET(dsptb_canvas_api, begin_region, 32);
DSPTB_ABI_OFFSET(dsptb_canvas_api, texture_destroy, 280);
DSPTB_ABI_SIZE(dsptb_host_api, 96);
DSPTB_ABI_OFFSET(dsptb_host_api, abi_version, 4);
DSPTB_ABI_OFFSET(dsptb_host_api, host, 8);
DSPTB_ABI_OFFSET(dsptb_host_api, log, 40);
DSPTB_ABI_OFFSET(dsptb_host_api, gpu_unlock, 88);
DSPTB_ABI_SIZE(dsptb_module_desc, 176);
DSPTB_ABI_OFFSET(dsptb_module_desc, abi_version, 4);
DSPTB_ABI_OFFSET(dsptb_module_desc, id, 8);
DSPTB_ABI_OFFSET(dsptb_module_desc, kind, 48);
DSPTB_ABI_OFFSET(dsptb_module_desc, caps, 52);
DSPTB_ABI_OFFSET(dsptb_module_desc, default_tap, 56);
DSPTB_ABI_OFFSET(dsptb_module_desc, params, 64);
DSPTB_ABI_OFFSET(dsptb_module_desc, num_params, 72);
DSPTB_ABI_OFFSET(dsptb_module_desc, create, 80);
DSPTB_ABI_OFFSET(dsptb_module_desc, process, 128);
DSPTB_ABI_OFFSET(dsptb_module_desc, load_state, 168);
#endif
