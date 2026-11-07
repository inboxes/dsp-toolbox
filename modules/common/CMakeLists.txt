cmake_minimum_required(VERSION 3.28)

project(dsptb_common VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# modules/common — shared DSP libraries
#
# This project works in two modes:
#   * Subdirectory of the main build: the top-level CMakeLists.txt adds us
#     with add_subdirectory(modules/common). Dependencies come from vcpkg
#     (find_package / find_path / find_library) and warnings come from the
#     dsptb_warnings target when it exists.
#   * Standalone (PROJECT_IS_TOP_LEVEL): dependencies are fetched with
#     FetchContent at the pinned versions below, so people on any OS
#     can build and test without vcpkg or the Windows app.
# ---------------------------------------------------------------------------

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
endif()

include(FetchContent)

# Pinned dependency versions for the standalone build. Keep the
# README table in sync when these change.
set(DSPTB_DOCTEST_VERSION "v2.4.11")
set(DSPTB_LIBEBUR128_VERSION "v1.2.6")
# marton78/pffft, pinned by commit SHA (no suitable release tag exists).
set(DSPTB_PFFFT_COMMIT "7ae36e8c1d352cba66231de953cfa35f235b9e7a")

# SDK headers: the only part of the wider repo these libraries may include.
if(PROJECT_IS_TOP_LEVEL)
    set(DSPTB_SDK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include")
else()
    set(DSPTB_SDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/sdk/include")
endif()

# Warnings for our own targets in standalone mode. In the main build the
# dsptb_warnings target carries the equivalent flags.
function(dsptb_common_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# Applies the right warning setup for the current mode to one of our targets.
function(dsptb_common_setup_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(NOT PROJECT_IS_TOP_LEVEL AND TARGET dsptb_warnings)
        target_link_libraries(${target} PRIVATE dsptb_warnings)
    else()
        dsptb_common_apply_warnings(${target})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

if(PROJECT_IS_TOP_LEVEL)
    # --- Standalone: FetchContent at pinned versions -----------------------
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG ${DSPTB_DOCTEST_VERSION})
    FetchContent_Declare(libebur128
        GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
        GIT_TAG ${DSPTB_LIBEBUR128_VERSION})
    FetchContent_Declare(pffft
        GIT_REPOSITORY https://github.com/marton78/pffft.git
        GIT_TAG ${DSPTB_PFFFT_COMMIT})

    # libebur128: build it static, with its tests and examples off.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build libebur128 static" FORCE)
    set(ENABLE_TESTS OFF CACHE BOOL "libebur128 tests off" FORCE)
    # PFFFT: only the library, no tests / benchmarks / examples / install.
    set(PFFFT_BUILD_TESTS OFF CACHE BOOL "PFFFT tests off" FORCE)
    set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "PFFFT benchmarks off" FORCE)
    set(PFFFT_BUILD_EXAMPLES OFF CACHE BOOL "PFFFT examples off" FORCE)
    set(INSTALL_PFFFT OFF CACHE BOOL "PFFFT install off" FORCE)
    set(INSTALL_PFDSP OFF CACHE BOOL "PFFFT pfdsp install off" FORCE)
    set(INSTALL_PFFASTCONV OFF CACHE BOOL "PFFFT pffastconv install off" FORCE)

    # CMake 4 removed compatibility with cmake_minimum_required below 3.5,
    # and two pinned deps declare older minimums (doctest v2.4.11: 3.0,
    # libebur128 v1.2.6: 2.8.12). Set the policy floor CMake 4 provides for
    # exactly this case, so the pinned projects configure as if their
    # minimum were 3.5. Standalone mode only; a user-supplied value wins.
    if(NOT DEFINED CACHE{CMAKE_POLICY_VERSION_MINIMUM})
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
            "Policy floor for pinned FetchContent dependencies")
    endif()

    FetchContent_MakeAvailable(doctest libebur128 pffft)

    if(TARGET PFFFT::PFFFT)
        set(DSPTB_PFFFT_TARGET PFFFT::PFFFT)
    elseif(TARGET PFFFT)
        set(DSPTB_PFFFT_TARGET PFFFT)
    else()
        message(FATAL_ERROR "PFFFT FetchContent did not provide a target")
    endif()
    # libebur128 v1.2.6 exposes its header from its ebur128/ source dir and
    # does not publish an include directory on its target.
    set(DSPTB_EBUR128_TARGET ebur128)
    set(DSPTB_EBUR128_INCLUDE_DIRS "${libebur128_SOURCE_DIR}/ebur128")
    set(DSPTB_DOCTEST_TARGET doctest::doctest)
else()
    # --- Main build: dependencies come from vcpkg --------------------------
    find_package(doctest CONFIG REQUIRED)
    set(DSPTB_DOCTEST_TARGET doctest::doctest)

    # PFFFT: the vcpkg port (jpommier/pffft) ships a CMake config with the
    # target pffft::pffft; fall back to find_path/find_library if it has none
    # in some configuration.
    find_package(pffft CONFIG QUIET)
    find_package(PFFFT CONFIG QUIET)
    if(TARGET pffft::pffft)
        set(DSPTB_PFFFT_TARGET pffft::pffft)
    elseif(TARGET PFFFT::PFFFT)
        set(DSPTB_PFFFT_TARGET PFFFT::PFFFT)
    elseif(TARGET PFFFT)
        set(DSPTB_PFFFT_TARGET PFFFT)
    elseif(TARGET pffft)
        set(DSPTB_PFFFT_TARGET pffft)
    else()
        find_path(DSPTB_PFFFT_INCLUDE_DIR NAMES pffft.h pffft/pffft.h REQUIRED)
        find_library(DSPTB_PFFFT_LIBRARY NAMES pffft REQUIRED)
        add_library(dsptb_pffft_imported UNKNOWN IMPORTED)
        set_target_properties(dsptb_pffft_imported PROPERTIES
            IMPORTED_LOCATION "${DSPTB_PFFFT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${DSPTB_PFFFT_INCLUDE_DIR}")
        set(DSPTB_PFFFT_TARGET dsptb_pffft_imported)
    endif()

    # libebur128: the vcpkg port ships no CMake config, so use
    # find_path/find_library (find_package first, in case that changes).
    find_package(libebur128 CONFIG QUIET)
    find_package(unofficial-libebur128 CONFIG QUIET)
    if(TARGET libebur128::libebur128)
        set(DSPTB_EBUR128_TARGET libebur128::libebur128)
    elseif(TARGET unofficial::libebur128::libebur128)
        set(DSPTB_EBUR128_TARGET unofficial::libebur128::libebur128)
    elseif(TARGET ebur128)
        set(DSPTB_EBUR128_TARGET ebur128)
    else()
        find_path(DSPTB_EBUR128_INCLUDE_DIR NAMES ebur128.h REQUIRED)
        find_library(DSPTB_EBUR128_LIBRARY NAMES ebur128 libebur128 REQUIRED)
        add_library(dsptb_ebur128_imported UNKNOWN IMPORTED)
        set_target_properties(dsptb_ebur128_imported PROPERTIES
            IMPORTED_LOCATION "${DSPTB_EBUR128_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${DSPTB_EBUR128_INCLUDE_DIR}")
        set(DSPTB_EBUR128_TARGET dsptb_ebur128_imported)
        set(DSPTB_EBUR128_INCLUDE_DIRS "${DSPTB_EBUR128_INCLUDE_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

add_library(dsptb_dsp_common STATIC
    dsp_common/src/mel.cpp
    dsp_common/src/freq_map.cpp
    dsp_common/src/colormap.cpp
    dsp_common/src/loudness.cpp
    dsp_common/src/fft.cpp
    dsp_common/src/onset.cpp
    dsp_common/src/stft.cpp
    dsp_common/src/spectral_features.cpp
    dsp_common/src/pitch.cpp
    dsp_common/src/psola.cpp
    dsp_common/src/key.cpp
    dsp_common/src/tuning.cpp
    dsp_common/src/pitch_spectrum.cpp
    dsp_common/src/tuning_estimator.cpp
    dsp_common/src/version.cpp)
add_library(dsptb::dsp_common ALIAS dsptb_dsp_common)
target_include_directories(dsptb_dsp_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/dsp_common/include"
        "${DSPTB_SDK_INCLUDE_DIR}")
# Third-party deps are an implementation detail: no public header includes
# them in this scaffold, so they stay PRIVATE.
target_link_libraries(dsptb_dsp_common
    PRIVATE
        ${DSPTB_PFFFT_TARGET}
        ${DSPTB_EBUR128_TARGET})
if(DSPTB_EBUR128_INCLUDE_DIRS)
    target_include_directories(dsptb_dsp_common PRIVATE ${DSPTB_EBUR128_INCLUDE_DIRS})
endif()
dsptb_common_setup_target(dsptb_dsp_common)

add_library(dsptb_spatial_motion STATIC
    spatial_motion/src/motion.cpp
    spatial_motion/src/version.cpp)
add_library(dsptb::spatial_motion ALIAS dsptb_spatial_motion)
target_include_directories(dsptb_spatial_motion
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/spatial_motion/include"
        "${DSPTB_SDK_INCLUDE_DIR}")
dsptb_common_setup_target(dsptb_spatial_motion)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

set(DSPTB_COMMON_BUILD_TESTS ON)
if(NOT PROJECT_IS_TOP_LEVEL AND DEFINED DSPTB_BUILD_TESTS AND NOT DSPTB_BUILD_TESTS)
    set(DSPTB_COMMON_BUILD_TESTS OFF)
endif()

if(DSPTB_COMMON_BUILD_TESTS)
    enable_testing()

    # Globbed with CONFIGURE_DEPENDS so later issues only add files.
    file(GLOB DSPTB_COMMON_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")

    add_executable(dsptb_common_tests ${DSPTB_COMMON_TEST_SOURCES})
    target_include_directories(dsptb_common_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            "${DSPTB_SDK_INCLUDE_DIR}")
    if(DSPTB_EBUR128_INCLUDE_DIRS)
        target_include_directories(dsptb_common_tests PRIVATE ${DSPTB_EBUR128_INCLUDE_DIRS})
    endif()
    target_link_libraries(dsptb_common_tests
        PRIVATE
            dsptb::dsp_common
            dsptb::spatial_motion
            ${DSPTB_DOCTEST_TARGET}
            ${DSPTB_PFFFT_TARGET}
            ${DSPTB_EBUR128_TARGET})
    dsptb_common_setup_target(dsptb_common_tests)

    add_test(NAME dsptb_common_tests COMMAND dsptb_common_tests)
endif()
