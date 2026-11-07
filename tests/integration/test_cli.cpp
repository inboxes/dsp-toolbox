// Runs the real dsptb-cli executable (path injected by CMake) and checks the acceptance numbers.
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

struct RunResult {
    int exitCode = -1;
    std::string out;
};

RunResult runCli(const std::string& args) {
    // cmd.exe strips one pair of outer quotes, so wrap the whole command line.
    const std::string cmd = "\"\"" + std::string(DSPTB_CLI_PATH) + "\" " + args + " 2>&1\"";
    RunResult r;
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        r.out += buf;
    r.exitCode = _pclose(pipe);
    return r;
}

std::string q(const fs::path& p) {
    return "\"" + p.string() + "\"";
}

fs::path dir() {
    const fs::path d = fs::temp_directory_path() / "dsptb_test_cli";
    fs::create_directories(d);
    return d;
}

} // namespace

TEST_CASE("cli: gen sine -6 dBFS then analyze --json reports -6.00 peak and -9.01 RMS") {
    const fs::path wav = dir() / "t.wav";
    REQUIRE(runCli("gen sine --freq 1000 --dbfs -6 --out " + q(wav)).exitCode == 0);
    const RunResult r = runCli("analyze " + q(wav) + " --json");
    REQUIRE(r.exitCode == 0);
    const auto j = nlohmann::json::parse(r.out);
    CHECK(std::abs(j["peakDbfs"].get<double>() - (-6.00)) <= 0.01);
    CHECK(std::abs(j["rmsDbfs"].get<double>() - (-9.01)) <= 0.02);
    CHECK(j["channels"] == 2);
    CHECK(j["frames"] == 240000);
    CHECK(j["perChannel"].size() == 2);
}

TEST_CASE("cli: gen options, formats and errors") {
    const fs::path wav = dir() / "imp.wav";
    REQUIRE(runCli("gen impulse --dbfs 0 --seconds 1 --channels 1 --rate 44100 --format s24 --out " + q(wav))
                .exitCode == 0);
    const auto j = nlohmann::json::parse(runCli("analyze " + q(wav) + " --json").out);
    CHECK(j["channels"] == 1);
    CHECK(j["sampleRate"] == 44100);
    CHECK(j["perChannel"][0]["onsetSample"] == 4410);

    CHECK(runCli("gen banana --out " + q(dir() / "x.wav")).exitCode == 2);
    CHECK(runCli("gen sine --format mp3 --out " + q(dir() / "x.wav")).exitCode == 2);
    CHECK(runCli("analyze " + q(dir() / "missing.wav")).exitCode == 2);
}

TEST_CASE("cli: compare exits 0 for identical files and 1 above the tolerance") {
    const fs::path a = dir() / "a.wav", b = dir() / "b.wav", c = dir() / "c.wav";
    REQUIRE(runCli("gen sine --dbfs -6 --seconds 1 --out " + q(a)).exitCode == 0);
    REQUIRE(runCli("gen sine --dbfs -6 --seconds 1 --out " + q(b)).exitCode == 0);
    REQUIRE(runCli("gen sine --dbfs -7 --seconds 1 --out " + q(c)).exitCode == 0);
    CHECK(runCli("compare " + q(a) + " " + q(b)).exitCode == 0);
    CHECK(runCli("compare " + q(a) + " " + q(c)).exitCode == 1);
    CHECK(runCli("compare " + q(a) + " " + q(c) + " --tolerance 0.5").exitCode == 0);
    const auto j = nlohmann::json::parse(runCli("compare " + q(a) + " " + q(c) + " --json").out);
    CHECK(j["pass"] == false);
    CHECK(j["maxAbsDiff"].get<double>() > 0.05);
}

TEST_CASE("cli: bench runs the empty chain with no RT guard hits and no underruns") {
    const RunResult r = runCli("bench --block 256 --seconds 5 --json");
    REQUIRE(r.exitCode == 0);
    const auto j = nlohmann::json::parse(r.out);
    CHECK(j["rtGuardViolations"] == 0);
    CHECK(j["underruns"] == 0);
    CHECK(j["callbacks"] == 937);
#ifdef NDEBUG
    CHECK(j["p99Percent"].get<double>() < 2.0); // p99 < 2 % of the block period
#else
    CHECK(j["p99Percent"].get<double>() < 100.0);
#endif
}

TEST_CASE("cli: devices --json is well-formed") {
    const RunResult r = runCli("devices --json");
    const auto j = nlohmann::json::parse(r.out); // CI runners may have no devices; the shape must still hold
    CHECK(j.contains("devices"));
    CHECK(j.contains("backends"));
}
