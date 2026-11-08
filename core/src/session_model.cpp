#include "dsptb/core/session_model.hpp"

#include "dsptb/core/paths.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace dsptb {

namespace {
constexpr double kRemoveRampMs = 20.0; // the bypass crossfade the audio side runs on removal
constexpr double kDipMs = 10.0;        // reorder fade-out
} // namespace

SessionModel::SessionModel(Engine& engine, ModuleRegistry& registry, bool analysisThread)
    : engine_(engine), registry_(registry), analysis_(analysisThread) {
    engine_.setOnOpened([this] { prepareAll(); });
}

SessionModel::~SessionModel() {
    engine_.setOnOpened({});
    // Snapshots keep the instances alive until the audio thread lets go, so only the host back-pointer needs
    // clearing; then the chain is emptied.
    for (auto& r : records_)
        if (r.instance)
            r.instance->setHost(nullptr);
    engine_.publishChain(std::make_unique<ChainSnapshot>());
}

InstanceRecord* SessionModel::find(InstanceId id) {
    for (auto& r : records_)
        if (r.id == id)
            return &r;
    return nullptr;
}

double SessionModel::transitionSeconds(double rampMs) const {
    const double sr = std::max(1u, engine_.sampleRate());
    const double buffer = static_cast<double>(engine_.bufferFrames()) / sr;
    // The ramp itself, plus one and a half callback periods for the audio thread to pick the snapshot up,
    // plus margin. Kept tight on purpose: during a reorder this is how long the output stays dipped.
    return rampMs * 0.001 + 1.5 * buffer + 0.002;
}

size_t SessionModel::pendingRemovals() const {
    return static_cast<size_t>(
        std::count_if(records_.begin(), records_.end(), [](const InstanceRecord& r) { return r.removing; }));
}

uint32_t SessionModel::totalLatencyFrames() const {
    uint32_t total = 0;
    for (InstanceId id : audioOrder_)
        for (const auto& r : records_)
            if (r.id == id && r.instance)
                total += r.instance->latencyFrames();
    return total;
}

void SessionModel::publish() {
    auto snap = std::make_unique<ChainSnapshot>();
    for (InstanceId id : audioOrder_) {
        const InstanceRecord* rec = nullptr;
        for (const auto& r : records_)
            if (r.id == id) {
                rec = &r;
                break;
            }
        if (!rec || !rec->instance)
            continue;
        ProcessorSlot slot;
        slot.instance = rec->instance;
        slot.enabled = rec->enabled && !rec->removing;
        snap->latencyFrames += rec->instance->latencyFrames();
        snap->slots.push_back(std::move(slot));
    }
    // Analyzers listen through their tap ring. A disabled or faulted analyzer is left out, so the audio
    // thread stops feeding it ("pause analyzers whose instance is disabled").
    for (const auto& r : records_) {
        if (!r.isAnalyzer() || !r.ring || !r.enabled || r.removing || r.instance->faulted())
            continue;
        (r.tap == DSPTB_TAP_PRE ? snap->preTaps : snap->postTaps).push_back(r.ring.get());
        snap->tapOwners.push_back(r.ring);
    }
    if (recordRing_) {
        (recordTap_ == DSPTB_TAP_PRE ? snap->preTaps : snap->postTaps).push_back(recordRing_.get());
        snap->tapOwners.push_back(recordRing_);
    }
    snap->outputMuted = reorderPending_;
    engine_.publishChain(std::move(snap));
}

void SessionModel::setRecordTap(std::shared_ptr<TapRing> ring, dsptb_tap tap) {
    recordRing_ = std::move(ring);
    recordTap_ = tap;
    publish(); // the old ring stays alive in the snapshots that still hold it
}

// ----------------------------------------------------------------------------------------------------------
// persistence
// ----------------------------------------------------------------------------------------------------------

bool SessionModel::removePlaceholder(size_t index) {
    if (index >= placeholders_.size())
        return false;
    placeholders_.erase(placeholders_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::vector<SessionInstanceDoc> SessionModel::capture() const {
    std::vector<SessionInstanceDoc> docs;
    for (const auto& r : records_) {
        if (r.removing || !r.instance)
            continue;
        const ModuleInstance& inst = *r.instance;
        SessionInstanceDoc d;
        d.module = inst.moduleId();
        d.moduleVersion = inst.package().manifest.version;
        d.enabled = r.enabled;
        d.role = r.isAnalyzer() ? "analyzer" : r.isOffline() ? "offline" : "processor";
        if (r.isAnalyzer())
            d.tap = r.tap == DSPTB_TAP_PRE ? "pre" : "post";
        d.label = r.label;
        d.panelOpen = r.panelOpen;
        const ParamBlock& pb = inst.params();
        for (uint32_t i = 0; i < pb.size(); ++i)
            d.params[pb.desc(i).id] = static_cast<double>(pb.get(i));
        inst.saveState(d.stateJson);
        docs.push_back(std::move(d));
    }
    // Placeholders go back where they were; ascending slot order keeps the positions right.
    std::vector<const Placeholder*> sorted;
    for (const auto& p : placeholders_)
        sorted.push_back(&p);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Placeholder* a, const Placeholder* b) { return a->slot < b->slot; });
    for (const Placeholder* p : sorted)
        docs.insert(docs.begin() + static_cast<std::ptrdiff_t>(std::min(p->slot, docs.size())), p->doc);
    for (size_t i = 0; i < docs.size(); ++i)
        docs[i].instanceId = "i" + std::to_string(i + 1);
    return docs;
}

void SessionModel::clear() {
    placeholders_.clear();
    gesture_ = OpenGesture{};
    undo_.clear();
    redo_.clear();
    std::vector<InstanceId> ids;
    for (const auto& r : records_)
        if (!r.removing)
            ids.push_back(r.id);
    for (InstanceId id : ids)
        remove(id);
}

void SessionModel::restore(const std::vector<SessionInstanceDoc>& docs,
                           std::vector<SessionMessage>& messages) {
    clear();
    for (size_t slot = 0; slot < docs.size(); ++slot) {
        const SessionInstanceDoc& d = docs[slot];
        std::string error;
        const InstanceId id = add(d.module, error);
        if (id == 0) {
            const std::string why = error.empty() ? "the module is not available" : error;
            messages.push_back({NoticeLevel::Warning, "Module " + d.module + " is missing (" + why +
                                                          "); it is kept as a placeholder."});
            spdlog::warn("session: module {} unavailable: {}", d.module, why);
            placeholders_.push_back({d, slot, why});
            continue;
        }
        InstanceRecord* rec = find(id);
        ModuleInstance& inst = *rec->instance;
        if (!d.stateJson.empty() && d.stateJson != "{}" && !inst.loadState(d.stateJson)) {
            messages.push_back({NoticeLevel::Warning, "Could not restore the saved state of " +
                                                          std::string(inst.desc().name) +
                                                          "; defaults used."});
            spdlog::warn("session: state of {} was rejected", d.module);
        }
        for (const auto& [pid, value] : d.params) {
            const int idx = inst.params().indexOf(pid);
            if (idx < 0) {
                spdlog::info("session: {} has no parameter \"{}\"; ignored", d.module, pid);
                continue;
            }
            inst.params().set(static_cast<uint32_t>(idx), static_cast<float>(value));
        }
        setEnabled(id, d.enabled);
        rec->panelOpen = d.panelOpen;
        setLabel(id, d.label);
        if (rec->isAnalyzer() && (d.tap == "pre" || d.tap == "post"))
            setTap(id, d.tap == "pre" ? DSPTB_TAP_PRE : DSPTB_TAP_POST);
        inst.saveState(rec->lastState);
    }
}

void SessionModel::prepareAll() {
    const uint32_t sr = engine_.sampleRate();
    if (sr == 0)
        return;
    for (auto& r : records_) {
        // An offline module may be mid-run, and each run prepares it for the file anyway.
        if (!r.instance || r.instance->faulted() || r.isOffline())
            continue;
        std::string err;
        if (r.isAnalyzer()) {
            // The module may not be mid-analyze() while it is re-prepared, so take it off the analysis thread
            // first. It gets a new ring sized for the new rate.
            analysis_.detach(r.id);
            r.ring.reset();
        }
        const uint32_t block = r.isAnalyzer() ? AnalysisHost::kChunk : engine_.maxBlockFrames();
        if (!r.instance->prepare(static_cast<double>(sr), block, false, err)) {
            spdlog::warn("module {}: prepare failed: {}", r.instance->moduleId(), err);
            r.instance->markFaulted(FaultReason::Create);
            continue;
        }
        if (r.isAnalyzer())
            r.ring = analysis_.attach(r.instance, r.tap, sr);
    }
    publish();
}

InstanceId SessionModel::add(const std::string& moduleId, std::string& error) {
    ModulePackage* pkg = nullptr;
    const dsptb_module_desc* desc = registry_.descriptor(moduleId, &pkg, error);
    if (!desc)
        return 0;
    const bool analyzer = desc->kind == DSPTB_KIND_ANALYZER;
    auto inst = ModuleInstance::create(nextId_, *pkg, *desc, this, error);
    if (!inst) {
        registry_.markFaulted(moduleId, error);
        return 0;
    }
    if (engine_.sampleRate() > 0) {
        const uint32_t block = analyzer                          ? AnalysisHost::kChunk
                               : desc->kind == DSPTB_KIND_OFFLINE ? kOfflineMaxBlock
                                                                  : engine_.maxBlockFrames();
        if (!inst->prepare(static_cast<double>(engine_.sampleRate()), block, false, error)) {
            if (inst->faulted())
                registry_.markFaulted(moduleId, error);
            return 0;
        }
    }
    InstanceRecord rec;
    rec.id = nextId_++;
    rec.instance = std::move(inst);
    rec.instance->saveState(rec.lastState);
    if (analyzer) {
        rec.tap = desc->default_tap;
        // Before the engine has opened there is no rate yet: use a placeholder ring, replaced by
        // prepareAll().
        rec.ring =
            analysis_.attach(rec.instance, rec.tap, engine_.sampleRate() ? engine_.sampleRate() : 48000);
    }
    records_.push_back(std::move(rec));
    if (desc->kind == DSPTB_KIND_PROCESSOR)
        audioOrder_.push_back(records_.back().id);
    publish();
    return records_.back().id;
}

bool SessionModel::remove(InstanceId id) {
    InstanceRecord* rec = find(id);
    if (!rec || rec->removing)
        return false;
    if (gesture_.open && gesture_.id == id)
        endGesture();
    if (!rec->isProcessor()) {
        // No audio goes through an analyzer or offline module, so there is nothing to ramp. A ring stays alive
        // in the snapshots that still mention it until the audio thread has retired them.
        if (rec->isAnalyzer())
            analysis_.detach(id);
        records_.erase(records_.begin() + (rec - records_.data()));
        publish();
        return true;
    }
    if (!engineLive()) {
        audioOrder_.erase(std::remove(audioOrder_.begin(), audioOrder_.end(), id), audioOrder_.end());
        records_.erase(records_.begin() + (rec - records_.data()));
        publish();
        return true;
    }
    rec->removing = true;
    rec->removeAt = now_ + transitionSeconds(kRemoveRampMs);
    publish(); // the slot now ramps to bypass
    return true;
}

bool SessionModel::move(InstanceId id, size_t newIndex) {
    const auto it = std::find_if(records_.begin(), records_.end(), [&](const InstanceRecord& r) {
        return r.id == id && !r.removing && r.isProcessor();
    });
    if (it == records_.end())
        return false;
    // `newIndex` counts processors only; analyzers sit in records_ too but are not part of the chain order.
    const auto procIndex = [&](const InstanceRecord* upTo) {
        size_t k = 0;
        for (const auto& r : records_) {
            if (&r == upTo)
                break;
            if (r.isProcessor())
                ++k;
        }
        return k;
    };
    size_t procCount = 0;
    for (const auto& r : records_)
        procCount += r.isProcessor() ? 1 : 0;
    newIndex = std::min(newIndex, procCount - 1);
    if (procIndex(&*it) == newIndex)
        return true;
    InstanceRecord rec = std::move(*it);
    records_.erase(it);
    // Insert in front of the processor that currently holds position `newIndex` (or at the end).
    auto pos = records_.end();
    size_t k = 0;
    for (auto p = records_.begin(); p != records_.end(); ++p) {
        if (!p->isProcessor())
            continue;
        if (k++ == newIndex) {
            pos = p;
            break;
        }
    }
    records_.insert(pos, std::move(rec));

    const auto rebuildOrder = [&] {
        audioOrder_.clear();
        for (const auto& r : records_)
            if (r.isProcessor())
                audioOrder_.push_back(r.id);
    };
    if (!engineLive()) {
        rebuildOrder();
        publish();
        return true;
    }
    if (!reorderPending_) {
        reorderPending_ = true;
        publish(); // same order, outputMuted: the chain output fades out over 10 ms
        reorderAt_ = now_ + transitionSeconds(kDipMs);
    }
    return true;
}

bool SessionModel::setEnabled(InstanceId id, bool enabled) {
    InstanceRecord* rec = find(id);
    if (!rec || rec->removing)
        return false;
    if (rec->enabled == enabled)
        return true;
    rec->enabled = enabled;
    if (rec->isAnalyzer() && enabled)
        analysis_.markGap(id); // the signal was not delivered while it was off
    publish();
    return true;
}

bool SessionModel::setTap(InstanceId id, dsptb_tap tap) {
    InstanceRecord* rec = find(id);
    if (!rec || !rec->isAnalyzer() || rec->removing)
        return false;
    if (rec->tap == tap)
        return true;
    rec->tap = tap;
    analysis_.setTap(id, tap);
    analysis_.markGap(id); // PRE and POST are different signals
    publish();
    return true;
}

bool SessionModel::setLabel(InstanceId id, std::string label) {
    InstanceRecord* rec = find(id);
    if (!rec)
        return false;
    if (label.size() > 48)
        label.resize(48);
    rec->label = std::move(label);
    return true;
}

void SessionModel::tick(double now, std::vector<SessionMessage>& messages) {
    now_ = now;
    bool changed = false;

    if (reorderPending_ && now >= reorderAt_) {
        audioOrder_.clear();
        for (const auto& r : records_)
            if (r.isProcessor())
                audioOrder_.push_back(r.id);
        reorderPending_ = false;
        changed = true; // new order, outputMuted cleared: fades back in
    }

    for (size_t i = 0; i < records_.size();) {
        if (records_[i].removing && now >= records_[i].removeAt) {
            const InstanceId id = records_[i].id;
            audioOrder_.erase(std::remove(audioOrder_.begin(), audioOrder_.end(), id), audioOrder_.end());
            records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
        } else {
            ++i;
        }
    }

    for (auto& r : records_) {
        if (r.instance && r.instance->faulted() && !r.faultReported) {
            r.faultReported = true;
            const char* why = r.instance->faultReason() == FaultReason::NonFinite
                                  ? "produced invalid samples (NaN/Inf) for several blocks"
                              : r.instance->faultReason() == FaultReason::Create ? "could not be prepared"
                                                                                 : "crashed";
            const std::string name = r.instance->desc().name;
            registry_.markFaulted(r.instance->moduleId(), std::string(why));
            spdlog::error("module {} (instance {}) faulted: {}", r.instance->moduleId(), r.id, why);
            messages.push_back({NoticeLevel::Error, name + " " + why + " and was bypassed."});
            changed = true;
        }
    }

    if (changed)
        publish();
    engine_.collectGarbage();
}

// ----------------------------------------------------------------------------------------------------------
// parameters and undo
// ----------------------------------------------------------------------------------------------------------

void SessionModel::setParam(InstanceId id, uint32_t index, float value) {
    if (InstanceRecord* rec = find(id))
        if (rec->instance && index < rec->instance->params().size())
            rec->instance->params().set(index, value);
}

void SessionModel::pushUndo(UndoStep step) {
    redo_.clear();
    undo_.push_back(std::move(step));
    while (undo_.size() > kMaxUndo)
        undo_.pop_front();
}

void SessionModel::commitParam(InstanceId id, uint32_t index, float oldValue, float newValue) {
    InstanceRecord* rec = find(id);
    if (!rec || !rec->instance || index >= rec->instance->params().size() || oldValue == newValue)
        return;
    UndoStep s;
    s.id = id;
    s.index = index;
    s.oldValue = oldValue;
    s.newValue = newValue;
    s.label = std::string("Change ") + rec->instance->params().desc(index).name;
    pushUndo(std::move(s));
}

void SessionModel::setParamUndoable(InstanceId id, uint32_t index, float value) {
    InstanceRecord* rec = find(id);
    if (!rec || !rec->instance || index >= rec->instance->params().size())
        return;
    const float before = rec->instance->params().get(index);
    const float after = rec->instance->params().set(index, value);
    commitParam(id, index, before, after);
}

void SessionModel::applyStep(const UndoStep& s, bool forward) {
    if (!s.group.empty()) {
        if (forward)
            for (const UndoStep& part : s.group)
                applyStep(part, true);
        else
            for (auto it = s.group.rbegin(); it != s.group.rend(); ++it)
                applyStep(*it, false);
        return;
    }
    InstanceRecord* rec = find(s.id);
    if (!rec || !rec->instance)
        return;
    if (s.isState) {
        const std::string& text = forward ? s.newState : s.oldState;
        rec->instance->loadState(text);
        rec->lastState = text;
    } else {
        rec->instance->params().set(s.index, forward ? s.newValue : s.oldValue);
    }
}

bool SessionModel::undo() {
    endGesture();
    while (!undo_.empty()) {
        UndoStep s = std::move(undo_.back());
        undo_.pop_back();
        if (!find(s.id))
            continue; // its instance is gone
        applyStep(s, false);
        redo_.push_back(std::move(s));
        return true;
    }
    return false;
}

bool SessionModel::redo() {
    endGesture();
    while (!redo_.empty()) {
        UndoStep s = std::move(redo_.back());
        redo_.pop_back();
        if (!find(s.id))
            continue;
        applyStep(s, true);
        undo_.push_back(std::move(s));
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------------------------------------
// IModuleHost
// ----------------------------------------------------------------------------------------------------------

void SessionModel::moduleLog(ModuleInstance& who, dsptb_log_level level, const char* utf8) {
    SimpleModuleHost().moduleLog(who, level, utf8);
}

void SessionModel::moduleSetParam(ModuleInstance& who, uint32_t index, float value) {
    if (!(gesture_.open && gesture_.id == who.id())) {
        setParamUndoable(who.id(), index, value);
        return;
    }
    if (index >= who.params().size())
        return;
    const float before = who.params().get(index);
    const float after = who.params().set(index, value);
    // Merge into the gesture: keep the value from before it started, update the latest.
    for (UndoStep& s : gesture_.steps)
        if (!s.isState && s.index == index) {
            s.newValue = after;
            return;
        }
    UndoStep s;
    s.id = who.id();
    s.index = index;
    s.oldValue = before;
    s.newValue = after;
    gesture_.steps.push_back(std::move(s));
}

void SessionModel::moduleMarkStateDirty(ModuleInstance& who, const char* label) {
    InstanceRecord* rec = find(who.id());
    if (!rec)
        return;
    std::string now;
    if (!who.saveState(now) || now == rec->lastState)
        return;
    UndoStep s;
    s.isState = true;
    s.id = who.id();
    s.oldState = rec->lastState;
    s.newState = now;
    s.label = (label && *label) ? label : "Change state";
    rec->lastState = now;
    if (gesture_.open && gesture_.id == who.id()) {
        for (UndoStep& g : gesture_.steps)
            if (g.isState) {
                g.newState = s.newState;
                return;
            }
        gesture_.steps.push_back(std::move(s));
        return;
    }
    pushUndo(std::move(s));
}

std::filesystem::path SessionModel::moduleDataDir(const std::string& moduleId) {
    return dataDir() / "module-data" / std::filesystem::path(moduleId);
}

void SessionModel::moduleBeginGesture(ModuleInstance& who, const char* label) {
    endGesture(); // no nesting: a second begin ends the first
    gesture_.open = true;
    gesture_.id = who.id();
    gesture_.label = (label && *label) ? label : "Change";
}

void SessionModel::moduleEndGesture(ModuleInstance& who) {
    if (gesture_.open && gesture_.id == who.id())
        endGesture();
}

void SessionModel::endGesture() {
    if (!gesture_.open)
        return;
    OpenGesture g = std::move(gesture_);
    gesture_ = OpenGesture{};
    // A param set back to where it started is no change.
    std::erase_if(g.steps, [](const UndoStep& s) {
        return s.isState ? s.oldState == s.newState : s.oldValue == s.newValue;
    });
    if (g.steps.empty())
        return;
    if (g.steps.size() == 1) {
        g.steps.front().label = g.label;
        pushUndo(std::move(g.steps.front()));
        return;
    }
    UndoStep step;
    step.id = g.id;
    step.label = g.label;
    step.group = std::move(g.steps);
    pushUndo(std::move(step));
}

} // namespace dsptb
