# Steam Audio (Apache-2.0), used by the 3D Spatializer module only.
#
# Downloads the official release zip from GitHub, verifies its SHA-256, extracts just the headers and the
# Windows x64 import library and DLL, and defines the imported target SteamAudio::phonon. Nothing from Steam
# Audio is committed: the archive and the extracted files live under build/ (gitignored).
#
# The extraction is shared by every preset (build/_deps/steamaudio-<version>), so the 181 MB archive is
# downloaded once per checkout. To work offline, put the zip at build/_downloads/steamaudio_<version>.zip.
#
# Pinned release: v4.8.1, the latest 4.x on 2026-11-05. To update, change
# the version and the hash together; the hash is the SHA-256 GitHub shows for the release asset.

include_guard(GLOBAL)

set(DSPTB_STEAMAUDIO_VERSION "4.8.1")
set(DSPTB_STEAMAUDIO_SHA256 "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449")
set(DSPTB_STEAMAUDIO_URL
  "https://github.com/ValveSoftware/steam-audio/releases/download/v${DSPTB_STEAMAUDIO_VERSION}/steamaudio_${DSPTB_STEAMAUDIO_VERSION}.zip")

set(_sa_downloads "${CMAKE_SOURCE_DIR}/build/_downloads")
set(_sa_zip "${_sa_downloads}/steamaudio_${DSPTB_STEAMAUDIO_VERSION}.zip")
set(_sa_dir "${CMAKE_SOURCE_DIR}/build/_deps/steamaudio-${DSPTB_STEAMAUDIO_VERSION}")
set(_sa_root "${_sa_dir}/steamaudio")

if(NOT EXISTS "${_sa_root}/lib/windows-x64/phonon.dll")
  if(NOT EXISTS "${_sa_zip}")
    message(STATUS "Downloading Steam Audio ${DSPTB_STEAMAUDIO_VERSION} (181 MB, once)")
    file(MAKE_DIRECTORY "${_sa_downloads}")
    file(DOWNLOAD "${DSPTB_STEAMAUDIO_URL}" "${_sa_zip}"
      EXPECTED_HASH SHA256=${DSPTB_STEAMAUDIO_SHA256}
      SHOW_PROGRESS STATUS _sa_status)
    list(GET _sa_status 0 _sa_code)
    if(NOT _sa_code EQUAL 0)
      file(REMOVE "${_sa_zip}")
      message(FATAL_ERROR "Could not download Steam Audio from ${DSPTB_STEAMAUDIO_URL}: ${_sa_status}\n"
        "Download steamaudio_${DSPTB_STEAMAUDIO_VERSION}.zip by hand to ${_sa_zip} and configure again.")
    endif()
  else()
    file(SHA256 "${_sa_zip}" _sa_actual)
    if(NOT _sa_actual STREQUAL DSPTB_STEAMAUDIO_SHA256)
      message(FATAL_ERROR "${_sa_zip} has SHA-256 ${_sa_actual}, expected ${DSPTB_STEAMAUDIO_SHA256}. Delete it.")
    endif()
  endif()
  message(STATUS "Extracting Steam Audio ${DSPTB_STEAMAUDIO_VERSION}")
  file(ARCHIVE_EXTRACT INPUT "${_sa_zip}" DESTINATION "${_sa_dir}"
    PATTERNS "steamaudio/include/*" "steamaudio/lib/windows-x64/*" "steamaudio/THIRDPARTY.md")
  if(NOT EXISTS "${_sa_root}/lib/windows-x64/phonon.dll")
    message(FATAL_ERROR "Steam Audio extraction did not produce ${_sa_root}/lib/windows-x64/phonon.dll")
  endif()
endif()

add_library(SteamAudio::phonon SHARED IMPORTED GLOBAL)
set_target_properties(SteamAudio::phonon PROPERTIES
  IMPORTED_LOCATION "${_sa_root}/lib/windows-x64/phonon.dll"
  IMPORTED_IMPLIB "${_sa_root}/lib/windows-x64/phonon.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${_sa_root}/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_sa_root}/include")
set(DSPTB_STEAMAUDIO_DLL "${_sa_root}/lib/windows-x64/phonon.dll")
set(DSPTB_STEAMAUDIO_NOTICE "${_sa_root}/THIRDPARTY.md")
