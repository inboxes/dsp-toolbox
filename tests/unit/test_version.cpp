#include <doctest/doctest.h>

#include <dsptb/core/version.hpp>

#include <cstring>

TEST_CASE("version string matches the CMake project version") {
    CHECK(std::strcmp(dsptb::versionString(), "0.2.0") == 0);
}
