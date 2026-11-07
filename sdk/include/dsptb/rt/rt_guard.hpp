#pragma once

// Debug allocation guard.
//
// With DSPTB_RT_GUARD defined, a binary that places DSPTB_DEFINE_RT_GUARD() at global scope in exactly one
// .cpp gets replacement global operator new/delete. While the current thread is inside an RtScope, any
// call to them is recorded (counter + last call-site address) and breaks into an attached debugger.
// The engine opens an RtScope around the audio callback; tests assert that the counter stays 0.
// Each DLL has its own heap/CRT, so every module needs its own DSPTB_DEFINE_RT_GUARD() (done via sdk.hpp
// helpers).
//
// Without DSPTB_RT_GUARD everything here compiles to nothing.

#include <atomic>
#include <cstdint>

namespace dsptb::rt {

#if defined(DSPTB_RT_GUARD)

extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);

namespace detail {
inline thread_local bool tlInRtScope = false;
inline std::atomic<uint32_t>& violationCounter() noexcept {
    static std::atomic<uint32_t> counter{0};
    return counter;
}
inline std::atomic<void*>& lastViolationSite() noexcept {
    static std::atomic<void*> site{nullptr};
    return site;
}
} // namespace detail

// Marks the current thread as real-time for its lifetime. Nesting is fine.
class RtScope {
public:
    RtScope() noexcept : previous_(detail::tlInRtScope) { detail::tlInRtScope = true; }
    ~RtScope() { detail::tlInRtScope = previous_; }
    RtScope(const RtScope&) = delete;
    RtScope& operator=(const RtScope&) = delete;

private:
    bool previous_;
};

inline bool inRtScope() noexcept {
    return detail::tlInRtScope;
}
inline uint32_t rtGuardViolations() noexcept {
    return detail::violationCounter().load(std::memory_order_relaxed);
}
inline void resetRtGuardViolations() noexcept {
    detail::violationCounter().store(0, std::memory_order_relaxed);
}
inline void* rtGuardLastSite() noexcept {
    return detail::lastViolationSite().load(std::memory_order_relaxed);
}

namespace detail {
// Called from the replacement operators. `site` is the address of the offending allocation call.
inline void noteHeapUse(void* site) noexcept {
    if (!tlInRtScope)
        return;
    violationCounter().fetch_add(1, std::memory_order_relaxed);
    lastViolationSite().store(site, std::memory_order_relaxed);
    if (IsDebuggerPresent())
        __debugbreak();
}
} // namespace detail

} // namespace dsptb::rt

#include <cstdlib>
#include <intrin.h>
#include <new>

#define DSPTB_RT_GUARD_ALLOC_(size)                                                                          \
    ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                      \
    void* p = std::malloc((size) ? (size) : 1)
#define DSPTB_RT_GUARD_ALIGNED_ALLOC_(size, align)                                                           \
    ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                      \
    void* p = _aligned_malloc((size) ? (size) : 1, static_cast<std::size_t>(align))

#define DSPTB_DEFINE_RT_GUARD()                                                                              \
    void* operator new(std::size_t n) {                                                                      \
        DSPTB_RT_GUARD_ALLOC_(n);                                                                            \
        if (!p)                                                                                              \
            throw std::bad_alloc();                                                                          \
        return p;                                                                                            \
    }                                                                                                        \
    void* operator new[](std::size_t n) {                                                                    \
        DSPTB_RT_GUARD_ALLOC_(n);                                                                            \
        if (!p)                                                                                              \
            throw std::bad_alloc();                                                                          \
        return p;                                                                                            \
    }                                                                                                        \
    void* operator new(std::size_t n, const std::nothrow_t&) noexcept {                                      \
        DSPTB_RT_GUARD_ALLOC_(n);                                                                            \
        return p;                                                                                            \
    }                                                                                                        \
    void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {                                    \
        DSPTB_RT_GUARD_ALLOC_(n);                                                                            \
        return p;                                                                                            \
    }                                                                                                        \
    void* operator new(std::size_t n, std::align_val_t a) {                                                  \
        DSPTB_RT_GUARD_ALIGNED_ALLOC_(n, a);                                                                 \
        if (!p)                                                                                              \
            throw std::bad_alloc();                                                                          \
        return p;                                                                                            \
    }                                                                                                        \
    void* operator new[](std::size_t n, std::align_val_t a) {                                                \
        DSPTB_RT_GUARD_ALIGNED_ALLOC_(n, a);                                                                 \
        if (!p)                                                                                              \
            throw std::bad_alloc();                                                                          \
        return p;                                                                                            \
    }                                                                                                        \
    void operator delete(void* p) noexcept {                                                                 \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        std::free(p);                                                                                        \
    }                                                                                                        \
    void operator delete[](void* p) noexcept {                                                               \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        std::free(p);                                                                                        \
    }                                                                                                        \
    void operator delete(void* p, std::size_t) noexcept {                                                    \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        std::free(p);                                                                                        \
    }                                                                                                        \
    void operator delete[](void* p, std::size_t) noexcept {                                                  \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        std::free(p);                                                                                        \
    }                                                                                                        \
    void operator delete(void* p, const std::nothrow_t&) noexcept {                                          \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        std::free(p);                                                                                        \
    }                                                                                                        \
    void operator delete[](void* p, const std::nothrow_t&) noexcept {                                        \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        std::free(p);                                                                                        \
    }                                                                                                        \
    void operator delete(void* p, std::align_val_t) noexcept {                                               \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        _aligned_free(p);                                                                                    \
    }                                                                                                        \
    void operator delete[](void* p, std::align_val_t) noexcept {                                             \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        _aligned_free(p);                                                                                    \
    }                                                                                                        \
    void operator delete(void* p, std::size_t, std::align_val_t) noexcept {                                  \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        _aligned_free(p);                                                                                    \
    }                                                                                                        \
    void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {                                \
        ::dsptb::rt::detail::noteHeapUse(_ReturnAddress());                                                  \
        _aligned_free(p);                                                                                    \
    }                                                                                                        \
    static_assert(true, "DSPTB_DEFINE_RT_GUARD() must be used at global scope")

#else // !DSPTB_RT_GUARD

class RtScope {
public:
    RtScope() noexcept {}
    RtScope(const RtScope&) = delete;
    RtScope& operator=(const RtScope&) = delete;
};
inline bool inRtScope() noexcept {
    return false;
}
inline uint32_t rtGuardViolations() noexcept {
    return 0;
}
inline void resetRtGuardViolations() noexcept {}
inline void* rtGuardLastSite() noexcept {
    return nullptr;
}

} // namespace dsptb::rt

#define DSPTB_DEFINE_RT_GUARD() static_assert(true, "")

#endif
