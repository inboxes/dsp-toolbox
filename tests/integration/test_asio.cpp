// Sample-format conversion (always built) and the ASIO backend (only in builds with DSPTB_ENABLE_ASIO).
#include <doctest/doctest.h>

#include <dsptb/core/asio_buffer.hpp>
#include <dsptb/core/backend.hpp>
#include <dsptb/core/sample_convert.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

#ifdef DSPTB_HAS_ASIO
#include <dsptb/core/engine.hpp>
#endif

using namespace dsptb;

namespace {

// One LSB of the format, as a float amplitude.
double lsb(WireFormat f) {
    switch (f) {
    case WireFormat::Float32:
        return 1e-7;
    case WireFormat::Float64:
        return 1e-9;
    case WireFormat::Int16:
        return 1.0 / 32767.0;
    case WireFormat::Int24:
        return 1.0 / 8388607.0;
    default:
        return 1.0 / static_cast<double>((1LL << (wireSignificantBits(f) - 1)) - 1);
    }
}

size_t bytesPerSample(WireFormat f) {
    switch (f) {
    case WireFormat::Int16:
        return 2;
    case WireFormat::Int24:
        return 3;
    case WireFormat::Float64:
        return 8;
    default:
        return 4;
    }
}

} // namespace

TEST_CASE("asio: every driver sample format round-trips within one LSB, and clips instead of wrapping") {
    constexpr uint32_t kFrames = 1000;
    // Interleaved stereo bus: the left channel is a ramp over [-1, 1], the right channel something else.
    std::vector<float> bus(kFrames * 2);
    for (uint32_t i = 0; i < kFrames; ++i) {
        bus[i * 2] = -1.0f + 2.0f * static_cast<float>(i) / (kFrames - 1);
        bus[i * 2 + 1] = 0.25f;
    }
    for (WireFormat f :
         {WireFormat::Int16, WireFormat::Int24, WireFormat::Int32, WireFormat::Float32, WireFormat::Float64,
          WireFormat::Int32In16, WireFormat::Int32In18, WireFormat::Int32In20, WireFormat::Int32In24}) {
        CAPTURE(static_cast<int>(f));
        std::vector<unsigned char> wire(kFrames * bytesPerSample(f) + 8, 0xCD);
        writeWire(f, wire.data(), bus.data(), kFrames, 2);
        // Nothing may be written past the last sample.
        for (size_t i = kFrames * bytesPerSample(f); i < wire.size(); ++i)
            CHECK(wire[i] == 0xCD);
        std::vector<float> back(kFrames * 2, 9.0f);
        readWire(f, wire.data(), back.data(), kFrames, 2);
        double worst = 0.0;
        for (uint32_t i = 0; i < kFrames; ++i) {
            worst = std::max(worst, std::fabs(static_cast<double>(back[i * 2]) - bus[i * 2]));
            CHECK(back[i * 2 + 1] == 9.0f); // the other channel is untouched
        }
        CHECK(worst <= 1.5 * lsb(f) + 1e-6);
    }
    // Out-of-range input is clamped (a wrapped 16-bit value would flip sign).
    const float loud[2] = {3.0f, -3.0f};
    int16_t i16[2] = {};
    writeWire(WireFormat::Int16, i16, loud, 2, 1);
    CHECK(i16[0] == 32767);
    CHECK(i16[1] == -32767);
    int32_t i32[2] = {};
    writeWire(WireFormat::Int32, i32, loud, 2, 1);
    CHECK(i32[0] == 2147483647);
    CHECK(i32[1] == -2147483647);
}

TEST_CASE("asio: 24-bit packed samples keep their sign") {
    const unsigned char minusOneLsb[3] = {0xFF, 0xFF, 0xFF};
    float out = 0.0f;
    readWire(WireFormat::Int24, minusOneLsb, &out, 1, 1);
    CHECK(out == doctest::Approx(-1.0 / 8388608.0).epsilon(1e-6));
    const unsigned char fullNegative[3] = {0x00, 0x00, 0x80};
    readWire(WireFormat::Int24, fullNegative, &out, 1, 1);
    CHECK(out == doctest::Approx(-1.0f));
}

TEST_CASE("asio: the backend name parses only in builds that have ASIO") {
    BackendKind k = BackendKind::WasapiShared;
#ifdef DSPTB_HAS_ASIO
    CHECK(parseBackendKind("asio", k));
    const auto kinds = availableBackends();
    CHECK(std::find(kinds.begin(), kinds.end(), k) != kinds.end());
#else
    CHECK_FALSE(parseBackendKind("asio", k));
    CHECK(availableBackends().size() == 2);
#endif
    CHECK(parseBackendKind("wasapi-exclusive", k));
    CHECK(parseBackendKind("null", k));
}

#ifdef DSPTB_HAS_ASIO
TEST_CASE("asio: installed drivers are listed without being loaded") {
    std::vector<DeviceInfo> drivers;
    std::string error;
    CHECK(enumerateDevices(BackendKind::Asio, drivers, error));
    for (const auto& d : drivers) {
        CHECK_FALSE(d.name.empty());
        CHECK(d.id.size() >= 36); // a CLSID in braces
        CHECK_FALSE(d.isInput);
    }
}

TEST_CASE("asio: a missing driver is reported, not a crash") {
    AudioConfig cfg;
    cfg.backend = BackendKind::Asio;
    cfg.outputDeviceId = "{00000000-0000-0000-0000-000000000000}";
    Engine engine;
    std::string error;
    CHECK_FALSE(engine.open(cfg, error));
    CHECK_FALSE(error.empty());
}

// Real hardware: opt in with DSPTB_TEST_ASIO=1 (silent output through the first installed driver).
TEST_CASE("asio: the first installed driver streams silence" *
          doctest::skip(!std::getenv("DSPTB_TEST_ASIO"))) {
    AudioConfig cfg;
    cfg.backend = BackendKind::Asio;
    cfg.sampleRate = 48000;
    cfg.bufferFrames = 256;
    Engine engine;
    std::string error;
    REQUIRE_MESSAGE(engine.open(cfg, error), error);
    CHECK(engine.sampleRate() >= 8000);
    CHECK(engine.bufferFrames() > 0);
    REQUIRE_MESSAGE(engine.start(error), error);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_FALSE(engine.deviceLost());
    CHECK_FALSE(engine.describeBackend().empty());
    engine.stop();
    engine.close();
}
#endif

TEST_CASE("asio: the buffer size follows the driver's range, granularity and preferred size") {
    // Powers of two above the minimum (granularity -1): the first size at or above the request.
    CHECK(pickAsioBufferSize(64, 64, 2048, 512, -1) == 64);
    CHECK(pickAsioBufferSize(100, 64, 2048, 512, -1) == 128);
    CHECK(pickAsioBufferSize(128, 64, 2048, 512, -1) == 128);
    CHECK(pickAsioBufferSize(8192, 64, 2048, 512, -1) == 2048); // above the maximum: the maximum
    CHECK(pickAsioBufferSize(16, 64, 2048, 512, -1) == 64);     // below the minimum: the minimum
    CHECK(pickAsioBufferSize(256, 48, 960, 480, -1) == 384);    // min * 2^n, not a global power of two
    // A fixed step: the nearest step, never past the maximum.
    CHECK(pickAsioBufferSize(250, 32, 1024, 256, 32) == 256);
    CHECK(pickAsioBufferSize(1000000, 32, 1024, 256, 32) == 1024);
    // A driver that allows only its preferred size (granularity 0 or min == max).
    CHECK(pickAsioBufferSize(64, 64, 2048, 480, 0) == 480);
    CHECK(pickAsioBufferSize(64, 256, 256, 256, 1) == 256);
    // A nonsense range falls back to the preferred size.
    CHECK(pickAsioBufferSize(256, 0, 0, 512, -1) == 512);
    CHECK(pickAsioBufferSize(256, 2048, 64, 512, -1) == 512);
}
