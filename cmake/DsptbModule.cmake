# dsptb_add_module(<target>
#   ID org.dsptb.gain
#   SOURCES src/gain.cpp ...
#   [MANIFEST module.json]          # default: ./module.json
#   [DISTRIBUTION BUNDLED|SEPARATE] # default SEPARATE. BUNDLED also goes in the app zip and follows the app version
#   [VERSION 0.1.0]                 # overrides the manifest version; BUNDLED defaults to the project version
#   [DEPENDENCY_DLLS <paths...>]    # copied next to the DLL (e.g. phonon.dll)
#   [ASSETS <dir>]                  # copied to <package>/assets
#   [LINK_LIBRARIES <targets...>]   # e.g. dsptb::dsp_common
#   [EXTRA_FILES <paths...>]        # copied into the package folder (licenses, notices)
#   [PRIVATE_MODULE]                # output goes to modules-private/, never packaged into releases
#   [TEST_MODULE])                  # test fixture: output goes to bin/test-modules/, no validate_ test
#
# Builds a module DLL exactly like a third-party author would (MODULE_SDK §7): links dsptb::sdk, strict
# warnings, a package folder holding module.json and the DLL, and a validate_<target> ctest entry that runs
# `dsptb-cli validate` on the package.

include_guard(GLOBAL)

function(dsptb_add_module target)
  cmake_parse_arguments(M "PRIVATE_MODULE;TEST_MODULE" "ID;MANIFEST;VERSION;DISTRIBUTION" "SOURCES;DEPENDENCY_DLLS;ASSETS;LINK_LIBRARIES;EXTRA_FILES" ${ARGN})
  if(NOT M_ID)
    message(FATAL_ERROR "dsptb_add_module(${target}): ID is required")
  endif()
  if(NOT M_MANIFEST)
    set(M_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/module.json")
  elseif(NOT IS_ABSOLUTE "${M_MANIFEST}")
    set(M_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${M_MANIFEST}")
  endif()
  if(NOT M_DISTRIBUTION)
    set(M_DISTRIBUTION SEPARATE)
  elseif(NOT M_DISTRIBUTION MATCHES "^(BUNDLED|SEPARATE)$")
    message(FATAL_ERROR "dsptb_add_module(${target}): DISTRIBUTION must be BUNDLED or SEPARATE")
  endif()
  if(NOT M_VERSION AND M_DISTRIBUTION STREQUAL "BUNDLED")
    set(M_VERSION "${PROJECT_VERSION}")
  endif()

  if(M_PRIVATE_MODULE)
    set(pkg_dir "${CMAKE_BINARY_DIR}/modules-private/${M_ID}")
  elseif(M_TEST_MODULE)
    set(pkg_dir "${CMAKE_BINARY_DIR}/bin/test-modules/${M_ID}")
  else()
    set(pkg_dir "${CMAKE_BINARY_DIR}/bin/modules/${M_ID}")
  endif()

  add_library(${target} MODULE ${M_SOURCES})
  target_link_libraries(${target} PRIVATE dsptb_sdk dsptb_warnings ${M_LINK_LIBRARIES})
  set_target_properties(${target} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${target}"
    RUNTIME_OUTPUT_DIRECTORY "${pkg_dir}"
    LIBRARY_OUTPUT_DIRECTORY "${pkg_dir}")

  # Manifest: a separate module keeps its own version, because it releases on its own schedule.
  file(READ "${M_MANIFEST}" manifest_text)
  if(M_VERSION)
    string(JSON manifest_text SET "${manifest_text}" version "\"${M_VERSION}\"")
  endif()
  file(MAKE_DIRECTORY "${pkg_dir}")
  file(WRITE "${pkg_dir}/module.json" "${manifest_text}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${M_MANIFEST}")

  foreach(dll IN LISTS M_DEPENDENCY_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${pkg_dir}")
  endforeach()
  foreach(extra IN LISTS M_EXTRA_FILES)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${extra}" "${pkg_dir}")
  endforeach()
  if(M_ASSETS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${M_ASSETS}" "${pkg_dir}/assets")
  endif()

  # Only first-party modules get install rules (cmake/Packaging.cmake); DSPTB_IN_TREE keeps a third-party build
  # against the SDK package free of them.
  if(M_PRIVATE_MODULE OR M_TEST_MODULE)
    set_property(GLOBAL APPEND PROPERTY DSPTB_UNPACKAGED_MODULE_IDS "${M_ID}")
  elseif(DSPTB_IN_TREE)
    set(component_dests "module_${M_ID}|${M_ID}")
    if(M_DISTRIBUTION STREQUAL "BUNDLED")
      list(APPEND component_dests "app|modules/${M_ID}")
    else()
      set_property(GLOBAL APPEND PROPERTY DSPTB_SEPARATE_MODULE_IDS "${M_ID}")
    endif()
    foreach(component_dest IN LISTS component_dests)
      string(REPLACE "|" ";" parts "${component_dest}")
      list(GET parts 0 component)
      list(GET parts 1 destination)
      install(DIRECTORY "${pkg_dir}/" DESTINATION "${destination}" COMPONENT "${component}"
        PATTERN "*.pdb" EXCLUDE PATTERN "*.lib" EXCLUDE PATTERN "*.exp" EXCLUDE PATTERN "*.ilk" EXCLUDE
        PATTERN "custom" EXCLUDE)
    endforeach()
    # A module zip travels alone, so it carries the project licence too.
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "${M_ID}" COMPONENT "module_${M_ID}")
  endif()

  if(NOT M_TEST_MODULE AND TARGET dsptb-cli)
    add_test(NAME validate_${target} COMMAND dsptb-cli validate "${pkg_dir}")
  endif()
endfunction()
