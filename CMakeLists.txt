cmake_minimum_required(VERSION 3.28)

project(dsptb VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(DSPTB_RT_GUARD "Enable the audio-thread allocation guard" OFF)
option(DSPTB_BUILD_TESTS "Build unit tests" ON)
# Semicolon list of folders outside the repo whose subfolders are extra (private) modules.
set(DSPTB_EXTRA_MODULE_DIRS "" CACHE STRING "Folders with extra module sources, built like first-party modules")

set(DSPTB_IN_TREE TRUE)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Branding)
include(Warnings)
include(DsptbModule)
dsptb_generate_branding()

find_package(imgui CONFIG REQUIRED)
# The vcpkg miniaudio port is header-only and ships no CMake config.
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h" REQUIRED)
add_library(miniaudio_headers INTERFACE)
target_include_directories(miniaudio_headers SYSTEM INTERFACE "${MINIAUDIO_INCLUDE_DIRS}")
# These macros change miniaudio's struct layouts, so every translation unit that includes it must see the
# same set: WASAPI only (plus the built-in WAV/FLAC/MP3 decoders and the WAV encoder), no engine/node graph.
target_compile_definitions(miniaudio_headers INTERFACE MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI
  MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER)

# miniaudio's implementation: its own library so our strict warning flags do not apply to third-party code.
add_library(dsptb_miniaudio_impl STATIC core/src/miniaudio_impl.cpp)
target_link_libraries(dsptb_miniaudio_impl PUBLIC miniaudio_headers)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)

# --- SDK: header-only; the only thing modules include ---
add_library(dsptb_sdk INTERFACE)
add_library(dsptb::sdk ALIAS dsptb_sdk)
target_include_directories(dsptb_sdk INTERFACE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_features(dsptb_sdk INTERFACE cxx_std_20)
target_link_libraries(dsptb_sdk INTERFACE nlohmann_json::nlohmann_json)
# Every module DLL has its own operator new, so it needs the guard switched on exactly like the host.
if(DSPTB_RT_GUARD)
  target_compile_definitions(dsptb_sdk INTERFACE DSPTB_RT_GUARD=1)
endif()

# The ABI header must compile as C and as C++ with warnings as errors (layout asserts included).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_library(dsptb_abi_compile_test STATIC tests/abi/abi_c.c tests/abi/abi_cpp.cpp)
target_link_libraries(dsptb_abi_compile_test PRIVATE dsptb_sdk dsptb_warnings)

# --- Core: no UI, no D3D ---
add_library(dsptb_core STATIC
  core/src/version.cpp
  core/src/paths.cpp
  core/src/settings.cpp
  core/src/logging.cpp
  core/src/notifications.cpp
  core/src/audio_io.cpp
  core/src/signal_gen.cpp
  core/src/analysis.cpp
  core/src/backend.cpp
  core/src/null_backend.cpp
  core/src/miniaudio_backend.cpp
  core/src/engine.cpp
  core/src/worker_pool.cpp
  core/src/file_loader.cpp
  core/src/audio_session.cpp
  core/src/module_manifest.cpp
  core/src/seh_guard.cpp
  core/src/module_registry.cpp
  core/src/module_instance.cpp
  core/src/chain_processor.cpp
  core/src/analysis_host.cpp
  core/src/offline_analyze.cpp
  core/src/session_model.cpp
  core/src/session_file.cpp
  core/src/session_io.cpp
  core/src/recorder.cpp
  core/src/export_job.cpp
  core/src/offline_render.cpp
  core/src/recording_canvas.cpp
  core/src/module_validator.cpp
  core/src/module_install.cpp)
target_include_directories(dsptb_core PUBLIC "${CMAKE_SOURCE_DIR}/core/include")

# --- Optional ASIO backend. Off by default: the SDK is downloaded
#     (tools/fetch-asio-sdk.ps1), never committed. The project is GPLv3, like the SDK's GPL option. ---
option(DSPTB_ENABLE_ASIO "Build the ASIO audio backend (needs the Steinberg ASIO SDK)" OFF)
set(DSPTB_ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/asio-sdk/ASIOSDK" CACHE PATH "Folder of the Steinberg ASIO SDK")
if(DSPTB_ENABLE_ASIO)
  if(NOT EXISTS "${DSPTB_ASIO_SDK_DIR}/common/iasiodrv.h")
    message(FATAL_ERROR "DSPTB_ENABLE_ASIO is ON but the ASIO SDK was not found at '${DSPTB_ASIO_SDK_DIR}' "
      "(expected common/iasiodrv.h). Run tools/fetch-asio-sdk.ps1, or set -DDSPTB_ASIO_SDK_DIR=<folder>.")
  endif()
  message(STATUS "ASIO backend enabled (SDK: ${DSPTB_ASIO_SDK_DIR}). The SDK is used under its GPLv3 option.")
  target_sources(dsptb_core PRIVATE core/src/asio_backend.cpp)
  target_include_directories(dsptb_core SYSTEM PRIVATE "${DSPTB_ASIO_SDK_DIR}/common")
  target_compile_definitions(dsptb_core PUBLIC DSPTB_HAS_ASIO=1)
endif()
target_link_libraries(dsptb_core PUBLIC dsptb_sdk dsptb_branding spdlog::spdlog nlohmann_json::nlohmann_json
  PRIVATE dsptb_warnings dsptb_miniaudio_impl miniaudio_headers miniz::miniz shell32 ole32 psapi)

# --- App ---
add_executable(DSPToolbox WIN32
  app/src/main.cpp
  app/src/app.cpp
  app/src/d3d11_context.cpp
  app/src/png_writer.cpp
  app/src/png_reader.cpp
  app/src/ui/theme.cpp
  app/src/ui/shell.cpp
  app/src/ui/audio_ui.cpp
  app/src/ui/file_dialog.cpp
  app/src/ui/canvas_impl.cpp
  app/src/ui/module_ui.cpp
  app/src/ui/session_ui.cpp
  app/app.manifest
  app/app.rc)
# The resource compiler reports no dependency on the icon, so a new icon wouldn't otherwise rebuild it.
set_source_files_properties(app/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/app.ico")
set_target_properties(DSPToolbox PROPERTIES OUTPUT_NAME "${DSPTB_EXE_NAME}")
if(DSPTB_ENABLE_ASIO)
  # Steinberg's guidelines ask for the logo in the dialog that enables ASIO (docs/RELEASING.md).
  add_custom_command(TARGET DSPToolbox POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/docs/images/asio-compatible.png" "$<TARGET_FILE_DIR:DSPToolbox>/assets/asio-compatible.png")
endif()
target_link_libraries(DSPToolbox PRIVATE dsptb_core imgui::imgui d3d11 dxgi d3dcompiler dwmapi
  windowscodecs shell32 ole32 uuid
  dsptb_warnings)

# --- CLI ---
add_executable(dsptb-cli cli/src/main.cpp cli/src/cmd_basic.cpp cli/src/cmd_bench.cpp cli/src/cmd_probe.cpp cli/src/cmd_latency.cpp cli/src/cmd_module.cpp)
target_link_libraries(dsptb-cli PRIVATE dsptb_core CLI11::CLI11 dsptb_warnings)

# --- Modules ---
if(DSPTB_BUILD_TESTS)
  enable_testing()
endif()
# Shared DSP libraries. They also build standalone (dsp-common.yml). It
# comes before the modules because Spectrogram and Level Meter link dsptb::dsp_common.
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/common/CMakeLists.txt")
  add_subdirectory(modules/common)
endif()

add_subdirectory(modules)
foreach(extra_dir IN LISTS DSPTB_EXTRA_MODULE_DIRS)
  file(GLOB extra_children LIST_DIRECTORIES true "${extra_dir}/*")
  foreach(child IN LISTS extra_children)
    if(IS_DIRECTORY "${child}" AND EXISTS "${child}/CMakeLists.txt")
      get_filename_component(child_name "${child}" NAME)
      add_subdirectory("${child}" "${CMAKE_BINARY_DIR}/extra-modules/${child_name}")
    endif()
  endforeach()
endforeach()

# --- Tests ---
if(DSPTB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- Release packages. After everything else, so every module is known. ---
include(Packaging)
