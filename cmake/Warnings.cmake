# Strict warnings for OUR targets only (never third-party). /WX on warnings.
add_library(dsptb_warnings INTERFACE)
if(MSVC)
  target_compile_options(dsptb_warnings INTERFACE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(dsptb_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
