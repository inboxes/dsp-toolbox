add_executable(dsptb_tests
  unit/test_main.cpp unit/test_version.cpp unit/test_shell_core.cpp unit/test_rt_primitives.cpp
  unit/test_audio_tools.cpp
  unit/test_duplex_bridge.cpp
  unit/test_noise_gate.cpp
  unit/test_module_system.cpp
  unit/test_module_install.cpp
  integration/test_engine.cpp
  integration/test_audio_session.cpp
  integration/test_cli.cpp
  integration/test_modules.cpp
  integration/test_analyzers.cpp
  integration/test_analysis.cpp
  integration/test_spatializer.cpp
  integration/test_sessions.cpp
  integration/test_asio.cpp
  integration/test_gpu_loader.cpp)
target_link_libraries(dsptb_tests PRIVATE dsptb_core dsptb::dsp_common doctest::doctest miniz::miniz dsptb_warnings)
# test_cli.cpp runs the real CLI executable.
add_dependencies(dsptb_tests dsptb-cli gain spectrogram levelmeter spatializer dsptb_test_faulty dsptb_test_allocating
  dsptb_test_probe dsptb_test_abi_mismatch dsptb_test_gpu_gated dsptb_test_gpu_future_abi
  dsptb_test_gpu_unknown_requires dsptb_fake_gpu_runtime)
# A stand-in dsptb_gpu.dll for the loader and gating tests (it answers from environment variables).
add_library(dsptb_fake_gpu_runtime SHARED fixtures/fake_gpu_runtime/fake_gpu_runtime.cpp)
target_include_directories(dsptb_fake_gpu_runtime PRIVATE "${CMAKE_SOURCE_DIR}/gpu/include")
target_link_libraries(dsptb_fake_gpu_runtime PRIVATE dsptb_warnings)
set_target_properties(dsptb_fake_gpu_runtime PROPERTIES OUTPUT_NAME dsptb_gpu
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test-fake-gpu-runtime"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-fake-gpu-runtime"
  PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-fake-gpu-runtime")
target_compile_definitions(dsptb_tests PRIVATE DSPTB_FAKE_GPU_DIR="${CMAKE_BINARY_DIR}/bin/test-fake-gpu-runtime")
if(DSPTB_GPU_RUNTIME)
  target_compile_definitions(dsptb_tests PRIVATE DSPTB_GPU_RUNTIME_STAGE_DIR="${DSPTB_GPU_RUNTIME_STAGE_DIR}")
  add_dependencies(dsptb_tests dsptb_gpu)
  # The real runtime, the real stages and the probe module (G10 to G15). Its own executable: the fake dsptb_gpu.dll of
  # the loader tests has the same file name as the real one, and the two cannot share a process.
  add_executable(dsptb_gpu_integration_tests unit/test_main.cpp integration/test_gpu_runtime.cpp)
  target_link_libraries(dsptb_gpu_integration_tests PRIVATE dsptb_core doctest::doctest dsptb_warnings)
  add_dependencies(dsptb_gpu_integration_tests dsptb_gpu dsptb_test_gpu_probe gain)
  target_compile_definitions(dsptb_gpu_integration_tests PRIVATE DSPTB_GPU_RUNTIME_STAGE_DIR="${DSPTB_GPU_RUNTIME_STAGE_DIR}"
    DSPTB_MODULES_DIR="${CMAKE_BINARY_DIR}/bin/modules" DSPTB_TEST_MODULES_DIR="${CMAKE_BINARY_DIR}/bin/test-modules")
  add_test(NAME gpu_integration COMMAND dsptb_gpu_integration_tests)
  set_tests_properties(gpu_integration PROPERTIES LABELS gpu TIMEOUT 600)
endif()
target_compile_definitions(dsptb_tests PRIVATE DSPTB_CLI_PATH="$<TARGET_FILE:dsptb-cli>"
  DSPTB_MODULES_DIR="${CMAKE_BINARY_DIR}/bin/modules" DSPTB_TEST_MODULES_DIR="${CMAKE_BINARY_DIR}/bin/test-modules")

add_subdirectory(modules)

include(CTest)
add_test(NAME dsptb_tests COMMAND dsptb_tests)
# Release packaging: what the app package contains, and a module built from the SDK package alone.
add_test(NAME package_contents COMMAND powershell -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_SOURCE_DIR}/tools/check-package.ps1" -BuildDir "${CMAKE_BINARY_DIR}")
add_test(NAME sdk_out_of_tree COMMAND powershell -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_SOURCE_DIR}/tools/check-sdk.ps1" -BuildDir "${CMAKE_BINARY_DIR}")
