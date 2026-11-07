#pragma once

#include "module_instance.hpp"

#include <dsptb/rt/spsc_ring.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace dsptb {

// A tap's destination: an SPSC ring of interleaved stereo floats (L,R,L,R,...) plus counters
//. The engine writes whole blocks; if the ring cannot hold a whole block it drops the
// block and counts the frames, so the consumer always sees frame-aligned data.
struct TapRing {
    explicit TapRing(size_t capacityFrames) : ring(capacityFrames * 2) {}

    rt::SpscRing<float> ring;
    std::atomic<uint64_t> framesWritten{0}; // sample clock of the next frame to be written
    std::atomic<uint64_t> droppedFrames{0};
    // An analyzer's ring gets nothing while a paused file is the source. A recording keeps its timeline, so its
    // ring leaves this off. Set before the ring is published.
    bool stopsWhenPaused = false;
};

// One processor in the chain. The instance is shared with the SessionModel and with older/newer snapshots, so
// it stays alive until the last snapshot that mentions it has been retired.
struct ProcessorSlot {
    std::shared_ptr<ModuleInstance> instance;
    bool enabled = true; // target state; the instance's RtState ramps toward it over 20 ms
};

// What the audio thread needs for one block, immutable once published through RtSwap.
// Pointers to TapRings are not owned: whoever creates a TapRing keeps it alive until a snapshot without it
// has been retired.
struct ChainSnapshot {
    std::vector<ProcessorSlot> slots;
    std::vector<TapRing*> preTaps;  // raw source
    std::vector<TapRing*> postTaps; // after the processor chain
    // Keeps the rings above alive until this snapshot is retired, even if the analyzer was removed meanwhile.
    std::vector<std::shared_ptr<TapRing>> tapOwners;
    uint32_t latencyFrames = 0; // total reported chain latency
    // Reorder dip: while true the chain output fades to silence over 10 ms (processors
    // keep running so their state stays continuous); the next snapshot clears it and fades back in.
    bool outputMuted = false;
};

} // namespace dsptb
