#pragma once

#include "audio_buffer.hpp"
#include "module_registry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace dsptb {

struct AnalyzeModuleOptions {
    std::string moduleId;
    std::vector<std::pair<std::string, float>> params; // by parameter id; unknown ids are an error
    uint32_t blockFrames = 1024;                       // size of the chunks handed to analyze()
    dsptb_tap tap = DSPTB_TAP_POST;
};

// Runs an analyzer module over a whole buffer on the calling thread, with the same calls the analysis thread
// makes (`callAnalyze` in chunks, sample clock counting up), then asks the module for its frame dump. Backs
// `dsptb-cli analyze --module` and the module acceptance tests. The module is prepared with offline = true.
// Returns false with `error` when the module is missing, not an analyzer, faulted, or has no dump.
bool analyzeWithModule(ModuleRegistry& registry, const AnalyzeModuleOptions& options,
                       const AudioBuffer& input, std::string& dumpJson, std::string& error);

} // namespace dsptb
