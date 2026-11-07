# Install rules behind tools/make-release.ps1. Each release zip is one install component:
#   app                the application folder
#   sdk                the module SDK
#   module_<id>        one first-party module (added by dsptb_add_module)
# Only BUNDLED modules are in the app component; SEPARATE modules have their own zip and release.
# Private and test modules get no install rule, so they can't reach a package. tools/check-package.ps1 proves it.

include_guard(GLOBAL)

# The VC++ runtime goes next to the exe, so the app runs on a PC without the redistributable installed.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)

install(TARGETS DSPToolbox dsptb-cli RUNTIME DESTINATION . COMPONENT app)
install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION . COMPONENT app)
install(FILES README.md LICENSE MODULE-EXCEPTION.md DESTINATION . COMPONENT app)
if(DSPTB_ENABLE_ASIO)
  install(FILES docs/images/asio-compatible.png DESTINATION assets COMPONENT app)
endif()

# --- SDK ---
find_path(DSPTB_NLOHMANN_INCLUDE "nlohmann/json.hpp" REQUIRED)
install(DIRECTORY sdk/include/dsptb DESTINATION include COMPONENT sdk)
install(DIRECTORY "${DSPTB_NLOHMANN_INCLUDE}/nlohmann" DESTINATION include COMPONENT sdk)
install(FILES sdk/cmake/dsptb_sdkConfig.cmake cmake/DsptbModule.cmake cmake/Warnings.cmake
  DESTINATION cmake COMPONENT sdk)
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_BINARY_DIR}/packaging/dsptb_sdkConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
install(FILES "${CMAKE_BINARY_DIR}/packaging/dsptb_sdkConfigVersion.cmake" DESTINATION cmake COMPONENT sdk)
install(TARGETS dsptb-cli RUNTIME DESTINATION bin COMPONENT sdk)
install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin COMPONENT sdk)
install(DIRECTORY tools/module-template DESTINATION . COMPONENT sdk)
install(FILES tools/new-module.ps1 DESTINATION tools COMPONENT sdk)
install(FILES docs/MODULE_SDK.md LICENSE MODULE-EXCEPTION.md DESTINATION . COMPONENT sdk)

# --- What must never be packaged (read by tools/check-package.ps1) ---
get_property(dsptb_private_ids GLOBAL PROPERTY DSPTB_UNPACKAGED_MODULE_IDS)
string(REPLACE ";" "\n" dsptb_private_text "${dsptb_private_ids}")
file(WRITE "${CMAKE_BINARY_DIR}/packaging/unpackaged-modules.txt" "${dsptb_private_text}\n")

# --- What is released on its own and must stay out of the app zip ---
get_property(dsptb_separate_ids GLOBAL PROPERTY DSPTB_SEPARATE_MODULE_IDS)
string(REPLACE ";" "\n" dsptb_separate_text "${dsptb_separate_ids}")
file(WRITE "${CMAKE_BINARY_DIR}/packaging/separate-modules.txt" "${dsptb_separate_text}\n")
