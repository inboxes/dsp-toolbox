#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <dsptb/rt/rt_guard.hpp>

#include <Windows.h>

#include <filesystem>
#include <string>
#include <system_error>

// Replaces global operator new/delete in this binary when DSPTB_RT_GUARD is on (Debug and rt-check).
DSPTB_DEFINE_RT_GUARD();

// The tests run with their own per-user data folder, so they never write settings or module data into the
// real one. Child processes (dsptb-cli) inherit it.
int main(int argc, char** argv) {
    const std::filesystem::path data =
        std::filesystem::temp_directory_path() / ("dsptb-tests-data-" + std::to_string(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"DSPTB_DATA_DIR", data.c_str());
    doctest::Context context(argc, argv);
    const int result = context.run();
    std::error_code ec;
    std::filesystem::remove_all(data, ec);
    return result;
}
