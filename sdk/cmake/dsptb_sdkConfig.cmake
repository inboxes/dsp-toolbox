# find_package(dsptb_sdk CONFIG) for modules built outside the source tree (MODULE_SDK section 6).
# Provides dsptb::sdk and dsptb_add_module(), and a validate_<target> test through the bundled dsptb-cli.

include_guard(GLOBAL)
get_filename_component(_dsptb_sdk_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(dsptb_sdk INTERFACE)
add_library(dsptb::sdk ALIAS dsptb_sdk)
target_include_directories(dsptb_sdk INTERFACE "${_dsptb_sdk_root}/include")
target_compile_features(dsptb_sdk INTERFACE cxx_std_20)

add_executable(dsptb-cli IMPORTED GLOBAL)
set_target_properties(dsptb-cli PROPERTIES IMPORTED_LOCATION "${_dsptb_sdk_root}/bin/dsptb-cli.exe")

include("${CMAKE_CURRENT_LIST_DIR}/Warnings.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DsptbModule.cmake")
