#pragma once

// One loaded HRTF together with the per-source binaural effects built on it. Steam Audio objects can be
// created on any thread, so a bundle is built off the audio thread (UI or a worker) and handed over through
// RtSwap; the audio thread only uses it. Destroying it (on the publisher side) releases everything.
//
// Reading a SOFA set takes about half a second for the 36 MB packaged one, so loaded HRTFs are cached for the
// life of the process (a few entries, least recently used out). All instances share one Steam Audio context
// for the same reason: an HRTF belongs to its context. The context is created once and never released,
// because the DLL can be unloaded after the Steam Audio library it calls into.

#include "scene.hpp"

#include <phonon.h>

#include <intrin.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsptb::spat {

inline std::mutex& sharedMutex() {
    static std::mutex* m = new std::mutex; // leaked on purpose, see above
    return *m;
}

// The widest SIMD level this CPU and OS support (Steam Audio faults on an instruction set the CPU lacks).
// CPUID, not <windows.h>: that header's macros (far, RaiseException, ...) clash with the SDK and this module.
inline IPLSIMDLevel bestSimdLevel() {
    int r[4]{};
    __cpuid(r, 1);
    const bool sse42 = (r[2] & (1 << 20)) != 0;
    const bool fma = (r[2] & (1 << 12)) != 0;
    const bool avx = (r[2] & (1 << 28)) != 0;
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    // AVX needs the OS to save the YMM registers (XCR0 bits 1 and 2).
    const bool ymmEnabled = osxsave && (_xgetbv(0) & 0x6) == 0x6;
    if (avx && ymmEnabled) {
        int r7[4]{};
        __cpuidex(r7, 7, 0);
        if (fma && (r7[1] & (1 << 5)) != 0) // leaf 7 EBX bit 5: AVX2
            return IPL_SIMDLEVEL_AVX2;
        return IPL_SIMDLEVEL_AVX;
    }
    return sse42 ? IPL_SIMDLEVEL_SSE4 : IPL_SIMDLEVEL_SSE2;
}

// The process-wide context (null with `ok` false if Steam Audio cannot start). Thread safe.
inline IPLContext sharedContext() {
    std::lock_guard<std::mutex> lock(sharedMutex());
    static IPLContext ctx = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        IPLContextSettings cs{};
        cs.version = STEAMAUDIO_VERSION;
        cs.simdLevel = bestSimdLevel();
        if (iplContextCreate(&cs, &ctx) != IPL_STATUS_SUCCESS)
            ctx = nullptr;
    }
    return ctx;
}

struct HrtfBundle {
    IPLContext ctx = nullptr; // the shared context, not owned
    IPLHRTF hrtf = nullptr;   // retained
    std::array<IPLBinauralEffect, kMaxSources> fx{};
    std::string id; // "builtin", or the set's id

    HrtfBundle() = default;
    HrtfBundle(const HrtfBundle&) = delete;
    HrtfBundle& operator=(const HrtfBundle&) = delete;
    ~HrtfBundle() {
        for (auto& e : fx)
            if (e)
                iplBinauralEffectRelease(&e);
        if (hrtf)
            iplHRTFRelease(&hrtf);
    }

    // `sofaPath` empty = Steam Audio's built-in HRTF. Returns null with `error` set on failure.
    static std::unique_ptr<HrtfBundle> create(IPLContext context, const IPLAudioSettings& audio,
                                              const std::string& setId, const std::string& sofaPath,
                                              std::string& error) {
        auto b = std::make_unique<HrtfBundle>();
        b->id = setId;
        b->ctx = context;
        IPLAudioSettings a = audio;
        b->hrtf = cachedHrtf(context, a, sofaPath, error);
        if (!b->hrtf)
            return nullptr;
        IPLBinauralEffectSettings bs{};
        bs.hrtf = b->hrtf;
        for (auto& e : b->fx) {
            if (iplBinauralEffectCreate(context, &a, &bs, &e) != IPL_STATUS_SUCCESS) {
                error = "Steam Audio could not create a binaural effect";
                return nullptr;
            }
        }
        return b;
    }

private:
    struct CacheEntry {
        std::string key;
        IPLHRTF hrtf;
    };

    // A retained reference to the HRTF for (file, rate, frame size), loading it if needed.
    static IPLHRTF cachedHrtf(IPLContext context, IPLAudioSettings& audio, const std::string& sofaPath,
                              std::string& error) {
        static std::vector<CacheEntry>* cache = new std::vector<CacheEntry>; // leaked on purpose
        constexpr size_t kMaxEntries = 4;
        const std::string key =
            sofaPath + "|" + std::to_string(audio.samplingRate) + "|" + std::to_string(audio.frameSize);
        std::lock_guard<std::mutex> lock(sharedMutex());
        for (size_t i = 0; i < cache->size(); ++i) {
            if ((*cache)[i].key == key) {
                CacheEntry hit = (*cache)[i];
                cache->erase(cache->begin() + static_cast<std::ptrdiff_t>(i));
                cache->push_back(hit); // most recently used last
                return iplHRTFRetain(hit.hrtf);
            }
        }
        IPLHRTFSettings hs{};
        hs.type = sofaPath.empty() ? IPL_HRTFTYPE_DEFAULT : IPL_HRTFTYPE_SOFA;
        hs.sofaFileName = sofaPath.empty() ? nullptr : sofaPath.c_str();
        hs.volume = 1.0f;
        hs.normType = IPL_HRTFNORMTYPE_NONE;
        IPLHRTF h = nullptr;
        if (iplHRTFCreate(context, &audio, &hs, &h) != IPL_STATUS_SUCCESS) {
            error = sofaPath.empty() ? "Steam Audio could not create its built-in HRTF at this sample rate"
                                     : "Steam Audio could not load " + sofaPath +
                                           " (it needs a SimpleFreeFieldHRIR SOFA file with one distance)";
            return nullptr;
        }
        cache->push_back({key, h});
        if (cache->size() > kMaxEntries) {
            iplHRTFRelease(&(*cache)[0].hrtf); // bundles that still use it keep their own reference
            cache->erase(cache->begin());
        }
        return iplHRTFRetain(h);
    }
};

} // namespace dsptb::spat
