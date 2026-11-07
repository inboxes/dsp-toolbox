#pragma once

#include "chain.hpp"

namespace dsptb {

// Instances that faulted during one chain run (at most 4 reported per call; the rest show up next block).
struct ChainFaults {
    InstanceId ids[4] = {};
    FaultReason reasons[4] = {};
    uint32_t count = 0;
    void add(InstanceId id, FaultReason r) noexcept {
        if (count < 4) {
            ids[count] = id;
            reasons[count] = r;
            ++count;
        }
    }
};

// Runs the processor slots over one planar stereo block, in place. The engine and the
// offline renderer share this code, so a render matches what is heard.
//   * `ramps`: true on the audio thread (20 ms bypass crossfade, dry path delayed by the slot latency so it
//     lines up); false offline, where a slot is simply on or off.
//   * A slot that is fully bypassed does not call process(), but still delays the dry signal by its
//     latency, so toggling it never shifts the timing of the chain.
//   * A module that faults (structured exception) or writes NaN/Inf for 3 blocks in a row is marked Faulted
//     and its slot passes the dry signal from then on.
// `n` must not exceed any instance's prepared max block size. RT-safe.
void processChain(ChainSnapshot& chain, float* l, float* r, uint32_t n, const dsptb_transport& transport,
                  bool ramps, ChainFaults& faults) noexcept;

// Calls reset() on every healthy instance (after a seek). RT-safe.
void resetChain(ChainSnapshot& chain, ChainFaults& faults) noexcept;

} // namespace dsptb
