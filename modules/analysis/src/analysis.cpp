// The Audio Analysis package: one DLL with five modules.

#include "file_analysis_module.hpp"
#include "mfcc_module.hpp"
#include "onset_module.hpp"
#include "pitch_module.hpp"
#include "spectral_module.hpp"

using dsptb::analysis::FileAnalysisModule;
using dsptb::analysis::MfccModule;
using dsptb::analysis::OnsetModule;
using dsptb::analysis::PitchModule;
using dsptb::analysis::SpectralModule;

DSPTB_REGISTER_MODULES(PitchModule, OnsetModule, MfccModule, SpectralModule, FileAnalysisModule);
