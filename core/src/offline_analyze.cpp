#include "dsptb/core/offline_analyze.hpp"

#include "dsptb/core/analysis_host.hpp"
#include "dsptb/core/module_instance.hpp"

#include <algorithm>

namespace dsptb {

bool analyzeWithModule(ModuleRegistry& registry, const AnalyzeModuleOptions& options,
                       const AudioBuffer& input, std::string& dumpJson, std::string& error) {
    if (input.sampleRate == 0) {
        error = "the input has no sample rate";
        return false;
    }
    ModulePackage* pkg = nullptr;
    const dsptb_module_desc* desc = registry.descriptor(options.moduleId, &pkg, error);
    if (!desc)
        return false;
    if (desc->kind != DSPTB_KIND_ANALYZER && desc->kind != DSPTB_KIND_OFFLINE) {
        error = options.moduleId + " is not an analyzer or offline module";
        return false;
    }
    SimpleModuleHost host;
    auto inst = ModuleInstance::create(1, *pkg, *desc, &host, error);
    if (!inst)
        return false;
    for (const auto& [id, value] : options.params) {
        const int idx = inst->params().indexOf(id);
        if (idx < 0) {
            error = options.moduleId + " has no parameter \"" + id + "\"";
            return false;
        }
        inst->params().set(static_cast<uint32_t>(idx), value);
    }
    if (!inst->prepare(static_cast<double>(input.sampleRate), AnalysisHost::kChunk, /*offline=*/true, error))
        return false;

    const size_t frames = input.frames();
    if (desc->kind == DSPTB_KIND_OFFLINE) {
        const float* channels[2] = {input.ch[0].data(), input.ch[1].data()};
        int result = 1;
        if (!inst->callRunOffline(channels, 2, frames, static_cast<double>(input.sampleRate), nullptr, nullptr,
                                  result) ||
            result != 0) {
            error = options.moduleId + " failed while running";
            return false;
        }
        if (!inst->dumpFrames(dumpJson)) {
            error = options.moduleId + " has no frame dump (it does not export dsptb_dump_frames)";
            return false;
        }
        return true;
    }
    const uint32_t block = std::clamp<uint32_t>(options.blockFrames, 32, AnalysisHost::kChunk);
    for (size_t pos = 0; pos < frames; pos += block) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, frames - pos));
        if (!inst->callAnalyze(input.ch[0].data() + pos, input.ch[1].data() + pos, n, pos, 0, options.tap)) {
            error = options.moduleId + " crashed while analysing";
            return false;
        }
    }
    if (!inst->dumpFrames(dumpJson)) {
        error = options.moduleId + " has no frame dump (it does not export dsptb_dump_frames)";
        return false;
    }
    return true;
}

} // namespace dsptb
