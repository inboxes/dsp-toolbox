// Test executable entry point: the doctest main, plus the global operator
// new / delete overrides that feed the allocation counter in
// support/alloc_counter.hpp (spec §3: the overrides live inside the test
// executable only).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "support/alloc_counter.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif

// GCC/Clang flag std::free() inside a replacement operator delete as a
// mismatched deallocation when the delete is inlined into callers that used
// our operator new above. The pairing is correct by construction (every
// allocation here comes from malloc / aligned_alloc), so silence that one
// diagnostic for the definitions below only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmismatched-new-delete"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

// The nothrow variants (operator new(size, const std::nothrow_t&) and its
// array form) are deliberately not overridden: on libstdc++ and MSVC they
// route through the throwing operator new below, so they are counted too.
void* operator new(std::size_t size) {
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr)
        throw std::bad_alloc();
    dsptb::test::noteAllocation();
    return ptr;
}

void* operator new[](std::size_t size) {
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr)
        throw std::bad_alloc();
    dsptb::test::noteAllocation();
    return ptr;
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    const std::size_t align = static_cast<std::size_t>(alignment);
    // std::aligned_alloc requires the size to be a multiple of the alignment.
    const std::size_t rounded = ((size + align - 1) / align) * align;
    const std::size_t wanted = rounded == 0 ? align : rounded;
#if defined(_MSC_VER)
    // MSVC's CRT has no std::aligned_alloc; _aligned_malloc is its equivalent
    // (and its memory must be released with _aligned_free, below).
    void* ptr = _aligned_malloc(wanted, align);
#else
    void* ptr = std::aligned_alloc(align, wanted);
#endif
    if (ptr == nullptr)
        throw std::bad_alloc();
    dsptb::test::noteAllocation();
    return ptr;
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return operator new(size, alignment); }

namespace {
// Aligned allocations need their platform-matched release function.
inline void freeAligned(void* ptr) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
} // namespace

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { freeAligned(ptr); }

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
