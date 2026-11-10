# GPU runtime (MIT, see LICENSE). Everything that touches CUDA, ONNX Runtime or LibTorch lives here; the GPL
# host reaches it only through include/dsptb/gpu_runtime.h. With DSPTB_GPU_RUNTIME=OFF only the backend-free
# pipeline headers and their unit tests build (they need no GPU and run in CI).
# The backend-free helper library: the latency pipeline (pipeline.hpp, header-only) and the worker thread helpers
# (worker.cpp, the only part that includes <windows.h>). No CUDA, always built. Its unit tests use fake stages, so they
# run everywhere, including CI.
add_library(dsptb_gpu_pipeline STATIC sdk/src/worker.cpp)
add_library(dsptb::gpu_pipeline ALIAS dsptb_gpu_pipeline)
target_include_directories(dsptb_gpu_pipeline PUBLIC sdk/include)
target_compile_features(dsptb_gpu_pipeline PUBLIC cxx_std_20)
target_link_libraries(dsptb_gpu_pipeline PUBLIC avrt PRIVATE dsptb_warnings)

if(DSPTB_BUILD_TESTS)
  add_executable(dsptb_gpu_pipeline_tests tests/test_pipeline.cpp)
  target_link_libraries(dsptb_gpu_pipeline_tests PRIVATE dsptb_gpu_pipeline dsptb_sdk doctest::doctest dsptb_warnings)
  add_test(NAME gpu_pipeline COMMAND dsptb_gpu_pipeline_tests)
endif()

if(DSPTB_GPU_RUNTIME)
  include(FetchGpuRuntime)

  # dsptb_gpu.dll sits in the runtime folder itself, next to gpu-runtime.json. It imports nothing from CUDA,
  # ORT or LibTorch (everything is resolved with GetProcAddress after the preload), so the host can load it
  # before the runtime bin folder is on the DLL search path. Only the CUDA headers are used, for types.
  add_library(dsptb_gpu SHARED runtime/gpu_runtime.cpp)
  add_library(dsptb::gpu ALIAS dsptb_gpu)
  target_include_directories(dsptb_gpu PUBLIC include)
  target_include_directories(dsptb_gpu SYSTEM PRIVATE "${DSPTB_GPU_CUDA_INCLUDE}")
  target_link_libraries(dsptb_gpu PRIVATE nlohmann_json::nlohmann_json dsptb_warnings)
  set_target_properties(dsptb_gpu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DSPTB_GPU_RUNTIME_STAGE_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

  # The stages and their helpers (ORT, LibTorch, CUDA). Modules, tests and tools link this; it reaches dsptb_gpu.dll
  # at run time through sdk/include/dsptb/gpu/runtime_api.hpp, so nothing imports the runtime DLL.
  add_library(dsptb_gpu_sdk STATIC sdk/src/runtime_api.cpp sdk/src/ort_stage.cpp sdk/src/torch_stage.cpp)
  add_library(dsptb::gpu_sdk ALIAS dsptb_gpu_sdk)
  target_include_directories(dsptb_gpu_sdk PUBLIC sdk/include include)
  target_link_libraries(dsptb_gpu_sdk PUBLIC dsptb::gpu_pipeline GpuRuntime::onnxruntime GpuRuntime::torch GpuRuntime::cudart
    PRIVATE dsptb_warnings)
  # LibTorch headers trigger warnings when their templates are instantiated in our file (narrowing in function_schema.h and
  # compiled_autograd.h, unreachable code in irange.h). MSVC reports the first group inside the std headers, where
  # /external:W0 for LibTorch does not reach, so /WX would fail on third-party code. Warnings stay on for this one file but
  # are not errors; the rest of gpu/ keeps /W4 /WX.
  set_source_files_properties(sdk/src/torch_stage.cpp PROPERTIES COMPILE_OPTIONS "/WX-")

  # Executables that use the stages run from <runtime>/bin, so Windows finds ORT, LibTorch and CUDA next to them.
  function(dsptb_gpu_add_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE dsptb::gpu_sdk dsptb_warnings)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DSPTB_GPU_RUNTIME_STAGE_DIR}/bin")
    add_dependencies(${target} dsptb_gpu)
  endfunction()

  # The benchmark tool (spec 9.3). gpu_load.cpp includes LibTorch headers, so it gets the same /WX- as torch_stage.cpp.
  dsptb_gpu_add_executable(dsptb-gpu-bench tools/bench/bench.cpp tools/bench/gpu_load.cpp)
  target_link_libraries(dsptb-gpu-bench PRIVATE CLI11::CLI11 nlohmann_json::nlohmann_json)
  set_source_files_properties(tools/bench/gpu_load.cpp PROPERTIES COMPILE_OPTIONS "/WX-")

  # The test module for the whole path (docs/GPU_RUNTIME.md section 9.2). A TEST_MODULE: it lands in bin/test-modules and
  # is never packaged. Its models are copied into the package as assets.
  dsptb_add_module(dsptb_test_gpu_probe ID test.gpu-probe TEST_MODULE
    MANIFEST test-module/module.json SOURCES test-module/probe.cpp ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/tests/models"
    LINK_LIBRARIES dsptb::gpu_sdk)

  if(DSPTB_BUILD_TESTS AND TARGET dsptb-cli)
    # validate passes for the probe with the runtime, and is SKIPPED (exit 0 with --allow-skip) without it.
    add_test(NAME gpu_probe_validate COMMAND dsptb-cli validate "${CMAKE_BINARY_DIR}/bin/test-modules/test.gpu-probe" --allow-skip)
    set_tests_properties(gpu_probe_validate PROPERTIES LABELS gpu)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/no-gpu-runtime")
    add_test(NAME gpu_probe_validate_skip COMMAND dsptb-cli validate "${CMAKE_BINARY_DIR}/bin/test-modules/test.gpu-probe" --allow-skip)
    set_tests_properties(gpu_probe_validate_skip PROPERTIES LABELS gpu
      ENVIRONMENT "DSPTB_GPU_RUNTIME_DIR=${CMAKE_BINARY_DIR}/no-gpu-runtime"
      PASS_REGULAR_EXPRESSION "SKIPPED: needs the GPU runtime")
  endif()

  if(DSPTB_BUILD_TESTS)
    add_executable(dsptb_gpu_tests tests/test_gpu_runtime.cpp)
    target_link_libraries(dsptb_gpu_tests PRIVATE doctest::doctest dsptb_warnings)
    target_include_directories(dsptb_gpu_tests PRIVATE include)
    target_compile_definitions(dsptb_gpu_tests PRIVATE DSPTB_GPU_RUNTIME_DIR="${DSPTB_GPU_RUNTIME_STAGE_DIR}")
    add_dependencies(dsptb_gpu_tests dsptb_gpu)
    add_test(NAME gpu_runtime COMMAND dsptb_gpu_tests)
    set_tests_properties(gpu_runtime PROPERTIES LABELS gpu)

    # The real stages on the tiny models (G7, G8, G9). They skip themselves without an NVIDIA GPU.
    dsptb_gpu_add_executable(dsptb_gpu_stage_tests tests/test_gpu_stages.cpp)
    target_link_libraries(dsptb_gpu_stage_tests PRIVATE doctest::doctest)
    target_compile_definitions(dsptb_gpu_stage_tests PRIVATE DSPTB_GPU_RUNTIME_DIR="${DSPTB_GPU_RUNTIME_STAGE_DIR}"
      DSPTB_GPU_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/tests/models")
    add_test(NAME gpu_stages COMMAND dsptb_gpu_stage_tests)
    set_tests_properties(gpu_stages PROPERTIES LABELS gpu TIMEOUT 300)
  endif()
endif()
