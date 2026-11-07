#include <doctest/doctest.h>

#include <dsptb/dsp_common/version.hpp>

#include <cstring>

TEST_CASE("dsp_common version placeholder returns 0.1.0") {
    const char* v = dsptb::dsp::version();
    REQUIRE(v != nullptr);
    CHECK(std::strcmp(v, "0.1.0") == 0);
}
