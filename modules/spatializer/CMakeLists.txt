include(${CMAKE_SOURCE_DIR}/cmake/FetchSteamAudio.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/FetchHrtfSets.cmake)

# phonon.dll goes into the package next to the module DLL; Steam Audio's license and notices travel with it
# in the module zip.
dsptb_add_module(spatializer
  ID org.dsptb.spatializer
  DISTRIBUTION SEPARATE
  SOURCES src/spatializer.cpp src/scene.cpp src/ui.cpp src/user_dirs.cpp
  LINK_LIBRARIES SteamAudio::phonon dsptb::spatial_motion dsptb_branding shell32 ole32
  DEPENDENCY_DLLS "${DSPTB_STEAMAUDIO_DLL}"
  ASSETS "${DSPTB_HRTF_ASSET_DIR}"
  EXTRA_FILES
    "${CMAKE_SOURCE_DIR}/third_party/LICENSES/SteamAudio-LICENSE.txt"
    "${CMAKE_SOURCE_DIR}/third_party/LICENSES/SteamAudio-THIRDPARTY.md"
    "${CMAKE_SOURCE_DIR}/third_party/LICENSES/HRTF-SETS.md")

