dsptb_add_module(analysis
  ID org.dsptb.analysis
  DISTRIBUTION SEPARATE
  SOURCES src/analysis.cpp src/csv_export.cpp
  LINK_LIBRARIES dsptb::dsp_common dsptb_branding shell32 ole32)
