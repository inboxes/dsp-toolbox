#pragma once

// Buffer-size negotiation for ASIO drivers (SDK-free so it can be unit tested without a driver).

#include <algorithm>

namespace dsptb {

// ASIOGetBufferSize reports min, max, preferred and a granularity: -1 means min * 2^n, 0 means the preferred
// size only, a positive value is the step between min and max. Returns the size closest to `want` (never
// below it when a larger one is allowed in the power-of-two case) that the driver accepts, or the preferred
// size when the reported range is unusable.
inline long pickAsioBufferSize(long want, long minSize, long maxSize, long preferred, long granularity) {
    if (minSize <= 0 || maxSize <= 0 || minSize > maxSize)
        return preferred;
    if (minSize == maxSize || granularity == 0)
        return preferred > 0 ? preferred : minSize;
    want = std::clamp(want, minSize, maxSize);
    if (granularity < 0) { // powers of two above the minimum
        long best = minSize;
        for (long p = minSize; p <= maxSize; p *= 2) {
            best = p;
            if (p >= want)
                break;
        }
        return best;
    }
    const long steps = (want - minSize + granularity / 2) / granularity;
    return std::min(maxSize, minSize + steps * granularity);
}

} // namespace dsptb
