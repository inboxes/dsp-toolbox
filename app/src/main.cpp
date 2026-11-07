#include "app.hpp"

#include <dsptb/rt/rt_guard.hpp>

#include <cstdio>

// Replaces global operator new/delete when DSPTB_RT_GUARD is on (Debug, rt-check): the smoke test then fails
// if the audio thread ever allocates.
DSPTB_DEFINE_RT_GUARD();

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    const auto options = dsptb::app::parseCommandLine();
    return dsptb::app::run(instance, options);
}
