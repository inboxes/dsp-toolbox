#pragma once

// Allocation counter for the test executable.
//
// The global operator new / operator delete overrides live in test_main.cpp,
// so the counter exists inside the test executable only — the libraries under
// test never see it. An override calls noteAllocation() for every allocation;
// the counter only advances while an AllocGuard is alive on this thread.
//
// Usage (spec §3):
//   AllocGuard g;
//   ... code that must not allocate ...
//   CHECK(g.count() == 0);

#include <cstddef>

namespace dsptb::test {

// One counter per thread: real-time code is checked on the thread that runs
// it, and tests on other threads must not pollute the count.
inline thread_local std::size_t allocCount = 0;
inline thread_local bool allocCountingEnabled = false;

// Called by the global operator new overrides in test_main.cpp.
inline void noteAllocation() noexcept {
    if (allocCountingEnabled)
        ++allocCount;
}

// RAII guard: enables counting on construction, restores the previous state
// on destruction (so guards may nest). count() is the number of allocations
// made on this thread since the guard was constructed.
class AllocGuard {
public:
    AllocGuard() noexcept : start_(allocCount), previousEnabled_(allocCountingEnabled) {
        allocCountingEnabled = true;
    }
    ~AllocGuard() { allocCountingEnabled = previousEnabled_; }

    AllocGuard(const AllocGuard&) = delete;
    AllocGuard& operator=(const AllocGuard&) = delete;
    AllocGuard(AllocGuard&&) = delete;
    AllocGuard& operator=(AllocGuard&&) = delete;

    std::size_t count() const noexcept { return allocCount - start_; }

private:
    std::size_t start_;
    bool previousEnabled_;
};

} // namespace dsptb::test
