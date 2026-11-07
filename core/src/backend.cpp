#include "dsptb/core/backend.hpp"

#include "dsptb/core/miniaudio_backend.hpp"
#include "dsptb/core/null_backend.hpp"
#ifdef DSPTB_HAS_ASIO
#include "dsptb/core/asio_backend.hpp"
#endif

namespace dsptb {

const char* backendName(BackendKind kind) {
    switch (kind) {
    case BackendKind::WasapiShared:
        return "wasapi-shared";
    case BackendKind::WasapiExclusive:
        return "wasapi-exclusive";
#ifdef DSPTB_HAS_ASIO
    case BackendKind::Asio:
        return "asio";
#endif
    case BackendKind::Null:
        return "null";
    }
    return "?";
}

const char* backendDisplayName(BackendKind kind) {
    switch (kind) {
    case BackendKind::WasapiShared:
        return "WASAPI shared";
    case BackendKind::WasapiExclusive:
        return "WASAPI exclusive";
#ifdef DSPTB_HAS_ASIO
    case BackendKind::Asio:
        return "ASIO";
#endif
    case BackendKind::Null:
        return "Null (no audio)";
    }
    return "?";
}

std::vector<BackendKind> availableBackends() {
    return {BackendKind::WasapiShared, BackendKind::WasapiExclusive,
#ifdef DSPTB_HAS_ASIO
            BackendKind::Asio
#endif
    };
}

bool parseBackendKind(const std::string& name, BackendKind& out) {
    auto all = availableBackends();
    all.push_back(BackendKind::Null);
    for (BackendKind k : all)
        if (name == backendName(k)) {
            out = k;
            return true;
        }
    return false;
}

std::unique_ptr<IAudioBackend> createBackend(BackendKind kind) {
    if (kind == BackendKind::Null)
        return std::make_unique<NullBackend>();
#ifdef DSPTB_HAS_ASIO
    if (kind == BackendKind::Asio)
        return std::make_unique<AsioBackend>();
#endif
    return std::make_unique<MiniaudioBackend>();
}

bool enumerateDevices(std::vector<DeviceInfo>& out, std::string& error) {
    return MiniaudioBackend::enumerate(out, error);
}

bool enumerateDevices(BackendKind kind, std::vector<DeviceInfo>& out, std::string& error) {
#ifdef DSPTB_HAS_ASIO
    if (kind == BackendKind::Asio)
        return AsioBackend::enumerate(out, error);
#endif
    (void)kind;
    return MiniaudioBackend::enumerate(out, error);
}

} // namespace dsptb
