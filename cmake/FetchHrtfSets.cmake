# HRTF sets packaged with the 3D Spatializer.
#
#   near-kemar-20cm, near-kemar-50cm   committed in modules/spatializer/hrtf/: single-distance shells cut from
#                                      Li's KEMAR near-field set (CC BY 4.0) with tools/sofa (dsptb-sofa slice)
#   mid-sadie-d1                       downloaded here: the SADIE II D1 (Neumann KU100) HRIRs at 1.2 m, 48 kHz,
#                                      256 taps (Apache-2.0). Hash-pinned; not committed (36 MB).
#
# Everything is staged into build/_deps/hrtf-assets, which the module package copies to <package>/assets. To
# work offline, put the zip at build/_downloads/sadie2_D1_HRIR_SOFA.zip. Licences and attribution:
# third_party/LICENSES/HRTF-SETS.md.

include_guard(GLOBAL)

set(DSPTB_SADIE_D1_SHA256 "366321FA78F211BACC0EC6BEA96701625B196A5DB54EC16748B0EAB0B9705F75")
set(DSPTB_SADIE_D1_URL "https://zenodo.org/api/records/10886409/files/D1_HRIR_SOFA.zip/content")

set(_hs_downloads "${CMAKE_SOURCE_DIR}/build/_downloads")
set(_hs_zip "${_hs_downloads}/sadie2_D1_HRIR_SOFA.zip")
set(_hs_stage "${CMAKE_SOURCE_DIR}/build/_deps/hrtf-assets")
set(_hs_sadie "${_hs_stage}/mid-sadie-d1.sofa")

file(MAKE_DIRECTORY "${_hs_stage}/custom")
if(NOT EXISTS "${_hs_sadie}")
  if(NOT EXISTS "${_hs_zip}")
    message(STATUS "Downloading SADIE II D1 HRIRs (38 MB, once)")
    file(MAKE_DIRECTORY "${_hs_downloads}")
    file(DOWNLOAD "${DSPTB_SADIE_D1_URL}" "${_hs_zip}"
      EXPECTED_HASH SHA256=${DSPTB_SADIE_D1_SHA256} STATUS _hs_status)
    list(GET _hs_status 0 _hs_code)
    if(NOT _hs_code EQUAL 0)
      file(REMOVE "${_hs_zip}")
      message(WARNING "Could not download SADIE II D1 from ${DSPTB_SADIE_D1_URL}: ${_hs_status}\n"
        "The Spatializer will fall back to Steam Audio's built-in HRTF for the Mid and Far ranges. To fix, "
        "download D1_HRIR_SOFA.zip by hand to ${_hs_zip} and configure again.")
    endif()
  endif()
  if(EXISTS "${_hs_zip}")
    file(SHA256 "${_hs_zip}" _hs_actual)
    string(TOUPPER "${_hs_actual}" _hs_actual)
    if(NOT _hs_actual STREQUAL DSPTB_SADIE_D1_SHA256)
      message(FATAL_ERROR "${_hs_zip} has SHA-256 ${_hs_actual}, expected ${DSPTB_SADIE_D1_SHA256}. Delete it.")
    endif()
    set(_hs_tmp "${CMAKE_SOURCE_DIR}/build/_deps/hrtf-sadie-d1")
    file(ARCHIVE_EXTRACT INPUT "${_hs_zip}" DESTINATION "${_hs_tmp}" PATTERNS "*48K_24bit_256tap*")
    file(GLOB_RECURSE _hs_found "${_hs_tmp}/*48K_24bit_256tap_FIR_SOFA.sofa")
    list(GET _hs_found 0 _hs_first)
    if(_hs_first)
      file(COPY_FILE "${_hs_first}" "${_hs_sadie}")
    endif()
  endif()
endif()

file(GLOB _hs_committed "${CMAKE_SOURCE_DIR}/modules/spatializer/hrtf/*.sofa")
foreach(_f IN LISTS _hs_committed)
  get_filename_component(_n "${_f}" NAME)
  file(COPY_FILE "${_f}" "${_hs_stage}/${_n}" ONLY_IF_DIFFERENT)
endforeach()
file(WRITE "${_hs_stage}/custom/README.txt"
  "Put your own HRTF sets here (.sofa, SimpleFreeFieldHRIR, one distance per file) and pick them in the 3D Spatializer's HRTF set list.\n")

set(DSPTB_HRTF_ASSET_DIR "${_hs_stage}")
