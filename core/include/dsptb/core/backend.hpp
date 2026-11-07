#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dsptb {

// Asio exists only in builds made with DSPTB_ENABLE_ASIO: without it the name "asio" does not parse, so
// a session saved with ASIO loads on other builds with the current audio settings kept.
enum class BackendKind {
    WasapiShared,
    WasapiExclusive,
#ifdef DSPTB_HAS_ASIO
    Asio,
#endif
    Null
};

const char* backendName(BackendKind kind); // "wasapi-shared", "wasapi-exclusive", "asio", "null"
bool parseBackendKind(const std::string& name, BackendKind& out);
const char* backendDisplayName(BackendKind kind);

struct DeviceInfo {
    std::string id; // opaque, stable per device; empty means "system default"
    std::string name;
    bool isDefault = false;
    bool isInput = false;
};

// Audio configuration. Defaults are WASAPI shared, system default devices, 48 kHz, 256 frames.
struct AudioConfig {
    BackendKind backend = BackendKind::WasapiShared;
    std::string inputDeviceId;   // empty = default
    std::string outputDeviceId;  // empty = default
    uint32_t sampleRate = 48000; // shared mode follows the device's mix rate instead
    uint32_t bufferFrames = 256;
    bool capture = false; // open the input side too (duplex): needed for the Mic source
};

// Implemented by the engine. Called on the backend's audio thread with interleaved stereo float32.
// `in` is null when the stream has no capture side. `frames` may vary from call to call.
class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;
    virtual void onAudio(const float* in, float* out, uint32_t frames) noexcept = 0;
};

// One audio device stream. open/start/stop/close are called from the UI thread only.
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual bool open(const AudioConfig& config, IAudioCallback* callback, std::string& error) = 0;
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual uint32_t sampleRate() const = 0;    // what the stream really runs at
    virtual uint32_t bufferFrames() const = 0;  // the device period actually in use
    virtual uint32_t latencyFrames() const = 0; // buffered frames, output plus input when duplex
    // True once the device went away or failed while streaming (unplugged, reconfigured, ...).
    virtual bool deviceLost() const = 0;
    // Human-readable details of what the device really negotiated (periods, rates), for diagnostics.
    virtual std::string describe() const { return {}; }
    // Opens the driver's own settings window (ASIO). UI thread; false if the backend has none.
    virtual bool openControlPanel() { return false; }
};

std::unique_ptr<IAudioBackend> createBackend(BackendKind kind);

// Lists WASAPI input and output devices. Returns false with an error if the audio system is unavailable.
bool enumerateDevices(std::vector<DeviceInfo>& out, std::string& error);

// The backends this build offers, in menu order (Null is not included). Used by Audio Settings and the CLI.
std::vector<BackendKind> availableBackends();
// Devices for one backend. ASIO lists drivers (one entry each, isInput false); WASAPI lists inputs and
// outputs.
bool enumerateDevices(BackendKind kind, std::vector<DeviceInfo>& out, std::string& error);

} // namespace dsptb
