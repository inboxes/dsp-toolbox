#include "dsptb/core/offline_render.hpp"

#include "dsptb/core/chain_processor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

namespace dsptb {

SessionDescription descriptionFromSession(const SessionDoc& doc) {
    SessionDescription out;
    out.sampleRate = doc.audio.sampleRate;
    for (const SessionInstanceDoc& i : doc.instances) {
        if (i.role == "analyzer" || i.role == "offline")
            continue; // these don't change the audio
        InstanceDescription d;
        d.module = i.module;
        d.enabled = i.enabled;
        for (const auto& [id, v] : i.params)
            d.params[id] = static_cast<float>(v);
        d.stateJson = i.stateJson;
        out.instances.push_back(std::move(d));
    }
    return out;
}

bool parseSessionDescription(const std::string& text, SessionDescription& out, std::string& error) {
    out = SessionDescription{};
    SessionDoc doc;
    if (!sessionFromJson(text, doc, error))
        return false;
    out = descriptionFromSession(doc);
    return true;
}

bool renderOffline(ModuleRegistry& registry, const SessionDescription& session, const AudioBuffer& input,
                   const RenderOptions& options, AudioBuffer& output, std::string& error) {
    if (input.sampleRate == 0) {
        error = "the input has no sample rate";
        return false;
    }
    const double sr = static_cast<double>(input.sampleRate);
    const uint32_t block = std::max<uint32_t>(32, options.blockFrames);

    SimpleModuleHost host;
    ChainSnapshot chain;
    InstanceId nextId = 1;
    uint32_t latency = 0;
    for (const InstanceDescription& d : session.instances) {
        ModulePackage* pkg = nullptr;
        const dsptb_module_desc* desc = registry.descriptor(d.module, &pkg, error);
        if (!desc)
            return false;
        if (desc->kind == DSPTB_KIND_ANALYZER || desc->kind == DSPTB_KIND_OFFLINE)
            continue; // these don't change the audio
        if (desc->kind != DSPTB_KIND_PROCESSOR) {
            error = d.module + " is not a processor";
            return false;
        }
        auto inst = ModuleInstance::create(nextId++, *pkg, *desc, &host, error);
        if (!inst)
            return false;
        if (!d.stateJson.empty() && !inst->loadState(d.stateJson)) {
            error = "could not load the saved state of " + d.module;
            return false;
        }
        for (const auto& [id, value] : d.params) {
            const int idx = inst->params().indexOf(id);
            if (idx < 0) {
                error = d.module + " has no parameter \"" + id + "\"";
                return false;
            }
            inst->params().set(static_cast<uint32_t>(idx), value);
        }
        if (!inst->prepare(sr, block, /*offline=*/true, error)) {
            error = d.module + ": " + error;
            return false;
        }
        latency += inst->latencyFrames(); // a bypassed slot still delays its dry signal (chain_processor.hpp)
        chain.slots.push_back({std::move(inst), d.enabled});
    }

    const size_t inFrames = input.frames();
    const size_t tailFrames = static_cast<size_t>(std::max(0.0, options.tailSeconds) * sr);
    const size_t outFrames = inFrames + tailFrames;
    const size_t total = outFrames + latency; // run past the end so the latency can be trimmed

    std::vector<float> l(total, 0.0f), r(total, 0.0f);
    std::copy(input.ch[0].begin(), input.ch[0].end(), l.begin());
    std::copy(input.ch[1].begin(), input.ch[1].end(), r.begin());

    for (size_t pos = 0; pos < total; pos += block) {
        if (options.cancel && options.cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        if (options.progress)
            options.progress->store(static_cast<float>(pos) / static_cast<float>(total),
                                    std::memory_order_relaxed);
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, total - pos));
        dsptb_transport t{};
        t.struct_size = sizeof(t);
        t.source = DSPTB_SOURCE_OFFLINE;
        t.playing = 1;
        t.position_seconds = static_cast<double>(pos) / sr;
        t.sample_clock = pos;
        ChainFaults faults;
        processChain(chain, l.data() + pos, r.data() + pos, n, t, /*ramps=*/false, faults);
        if (faults.count > 0) {
            error = "module instance " + std::to_string(faults.ids[0]) + " faulted during the render";
            return false;
        }
    }

    if (options.progress)
        options.progress->store(1.0f, std::memory_order_relaxed);
    output.sampleRate = input.sampleRate;
    output.ch[0].assign(l.begin() + static_cast<std::ptrdiff_t>(latency), l.end());
    output.ch[1].assign(r.begin() + static_cast<std::ptrdiff_t>(latency), r.end());
    return true;
}

} // namespace dsptb
