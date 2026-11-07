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
  integration/test_asio.cpp)
target_link_libraries(dsptb_tests PRIVATE dsptb_core dsptb::dsp_common doctest::doctest miniz::miniz dsptb_warnings)
# test_cli.cpp runs the real CLI executable.
add_dependencies(dsptb_tests dsptb-cli gain spectrogram levelmeter spatializer dsptb_test_faulty dsptb_test_allocating
  dsptb_test_probe dsptb_test_abi_mismatch)
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
