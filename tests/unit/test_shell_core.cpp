#include <doctest/doctest.h>

#include <dsptb/core/logging.hpp>
#include <dsptb/core/notifications.hpp>
#include <dsptb/core/paths.hpp>
#include <dsptb/core/settings.hpp>

#include <Windows.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST_CASE("settings round-trip through JSON") {
    dsptb::Settings a;
    a.window = {10, 20, 1111, 777, true, true};
    a.uiScalePercent = 150;
    a.recentSessions = {"a.json", "b.json"};
    a.recentAudioFiles = {"x.wav"};
    a.layoutIni = "[Window][Foo]\nPos=1,2\n";
    a.panelsOpen["Log"] = false;
    a.audio = {"wasapi-exclusive", "in-id", "out-id", 96000, 128};
    a.micMonitorWarningShown = true;
    a.micGateEnabled = true;
    a.micGateThresholdDb = -42.5f;

    dsptb::Settings b;
    REQUIRE(b.fromJsonString(a.toJsonString()));
    CHECK(b.window.x == 10);
    CHECK(b.window.w == 1111);
    CHECK(b.window.maximized);
    CHECK(b.window.valid);
    CHECK(b.uiScalePercent == 150);
    CHECK(b.recentSessions == a.recentSessions);
    CHECK(b.recentAudioFiles == a.recentAudioFiles);
    CHECK(b.layoutIni == a.layoutIni);
    CHECK(b.panelsOpen.at("Log") == false);
    CHECK(b.audio.backend == "wasapi-exclusive");
    CHECK(b.audio.inputDeviceId == "in-id");
    CHECK(b.audio.outputDeviceId == "out-id");
    CHECK(b.audio.sampleRate == 96000);
    CHECK(b.audio.bufferFrames == 128);
    CHECK(b.micMonitorWarningShown);
    CHECK(b.micGateEnabled);
    CHECK(b.micGateThresholdDb == doctest::Approx(-42.5f));
}

TEST_CASE("settings tolerate junk and missing keys") {
    dsptb::Settings s;
    s.uiScalePercent = 200;
    CHECK_FALSE(s.fromJsonString("{ not json"));
    CHECK(s.uiScalePercent == 200); // untouched on failure

    REQUIRE(s.fromJsonString(R"({"uiScalePercent": 137, "recentSessions": [1, "ok"]})"));
    CHECK(s.uiScalePercent == 125); // snapped to the nearest supported step
    CHECK(s.recentSessions == std::vector<std::string>{"ok"});
    CHECK_FALSE(s.window.valid);
}

TEST_CASE("recent lists dedupe, reorder and cap") {
    std::vector<std::string> list;
    for (int i = 0; i < 15; ++i)
        dsptb::Settings::pushRecent(list, "f" + std::to_string(i));
    CHECK(list.size() == 10);
    CHECK(list.front() == "f14");
    dsptb::Settings::pushRecent(list, "f10");
    CHECK(list.front() == "f10");
    CHECK(list.size() == 10);
}

TEST_CASE("settings save/load via file with atomic replace") {
    SetEnvironmentVariableW(L"DSPTB_DATA_DIR", (fs::temp_directory_path() / "dsptb_test_data").c_str());
    const fs::path file = dsptb::dataDir() / "settings_test.json";
    fs::remove(file);

    dsptb::Settings s;
    CHECK(s.load(file) == dsptb::Settings::LoadResult::Missing);
    s.uiScalePercent = 125;
    REQUIRE(s.save(file));
    REQUIRE(s.save(file)); // overwrite an existing file

    dsptb::Settings t;
    CHECK(t.load(file) == dsptb::Settings::LoadResult::Loaded);
    CHECK(t.uiScalePercent == 125);

    std::ofstream(file, std::ios::trunc) << "garbage";
    CHECK(t.load(file) == dsptb::Settings::LoadResult::Corrupt);
    fs::remove(file);
}

TEST_CASE("DSPTB_DATA_DIR overrides the data directory") {
    const fs::path over = fs::temp_directory_path() / "dsptb_test_override";
    SetEnvironmentVariableW(L"DSPTB_DATA_DIR", over.c_str());
    CHECK(dsptb::dataDir() == over);
    CHECK(fs::is_directory(over));
    CHECK(dsptb::logDir() == over / "logs");
}

TEST_CASE("notifications expire, dedupe, dismiss and cap") {
    dsptb::NotificationCenter nc;
    const auto id = nc.push(dsptb::NoticeLevel::Error, "boom", 0.0);
    CHECK(nc.active().size() == 1);

    nc.push(dsptb::NoticeLevel::Error, "boom", 5.0); // same text: timer refreshed, no duplicate
    CHECK(nc.active().size() == 1);
    nc.update(9.0); // original 8 s would have expired
    CHECK(nc.active().size() == 1);
    nc.update(13.0);
    CHECK(nc.empty());

    const auto a = nc.push(dsptb::NoticeLevel::Info, "a", 20.0);
    nc.dismiss(a);
    CHECK(nc.empty());
    (void)id;

    for (int i = 0; i < 10; ++i)
        nc.push(dsptb::NoticeLevel::Info, "n" + std::to_string(i), 30.0);
    CHECK(nc.active().size() == dsptb::NotificationCenter::kMaxVisible);
    CHECK(nc.active().back().text == "n9");
}

TEST_CASE("log buffer keeps the newest lines and bumps its version") {
    dsptb::LogBuffer buf(3);
    const auto v0 = buf.version();
    for (int i = 0; i < 5; ++i)
        buf.push(spdlog::level::info, "line" + std::to_string(i));
    CHECK(buf.version() > v0);

    std::vector<dsptb::LogEntry> out;
    buf.snapshot(out);
    REQUIRE(out.size() == 3);
    CHECK(out.front().text == "line2");
    CHECK(out.back().text == "line4");

    buf.clear();
    buf.snapshot(out);
    CHECK(out.empty());
}

TEST_CASE("spdlog lines reach the ring buffer with their level") {
    auto buffer = dsptb::initLogging({}); // no file sink
    spdlog::warn("hello {}", 42);
    spdlog::debug("quiet");
    std::vector<dsptb::LogEntry> out;
    buffer->snapshot(out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].level == spdlog::level::warn);
    CHECK(out[0].text.find("hello 42") != std::string::npos);
    CHECK(out[0].text.find("[warning]") != std::string::npos);
    dsptb::shutdownLogging();
}
