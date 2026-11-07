#include <doctest/doctest.h>

#include "support/alloc_counter.hpp"

using dsptb::test::AllocGuard;

TEST_CASE("AllocGuard counts a new and ignores non-allocating work") {
    {
        AllocGuard guard;
        // Plain arithmetic must not allocate.
        volatile int value = 40;
        value += 2;
        CHECK(value == 42);
        CHECK(guard.count() == 0);
    }
    {
        AllocGuard guard;
        int* number = new int(42);
        CHECK(*number == 42);
        CHECK(guard.count() >= 1);
        delete number;
    }
    {
        AllocGuard guard;
        float* samples = new float[16];
        samples[0] = 1.0f;
        CHECK(samples[0] == doctest::Approx(1.0f));
        CHECK(guard.count() >= 1);
        delete[] samples;
    }
}

TEST_CASE("AllocGuard stops counting when it goes out of scope") {
    {
        AllocGuard guard;
        CHECK(guard.count() == 0);
    }
    // This allocation is outside any guard and must not be visible to a
    // guard created afterwards.
    int* number = new int(7);
    AllocGuard guard;
    CHECK(guard.count() == 0);
    delete number;
}
