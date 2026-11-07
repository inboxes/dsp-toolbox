// Smoke test that the two third-party dependencies the scaffold wires up
// (spec §2) are really linkable and usable in both build modes: PFFFT's
// classic float API and libebur128's C API.

#include <doctest/doctest.h>

#include <ebur128.h>
#include <pffft/pffft.h>

TEST_CASE("dependencies: PFFFT creates and destroys a real FFT setup") {
    PFFFT_Setup* setup = pffft_new_setup(64, PFFFT_REAL);
    REQUIRE(setup != nullptr);
    pffft_destroy_setup(setup);
}

TEST_CASE("dependencies: libebur128 initialises a momentary loudness state") {
    ebur128_state* state = ebur128_init(2, 48000, EBUR128_MODE_M);
    REQUIRE(state != nullptr);
    ebur128_destroy(&state);
    CHECK(state == nullptr);
}
