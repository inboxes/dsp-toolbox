/* dsptb/gpu_runtime.h - DSP Toolbox GPU runtime ABI 1. C99. MIT License.
 *
 * The only GPU header that host code (app, core, cli) may include. Plain C: no CUDA, ONNX Runtime or LibTorch
 * types. The host loads dsptb_gpu.dll with LoadLibraryExW and resolves these functions with GetProcAddress;
 * it never links dsptb_gpu.lib. Modules do link it, so they share the host's instance, device and capture
 * lock.
 */
#ifndef DSPTB_GPU_RUNTIME_H
#define DSPTB_GPU_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSPTB_GPU_RUNTIME_ABI 1u

enum {
    DSPTB_GPU_OK = 0,
    DSPTB_GPU_NO_DRIVER = 1,
    DSPTB_GPU_NO_DEVICE = 2,
    DSPTB_GPU_DLL_MISSING = 3,
    DSPTB_GPU_VERSION_MISMATCH = 4,
    DSPTB_GPU_INIT_FAILED = 5,
    DSPTB_GPU_NOT_INITIALIZED = 6
};
enum { DSPTB_GPU_SYNC_SPIN = 0, DSPTB_GPU_SYNC_BLOCKING = 1 };

typedef struct dsptb_gpu_device_info {
    uint32_t struct_size;
    int32_t index;
    char name[128];                             /* UTF-8, e.g. "NVIDIA GeForce RTX 3090" */
    int32_t cc_major, cc_minor;                 /* compute capability */
    uint64_t vram_total_bytes, vram_free_bytes; /* free is 0 when unknown */
} dsptb_gpu_device_info;

typedef struct dsptb_gpu_runtime_info {
    uint32_t struct_size;
    uint32_t abi_version;         /* DSPTB_GPU_RUNTIME_ABI */
    int32_t status;               /* DSPTB_GPU_* */
    char status_text[256];        /* the reason when status != OK; the one-line GPU summary when OK */
    char runtime_dir[520];        /* UTF-8 absolute path of the gpu-runtime folder */
    int32_t driver_cuda_version;  /* cudaDriverGetVersion, e.g. 13000 */
    int32_t runtime_cuda_version; /* cudaRuntimeGetVersion */
    int32_t cudnn_version;        /* cudnnGetVersion, e.g. 92400 */
    char ort_version[32];         /* OrtGetApiBase()->GetVersionString() */
    char torch_version[32];       /* from gpu-runtime.json (LibTorch has no C version call) */
    int32_t device_count;
    int32_t selected_device; /* -1 when none */
    int32_t sync_mode;       /* DSPTB_GPU_SYNC_* */
} dsptb_gpu_runtime_info;

#ifdef __cplusplus
static_assert(sizeof(dsptb_gpu_device_info) == 160, "dsptb_gpu_device_info layout");
static_assert(sizeof(dsptb_gpu_runtime_info) == 876, "dsptb_gpu_runtime_info layout");
#else
_Static_assert(sizeof(dsptb_gpu_device_info) == 160, "dsptb_gpu_device_info layout");
_Static_assert(sizeof(dsptb_gpu_runtime_info) == 876, "dsptb_gpu_runtime_info layout");
#endif

#if defined(_WIN32) && defined(DSPTB_GPU_BUILD)
#define DSPTB_GPU_API __declspec(dllexport)
#elif defined(_WIN32)
#define DSPTB_GPU_API __declspec(dllimport)
#else
#define DSPTB_GPU_API
#endif

/* All functions: any non-audio thread. */

/* Loads the runtime from runtime_dir_utf8 and picks the device. Idempotent: a second call returns the stored
 * result; different arguments after a successful init are ignored. Returns the status. Never throws. */
DSPTB_GPU_API int dsptb_gpu_init(const char* runtime_dir_utf8, int32_t device_index, int32_t sync_mode,
                                 dsptb_gpu_runtime_info* out);
DSPTB_GPU_API void dsptb_gpu_get_info(dsptb_gpu_runtime_info* out);
DSPTB_GPU_API int dsptb_gpu_device_info(int32_t index, struct dsptb_gpu_device_info* out); /* 1 = ok */
/* Modules call this. Initialises with the folder of dsptb_gpu.dll, device 0 and spin sync if nobody did yet.
 */
DSPTB_GPU_API int32_t dsptb_gpu_selected_device(void);

/* Process-wide capture lock. Workers hold it SHARED around each inference; graph capture and model loading
 * hold it EXCLUSIVE, because some CUDA calls are illegal on every thread while one thread captures. */
DSPTB_GPU_API void dsptb_gpu_lock_shared(void);
DSPTB_GPU_API void dsptb_gpu_unlock_shared(void);
DSPTB_GPU_API void dsptb_gpu_lock_exclusive(void);
DSPTB_GPU_API void dsptb_gpu_unlock_exclusive(void);

/* The host calls this once, after every module is destroyed. */
DSPTB_GPU_API void dsptb_gpu_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
