#include <doctest/doctest.h>

#include <dsptb/spatial_motion/version.hpp>

#include <cstring>

TEST_CASE("spatial_motion version placeholder returns 0.1.0") {
    const char* v = dsptb::spatial::version();
    REQUIRE(v != nullptr);
    CHECK(std::strcmp(v, "0.1.0") == 0);
}
