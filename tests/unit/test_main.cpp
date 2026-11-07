#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dsptb/rt/rt_guard.hpp>

// Replaces global operator new/delete in this binary when DSPTB_RT_GUARD is on (Debug and rt-check).
DSPTB_DEFINE_RT_GUARD();
