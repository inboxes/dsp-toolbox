# Pure logic, so the unit tests can link it without the SDK or a host.
add_library(dsptb_autotune_core STATIC
  core/pitch_tracker.cpp core/target_picker.cpp core/correction_smoother.cpp)
target_include_directories(dsptb_autotune_core PUBLIC core)
target_compile_features(dsptb_autotune_core PUBLIC cxx_std_20)
target_link_libraries(dsptb_autotune_core PRIVATE dsptb_warnings)

dsptb_add_module(autotune
  ID org.dsptb.autotune
  DISTRIBUTION SEPARATE
  SOURCES src/autotune.cpp src/pitch_graph.cpp
  LINK_LIBRARIES dsptb_autotune_core dsptb::dsp_common)
