# GPU runtime dependencies: ONNX Runtime (CUDA EP) and LibTorch (CUDA), both for CUDA 13.
# Used only with -DDSPTB_GPU_RUNTIME=ON (presets windows-release-gpu and rt-check-gpu). Dev-only: nothing from
# here is packaged or released (see tools/make-release.ps1 and gpu/README.md).
#
# Like FetchSteamAudio.cmake it downloads official archives, verifies each SHA-256, and extracts them under
# build/ (gitignored). The archives live in build/_downloads and the extraction in build/_deps is shared by
# every preset. To work offline, put the four archives in build/_downloads by hand.
#
# What it defines:
#   GpuRuntime::onnxruntime  ORT headers and import library (C and C++ API)
#   GpuRuntime::cudart       CUDA runtime headers and import library
#   GpuRuntime::torch        LibTorch headers and import libraries. LibTorch's own CMake config needs the CUDA
#                            toolkit installed, so the targets are written by hand, with the /INCLUDE that keeps
#                            torch_cuda.dll from being dropped by the linker (the same one TorchConfig.cmake uses).
#   DSPTB_GPU_RUNTIME_STAGE_DIR   <build>/bin/gpu-runtime, laid out as docs/GPU_RUNTIME.md section 6.1
#
# To update a pin, change the version and the hash together.

include_guard(GLOBAL)

set(DSPTB_ORT_VERSION "1.31.1")
set(DSPTB_ORT_SHA256 "6c64508de9c04f9d938297287e9d81fa47a2fbbb30cc04e6fac0db88f6cb68c1")
set(DSPTB_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${DSPTB_ORT_VERSION}/onnxruntime-win-x64-gpu_cuda13-${DSPTB_ORT_VERSION}.zip")

set(DSPTB_LIBTORCH_VERSION "2.14.1")
set(DSPTB_LIBTORCH_SHA256 "b86d779a49928a30d24bd13b386b2843ed2fda8e93ea9abb625b2bba1928fd2a")
set(DSPTB_LIBTORCH_URL "https://download.pytorch.org/libtorch/cu130/libtorch-win-shared-with-deps-${DSPTB_LIBTORCH_VERSION}%2Bcu130.zip")

# NVIDIA redistributables: headers and cudart.lib only (the DLL comes with LibTorch). cuda_crt holds
# crt/host_config.h, which cuda_runtime.h includes.
set(DSPTB_CUDART_VERSION "13.3.29")
set(DSPTB_CUDART_SHA256 "1feb7dd266813ffe8dbc24e115183a5ac35a4795c8d34aca0df85ab616b64d9c")
set(DSPTB_CUDART_URL "https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-${DSPTB_CUDART_VERSION}-archive.zip")
set(DSPTB_CUDACRT_VERSION "13.3.33")
set(DSPTB_CUDACRT_SHA256 "752c528281a06a0ddf89237d760ffd6acde1b9cd59efc35803c2591127ef55f0")
set(DSPTB_CUDACRT_URL "https://developer.download.nvidia.com/compute/cuda/redist/cuda_crt/windows-x86_64/cuda_crt-windows-x86_64-${DSPTB_CUDACRT_VERSION}-archive.zip")

# The cuDNN that LibTorch ships (ORT's zip has none and uses this one). Informational: written to the manifest.
set(DSPTB_CUDNN_VERSION "9.24.0.43")

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "The GPU runtime builds only in Release: PyTorch publishes no Windows debug LibTorch. "
    "Use the windows-release-gpu or rt-check-gpu preset.")
endif()

set(_gr_downloads "${CMAKE_SOURCE_DIR}/build/_downloads")
set(_gr_deps "${CMAKE_SOURCE_DIR}/build/_deps/gpu-runtime-ort${DSPTB_ORT_VERSION}-torch${DSPTB_LIBTORCH_VERSION}-cuda13")

# Downloads <url> to build/_downloads/<file> unless it is there already, and checks the hash either way.
function(dsptb_gpu_fetch file url sha256 size_text)
  set(_zip "${_gr_downloads}/${file}")
  if(NOT EXISTS "${_zip}")
    message(STATUS "Downloading ${file} (${size_text}, once)")
    file(MAKE_DIRECTORY "${_gr_downloads}")
    file(DOWNLOAD "${url}" "${_zip}" EXPECTED_HASH SHA256=${sha256} SHOW_PROGRESS STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_zip}")
      message(FATAL_ERROR "Could not download ${url}: ${_status}\nDownload ${file} by hand to ${_zip} and configure again.")
    endif()
  else()
    file(SHA256 "${_zip}" _actual)
    if(NOT _actual STREQUAL sha256)
      message(FATAL_ERROR "${_zip} has SHA-256 ${_actual}, expected ${sha256}. Delete it.")
    endif()
  endif()
endfunction()

set(_ort_zip_name "onnxruntime-win-x64-gpu_cuda13-${DSPTB_ORT_VERSION}.zip")
set(_lt_zip_name "libtorch-win-shared-with-deps-${DSPTB_LIBTORCH_VERSION}+cu130.zip")
set(_cudart_zip_name "cuda_cudart-windows-x86_64-${DSPTB_CUDART_VERSION}-archive.zip")
set(_cudacrt_zip_name "cuda_crt-windows-x86_64-${DSPTB_CUDACRT_VERSION}-archive.zip")

set(_ort_root "${_gr_deps}/onnxruntime-win-x64-gpu_cuda13-${DSPTB_ORT_VERSION}")
set(_lt_root "${_gr_deps}/libtorch")
set(_cudart_root "${_gr_deps}/cuda_cudart-windows-x86_64-${DSPTB_CUDART_VERSION}-archive")
set(_cudacrt_root "${_gr_deps}/cuda_crt-windows-x86_64-${DSPTB_CUDACRT_VERSION}-archive")

if(NOT EXISTS "${_gr_deps}/.extracted-ok")
  dsptb_gpu_fetch("${_ort_zip_name}" "${DSPTB_ORT_URL}" "${DSPTB_ORT_SHA256}" "309 MB")
  dsptb_gpu_fetch("${_lt_zip_name}" "${DSPTB_LIBTORCH_URL}" "${DSPTB_LIBTORCH_SHA256}" "3.9 GB")
  dsptb_gpu_fetch("${_cudart_zip_name}" "${DSPTB_CUDART_URL}" "${DSPTB_CUDART_SHA256}" "2.6 MB")
  dsptb_gpu_fetch("${_cudacrt_zip_name}" "${DSPTB_CUDACRT_URL}" "${DSPTB_CUDACRT_SHA256}" "0.2 MB")
  message(STATUS "Extracting the GPU runtime archives (about 3.2 GB, once)")
  file(REMOVE_RECURSE "${_gr_deps}")
  file(MAKE_DIRECTORY "${_gr_deps}")
  file(ARCHIVE_EXTRACT INPUT "${_gr_downloads}/${_ort_zip_name}" DESTINATION "${_gr_deps}")
  # LibTorch's bin/ repeats lib/ byte for byte (about 3 GB), so leave it out.
  file(ARCHIVE_EXTRACT INPUT "${_gr_downloads}/${_lt_zip_name}" DESTINATION "${_gr_deps}"
    PATTERNS "libtorch/include/*" "libtorch/lib/*" "libtorch/build-version")
  file(ARCHIVE_EXTRACT INPUT "${_gr_downloads}/${_cudart_zip_name}" DESTINATION "${_gr_deps}")
  file(ARCHIVE_EXTRACT INPUT "${_gr_downloads}/${_cudacrt_zip_name}" DESTINATION "${_gr_deps}")
  foreach(_need "${_ort_root}/lib/onnxruntime_providers_cuda.dll" "${_lt_root}/lib/torch_cuda.dll"
      "${_cudart_root}/lib/x64/cudart.lib" "${_cudacrt_root}/include/crt/host_config.h")
    if(NOT EXISTS "${_need}")
      message(FATAL_ERROR "GPU runtime extraction did not produce ${_need}")
    endif()
  endforeach()
  file(WRITE "${_gr_deps}/.extracted-ok" "ok\n")
endif()

# The CUDA headers from the redistributables: cudart plus the crt folder.
set(_cuda_include "${_gr_deps}/cuda-include")
if(NOT EXISTS "${_cuda_include}/cuda_runtime.h")
  file(MAKE_DIRECTORY "${_cuda_include}")
  file(COPY "${_cudart_root}/include/" DESTINATION "${_cuda_include}")
  file(COPY "${_cudacrt_root}/include/" DESTINATION "${_cuda_include}")
endif()

set(DSPTB_GPU_CUDA_INCLUDE "${_cuda_include}" CACHE INTERNAL "CUDA runtime headers from the redistributables")

# --- Imported targets ---
add_library(GpuRuntime::cudart SHARED IMPORTED GLOBAL)
set_target_properties(GpuRuntime::cudart PROPERTIES
  IMPORTED_LOCATION "${_lt_root}/lib/cudart64_13.dll"
  IMPORTED_IMPLIB "${_cudart_root}/lib/x64/cudart.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${_cuda_include}"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cuda_include}")

add_library(GpuRuntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(GpuRuntime::onnxruntime PROPERTIES
  IMPORTED_LOCATION "${_ort_root}/lib/onnxruntime.dll"
  IMPORTED_IMPLIB "${_ort_root}/lib/onnxruntime.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${_ort_root}/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ort_root}/include")

add_library(GpuRuntime::torch INTERFACE IMPORTED GLOBAL)
set_target_properties(GpuRuntime::torch PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_lt_root}/include;${_lt_root}/include/torch/csrc/api/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_lt_root}/include;${_lt_root}/include/torch/csrc/api/include"
  INTERFACE_COMPILE_OPTIONS "/Zc:__cplusplus"
  INTERFACE_COMPILE_DEFINITIONS "NOMINMAX"
  INTERFACE_LINK_DIRECTORIES "${_lt_root}/lib"
  INTERFACE_LINK_LIBRARIES "torch;torch_cpu;c10;c10_cuda;torch_cuda;GpuRuntime::cudart"
  INTERFACE_LINK_OPTIONS "/INCLUDE:?warp_size@cuda@at@@YAHXZ")

# --- Stage <build>/bin/gpu-runtime (spec section 6.1) ---
set(DSPTB_GPU_RUNTIME_STAGE_DIR "${CMAKE_BINARY_DIR}/bin/gpu-runtime" CACHE INTERNAL "Staged GPU runtime folder")
set(_stage_bin "${DSPTB_GPU_RUNTIME_STAGE_DIR}/bin")
file(MAKE_DIRECTORY "${_stage_bin}" "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses")

file(GLOB _lt_dlls "${_lt_root}/lib/*.dll")
list(FILTER _lt_dlls EXCLUDE REGEX "/(torch_python|shm)\\.dll$")   # Python bindings only
set(_ort_dlls "${_ort_root}/lib/onnxruntime.dll" "${_ort_root}/lib/onnxruntime_providers_shared.dll"
  "${_ort_root}/lib/onnxruntime_providers_cuda.dll")
set(_stage_files ${_ort_dlls} ${_lt_dlls})
set(_manifest "${DSPTB_GPU_RUNTIME_STAGE_DIR}/gpu-runtime.json")
set(_pin_stamp "ort=${DSPTB_ORT_VERSION} torch=${DSPTB_LIBTORCH_VERSION} cudart=${DSPTB_CUDART_VERSION}")

# Hard links when the volume allows (each preset would otherwise hold another 2.9 GB copy), else a copy.
set(_need_manifest FALSE)
foreach(_f IN LISTS _stage_files)
  get_filename_component(_n "${_f}" NAME)
  if(NOT EXISTS "${_stage_bin}/${_n}")
    file(CREATE_LINK "${_f}" "${_stage_bin}/${_n}" COPY_ON_ERROR)
    set(_need_manifest TRUE)
  endif()
endforeach()
file(COPY "${_ort_root}/LICENSE" DESTINATION "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses")
file(RENAME "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses/LICENSE" "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses/onnxruntime-LICENSE.txt")
file(COPY "${_ort_root}/ThirdPartyNotices.txt" DESTINATION "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses")
file(COPY "${_cudart_root}/LICENSE" DESTINATION "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses")
file(RENAME "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses/LICENSE" "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses/cuda-cudart-LICENSE.txt")
file(WRITE "${DSPTB_GPU_RUNTIME_STAGE_DIR}/licenses/README.txt"
  "The LibTorch zip carries no licence files. Before any public distribution, add PyTorch's LICENSE and NOTICE\n"
  "and NVIDIA's licence texts for cuDNN, cuBLAS and the other CUDA libraries (docs/GPU_RUNTIME.md section 12).\n")

# The manifest lists every staged third-party file with its SHA-256. Hashing 2.9 GB takes a while, so it is
# rewritten only when a file was added or the pins changed.
set(_old_stamp "")
if(EXISTS "${_manifest}")
  file(READ "${_manifest}" _old_json)
  string(JSON _old_stamp ERROR_VARIABLE _e GET "${_old_json}" pins)
endif()
if(_need_manifest OR NOT _old_stamp STREQUAL _pin_stamp)
  message(STATUS "Writing gpu-runtime.json (hashing the staged files)")
  set(_files_json "[]")
  set(_i 0)
  foreach(_f IN LISTS _stage_files)
    get_filename_component(_n "${_f}" NAME)
    file(SHA256 "${_stage_bin}/${_n}" _h)
    string(JSON _files_json SET "${_files_json}" ${_i} "{\"path\": \"bin/${_n}\", \"sha256\": \"${_h}\"}")
    math(EXPR _i "${_i} + 1")
  endforeach()
  set(_json "{}")
  string(JSON _json SET "${_json}" schema "1")
  string(JSON _json SET "${_json}" runtimeAbi "1")
  string(JSON _json SET "${_json}" pins "\"${_pin_stamp}\"")
  string(JSON _json SET "${_json}" cuda "\"13.0\"")
  string(JSON _json SET "${_json}" cudnn "\"${DSPTB_CUDNN_VERSION}\"")
  string(JSON _json SET "${_json}" onnxruntime "\"${DSPTB_ORT_VERSION}\"")
  string(JSON _json SET "${_json}" libtorch "\"${DSPTB_LIBTORCH_VERSION}+cu130\"")
  string(JSON _json SET "${_json}" files "${_files_json}")
  file(WRITE "${_manifest}" "${_json}\n")
endif()
message(STATUS "GPU runtime staged in ${DSPTB_GPU_RUNTIME_STAGE_DIR}")
