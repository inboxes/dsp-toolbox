#include "dsptb/core/chain_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsptb {

namespace {

// A NaN or infinity anywhere makes the sum NaN; one multiply-add per sample, so it vectorizes.
bool allFinite(const float* x, uint32_t n) noexcept {
    float acc = 0.0f;
    for (uint32_t i = 0; i < n; ++i)
        acc += x[i] * 0.0f;
    return std::isfinite(acc);
}

// Pushes `in` through a delay line of its size and writes the delayed signal to `out` (in == out is fine).
void delayBlock(std::vector<float>& line, uint32_t pos, const float* in, float* out, uint32_t n) noexcept {
    const uint32_t len = static_cast<uint32_t>(line.size());
    float* d = line.data();
    for (uint32_t i = 0; i < n; ++i) {
        const float x = in[i];
        out[i] = d[pos];
        d[pos] = x;
        if (++pos == len)
            pos = 0;
    }
}

// Delayed copy of the block into rt.dry*, keeping the delay line fed.
void captureDry(ModuleInstance& m, const float* l, const float* r, uint32_t n) noexcept {
    auto& rt = m.rt;
    if (m.latencyFrames() == 0) {
        std::memcpy(rt.dryL.data(), l, sizeof(float) * n);
        std::memcpy(rt.dryR.data(), r, sizeof(float) * n);
        return;
    }
    delayBlock(rt.delayL, rt.delayPos, l, rt.dryL.data(), n);
    delayBlock(rt.delayR, rt.delayPos, r, rt.dryR.data(), n);
    rt.delayPos = (rt.delayPos + n) % static_cast<uint32_t>(rt.delayL.size());
}

void processSlot(const ProcessorSlot& slot, float* l, float* r, uint32_t n, const dsptb_transport& transport,
                 bool ramps, ChainFaults& faults) noexcept {
    ModuleInstance& m = *slot.instance;
    auto& rt = m.rt;
    if (m.kind() != DSPTB_KIND_PROCESSOR || !m.prepared() || n > m.maxBlockFrames())
        return;
    m.params().copyToRt();

    const bool healthy = !m.faulted();
    const float target = (slot.enabled && healthy) ? 1.0f : 0.0f;
    if (!ramps)
        rt.wet = target;

    // Fully bypassed: no process() call; the dry signal still passes through the latency delay.
    if (rt.wet == 0.0f && target == 0.0f) {
        if (m.latencyFrames() > 0) {
            delayBlock(rt.delayL, rt.delayPos, l, l, n);
            delayBlock(rt.delayR, rt.delayPos, r, r, n);
            rt.delayPos = (rt.delayPos + n) % static_cast<uint32_t>(rt.delayL.size());
        }
        return;
    }

    captureDry(m, l, r, n); // also what we fall back to if the module misbehaves

    if (rt.wet == 0.0f) { // coming out of bypass: its internal state is stale
        if (!m.callReset()) {
            m.markFaulted(FaultReason::Exception);
            faults.add(m.id(), FaultReason::Exception);
            rt.wet = 0.0f;
            std::memcpy(l, rt.dryL.data(), sizeof(float) * n);
            std::memcpy(r, rt.dryR.data(), sizeof(float) * n);
            return;
        }
    }

    const bool ran = m.callProcess(l, r, n, transport);
    FaultReason fault = FaultReason::None;
    if (!ran) {
        fault = FaultReason::Exception;
    } else if (!allFinite(l, n) || !allFinite(r, n)) {
        // Scrub this block (use the dry signal); fault the module if it keeps doing it.
        if (++rt.nonFiniteRun >= 3)
            fault = FaultReason::NonFinite;
        std::memcpy(l, rt.dryL.data(), sizeof(float) * n);
        std::memcpy(r, rt.dryR.data(), sizeof(float) * n);
    } else {
        rt.nonFiniteRun = 0;
    }
    if (fault != FaultReason::None) {
        m.markFaulted(fault);
        faults.add(m.id(), fault);
        rt.wet = 0.0f;
        std::memcpy(l, rt.dryL.data(), sizeof(float) * n);
        std::memcpy(r, rt.dryR.data(), sizeof(float) * n);
        return;
    }

    if (rt.wet == target && target == 1.0f)
        return; // steady and active: the wet signal is the output, already in place

    // Crossfade: out = dry_delayed * (1 - g) + wet * g.
    float g = rt.wet;
    const float step = rt.rampStep;
    const float* dl = rt.dryL.data();
    const float* dr = rt.dryR.data();
    for (uint32_t i = 0; i < n; ++i) {
        if (g < target)
            g = std::min(target, g + step);
        else if (g > target)
            g = std::max(target, g - step);
        l[i] = dl[i] * (1.0f - g) + l[i] * g;
        r[i] = dr[i] * (1.0f - g) + r[i] * g;
    }
    rt.wet = g;
}

} // namespace

void processChain(ChainSnapshot& chain, float* l, float* r, uint32_t n, const dsptb_transport& transport,
                  bool ramps, ChainFaults& faults) noexcept {
    for (const ProcessorSlot& slot : chain.slots)
        if (slot.instance)
            processSlot(slot, l, r, n, transport, ramps, faults);
}

void resetChain(ChainSnapshot& chain, ChainFaults& faults) noexcept {
    for (const ProcessorSlot& slot : chain.slots) {
        if (!slot.instance || slot.instance->faulted() || slot.instance->kind() != DSPTB_KIND_PROCESSOR)
            continue;
        if (!slot.instance->callReset()) {
            slot.instance->markFaulted(FaultReason::Exception);
            faults.add(slot.instance->id(), FaultReason::Exception);
        }
    }
}

} // namespace dsptb
