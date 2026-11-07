#include "dsptb/core/asio_backend.hpp"

#include "dsptb/core/asio_buffer.hpp"
#include "dsptb/core/paths.hpp"
#include "dsptb/core/sample_convert.hpp"

#include <Windows.h>
#include <objbase.h>

// Steinberg ASIO SDK, used under its GPLv3 option. Only the interface headers are
// used.
#include "iasiodrv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace dsptb {

namespace {

struct DriverEntry {
    std::string name; // the registry key name, shown to the user
    std::string id;   // the CLSID text, the device id the rest of the app sees
    CLSID clsid{};
};

// ASIO drivers register under HKLM\SOFTWARE\ASIO\<name> with a CLSID value (Steinberg's host convention).
std::vector<DriverEntry> listDrivers() {
    std::vector<DriverEntry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) !=
        ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD nameLen = 256;
        const LSTATUS rc = RegEnumKeyExW(root, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS)
            continue; // e.g. a key name longer than the buffer: skip that one, keep listing the rest
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_WOW64_64KEY, &sub) != ERROR_SUCCESS)
            continue;
        wchar_t text[128];
        DWORD size = sizeof(text), type = 0;
        if (RegQueryValueExW(sub, L"CLSID", nullptr, &type, reinterpret_cast<LPBYTE>(text), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            text[127] = L'\0';
            DriverEntry e;
            if (SUCCEEDED(CLSIDFromString(text, &e.clsid))) {
                e.name = toUtf8(std::filesystem::path(std::wstring(name, nameLen)));
                e.id = toUtf8(std::filesystem::path(text));
                out.push_back(std::move(e));
            }
        }
        RegCloseKey(sub);
    }
    RegCloseKey(root);
    return out;
}

// ---- the driver's channel formats (conversion itself is in sample_convert.hpp) ----

bool toWire(ASIOSampleType t, WireFormat& out) {
    switch (t) {
    case ASIOSTInt16LSB:
        out = WireFormat::Int16;
        return true;
    case ASIOSTInt24LSB:
        out = WireFormat::Int24;
        return true;
    case ASIOSTInt32LSB:
        out = WireFormat::Int32;
        return true;
    case ASIOSTFloat32LSB:
        out = WireFormat::Float32;
        return true;
    case ASIOSTFloat64LSB:
        out = WireFormat::Float64;
        return true;
    case ASIOSTInt32LSB16:
        out = WireFormat::Int32In16;
        return true;
    case ASIOSTInt32LSB18:
        out = WireFormat::Int32In18;
        return true;
    case ASIOSTInt32LSB20:
        out = WireFormat::Int32In20;
        return true;
    case ASIOSTInt32LSB24:
        out = WireFormat::Int32In24;
        return true;
    default:
        return false; // big-endian, DSD
    }
}
// The ASIO callbacks carry no user pointer, so one stream per process is reached through this (the engine
// owns exactly one backend, as for the miniaudio path).
struct Stream {
    IAudioCallback* callback = nullptr;
    std::atomic<bool>* lost = nullptr;
    uint32_t sampleRate = 0;
    uint32_t bufferSize = 0;
    int inChannels = 0;  // 0 (no capture), 1 or 2 driver channels in use
    int outChannels = 0; // 1 or 2
    ASIOBufferInfo infos[4]{};
    WireFormat inType[2]{WireFormat::Float32, WireFormat::Float32};
    WireFormat outType[2]{WireFormat::Float32, WireFormat::Float32};
    std::vector<float> inBuf;  // interleaved stereo
    std::vector<float> outBuf; // interleaved stereo
    bool outputReady = false;
    IASIO* driver = nullptr;
    // Why the driver asked for a reopen (for the log): 0 none, 1 reset request, 2 sample rate change.
    std::atomic<int> lostReason{0};
    std::atomic<double> lostValue{0.0};
};
std::atomic<Stream*> gStream{nullptr}; // set while a stream is open; read on the driver's threads

void onBufferSwitch(long index, ASIOBool) {
    Stream* s = gStream.load(std::memory_order_acquire);
    if (!s || !s->callback)
        return;
    const uint32_t n = s->bufferSize;
    const float* in = nullptr;
    if (s->inChannels > 0) {
        for (int c = 0; c < s->inChannels; ++c)
            readWire(s->inType[c], s->infos[c].buffers[index], s->inBuf.data() + c, n, 2);
        if (s->inChannels == 1) // a mono input feeds both sides
            for (uint32_t i = 0; i < n; ++i)
                s->inBuf[static_cast<size_t>(i) * 2 + 1] = s->inBuf[static_cast<size_t>(i) * 2];
        in = s->inBuf.data();
    }
    s->callback->onAudio(in, s->outBuf.data(), n);
    const int firstOut = s->inChannels;
    if (s->outChannels == 1) { // a mono output carries the mix of both sides
        float* b = s->outBuf.data();
        for (uint32_t i = 0; i < n; ++i)
            b[i * 2] = 0.5f * (b[i * 2] + b[i * 2 + 1]);
    }
    for (int c = 0; c < s->outChannels; ++c)
        writeWire(s->outType[c], s->infos[firstOut + c].buffers[index], s->outBuf.data() + c, n, 2);
    if (s->outputReady && s->driver)
        s->driver->outputReady();
}

ASIOTime* onBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool direct) {
    onBufferSwitch(index, direct);
    return t;
}

void onSampleRateChanged(ASIOSampleRate rate) {
    Stream* s = gStream.load(std::memory_order_acquire);
    if (s && s->lost && static_cast<uint32_t>(rate + 0.5) != s->sampleRate) {
        s->lostValue.store(rate, std::memory_order_relaxed);
        s->lostReason.store(2, std::memory_order_relaxed);
        s->lost->store(true, std::memory_order_relaxed); // the engine runs at the old rate: reopen
    }
}

long onAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        switch (value) {
        case kAsioEngineVersion:
        case kAsioResetRequest:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
        case kAsioOverload:
            return 1;
        default:
            return 0;
        }
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest: // the driver wants to be reopened (buffer size, rate or device change)
        if (Stream* s = gStream.load(std::memory_order_acquire); s && s->lost) {
            s->lostReason.store(1, std::memory_order_relaxed);
            s->lost->store(true, std::memory_order_relaxed);
        }
        return 1;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
    case kAsioOverload:
        return 1;
    default:
        return 0;
    }
}

ASIOCallbacks gCallbacks = {onBufferSwitch, onSampleRateChanged, onAsioMessage, onBufferSwitchTimeInfo};

} // namespace

struct AsioBackend::Impl {
    Stream stream;
    bool comInit = false;
    bool buffersCreated = false;
    bool started = false;
    std::string description;
};

AsioBackend::AsioBackend() : impl_(std::make_unique<Impl>()) {}

AsioBackend::~AsioBackend() {
    close();
}

bool AsioBackend::enumerate(std::vector<DeviceInfo>& out, std::string&) {
    bool first = true;
    for (const DriverEntry& d : listDrivers()) {
        out.push_back({d.id, d.name, first, false});
        first = false;
    }
    return true; // no drivers is not an error
}

namespace {

// ---- the steps of opening a driver. Each returns false with `error` set and leaves cleanup to the caller
// (AsioBackend::open closes everything once on any failure). ----

const DriverEntry* chooseDriver(const std::vector<DriverEntry>& drivers, const std::string& id,
                                std::string& error) {
    if (drivers.empty()) {
        error = "No ASIO drivers are installed.";
        return nullptr;
    }
    if (id.empty())
        return &drivers.front();
    for (const DriverEntry& d : drivers)
        if (d.id == id)
            return &d;
    error = "The selected ASIO driver is not installed.";
    return nullptr;
}

bool createDriver(Stream& s, const DriverEntry& entry, std::string& error) {
    void* raw = nullptr;
    if (FAILED(CoCreateInstance(entry.clsid, nullptr, CLSCTX_INPROC_SERVER, entry.clsid, &raw)) || !raw) {
        error = "Could not load the ASIO driver \"" + entry.name + "\".";
        return false;
    }
    s.driver = static_cast<IASIO*>(raw);
    if (!s.driver->init(GetDesktopWindow())) {
        char msg[256] = {};
        s.driver->getErrorMessage(msg);
        error = "The ASIO driver \"" + entry.name + "\" could not start" +
                (msg[0] ? std::string(": ") + msg : std::string(".")) + " Close other programs that use it.";
        return false;
    }
    return true;
}

// Channel counts, sample rate and buffer size. The rate is the configured one when the driver accepts it,
// otherwise the one it runs at (the engine follows sampleRate()).
bool negotiate(Stream& s, const AudioConfig& config, long& nIn, long& nOut, long (&range)[2],
               std::string& error) {
    if (s.driver->getChannels(&nIn, &nOut) != ASE_OK || nOut < 1) {
        error = "The ASIO driver has no output channels.";
        return false;
    }
    s.outChannels = static_cast<int>(std::min<long>(2, nOut));
    s.inChannels = config.capture ? static_cast<int>(std::min<long>(2, nIn)) : 0;
    if (config.capture && s.inChannels == 0) {
        error = "The ASIO driver has no input channels.";
        return false;
    }

    if (config.sampleRate &&
        s.driver->canSampleRate(static_cast<ASIOSampleRate>(config.sampleRate)) == ASE_OK)
        s.driver->setSampleRate(static_cast<ASIOSampleRate>(config.sampleRate));
    ASIOSampleRate rate = 0;
    if (s.driver->getSampleRate(&rate) != ASE_OK || rate < 8000.0) {
        error = "The ASIO driver reports no usable sample rate (is the device connected and clocked?).";
        return false;
    }
    s.sampleRate = static_cast<uint32_t>(rate + 0.5);

    long preferred = 0, granularity = 0;
    if (s.driver->getBufferSize(&range[0], &range[1], &preferred, &granularity) != ASE_OK) {
        error = "The ASIO driver did not report its buffer sizes.";
        return false;
    }
    const long size = pickAsioBufferSize(static_cast<long>(config.bufferFrames), range[0], range[1],
                                         preferred, granularity);
    if (size <= 0) {
        error = "The ASIO driver reports an unusable buffer size.";
        return false;
    }
    s.bufferSize = static_cast<uint32_t>(size);
    return true;
}

// Inputs first, then outputs; checks each channel's sample format. Returns the channel count, 0 on failure.
long describeChannels(Stream& s, std::string& error) {
    long count = 0;
    for (int c = 0; c < s.inChannels; ++c, ++count) {
        s.infos[count].isInput = ASIOTrue;
        s.infos[count].channelNum = c;
    }
    for (int c = 0; c < s.outChannels; ++c, ++count) {
        s.infos[count].isInput = ASIOFalse;
        s.infos[count].channelNum = c;
    }
    for (long i = 0; i < count; ++i) {
        ASIOChannelInfo ci{};
        ci.channel = s.infos[i].channelNum;
        ci.isInput = s.infos[i].isInput;
        WireFormat wf = WireFormat::Float32;
        if (s.driver->getChannelInfo(&ci) != ASE_OK || !toWire(ci.type, wf)) {
            error = "The ASIO driver uses a sample format that is not supported (type " +
                    std::to_string(ci.type) + ").";
            return 0;
        }
        (ci.isInput ? s.inType : s.outType)[ci.channel] = wf;
    }
    return count;
}

} // namespace

bool AsioBackend::openDriver(const AudioConfig& config, IAudioCallback* callback, std::string& error) {
    Stream& s = impl_->stream;
    lost_.store(false);
    s.lostReason.store(0);

    const std::vector<DriverEntry> drivers = listDrivers();
    const DriverEntry* entry = chooseDriver(drivers, config.outputDeviceId, error);
    if (!entry)
        return false;

    // RPC_E_CHANGED_MODE: the thread already has another apartment; use it and do not uninitialise it.
    impl_->comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (!createDriver(s, *entry, error))
        return false;
    char driverName[64] = {};
    s.driver->getDriverName(driverName);

    long nIn = 0, nOut = 0, range[2] = {0, 0};
    if (!negotiate(s, config, nIn, nOut, range, error))
        return false;
    const long count = describeChannels(s, error);
    if (count == 0)
        return false;

    s.inBuf.assign(static_cast<size_t>(s.bufferSize) * 2, 0.0f);
    s.outBuf.assign(static_cast<size_t>(s.bufferSize) * 2, 0.0f);
    s.callback = callback;
    s.lost = &lost_;
    gStream.store(&s, std::memory_order_release);
    if (s.driver->createBuffers(s.infos, count, static_cast<long>(s.bufferSize), &gCallbacks) != ASE_OK) {
        error = "The ASIO driver refused to create its buffers (is another program using it?).";
        return false;
    }
    impl_->buffersCreated = true;
    s.outputReady = s.driver->outputReady() == ASE_OK;

    long inLat = 0, outLat = 0;
    s.driver->getLatencies(&inLat, &outLat);
    sampleRate_ = s.sampleRate;
    bufferFrames_ = s.bufferSize;
    latencyFrames_ =
        static_cast<uint32_t>(std::max<long>(0, outLat) + (s.inChannels ? std::max<long>(0, inLat) : 0));
    if (latencyFrames_ < bufferFrames_)
        latencyFrames_ = bufferFrames_; // a driver that reports 0 still buffers one block

    impl_->description =
        std::string("ASIO driver \"") + driverName + "\" v" + std::to_string(s.driver->getDriverVersion()) +
        ", " + std::to_string(nIn) + " in / " + std::to_string(nOut) + " out channels, buffer " +
        std::to_string(s.bufferSize) + " (driver range " + std::to_string(range[0]) + "-" +
        std::to_string(range[1]) + "), " + std::to_string(s.sampleRate) + " Hz, reported latency " +
        std::to_string(inLat) + " in / " + std::to_string(outLat) + " out";
    return true;
}

bool AsioBackend::open(const AudioConfig& config, IAudioCallback* callback, std::string& error) {
    close();
    if (!callback) {
        error = "no callback";
        return false;
    }
    if (openDriver(config, callback, error))
        return true;
    close(); // one cleanup for every failure path above
    return false;
}

bool AsioBackend::start(std::string& error) {
    Stream& s = impl_->stream;
    if (!s.driver || !impl_->buffersCreated) {
        error = "ASIO stream not open";
        return false;
    }
    if (impl_->started)
        return true;
    if (s.driver->start() != ASE_OK) {
        error = "The ASIO driver could not start streaming.";
        return false;
    }
    impl_->started = true;
    return true;
}

void AsioBackend::stop() {
    Stream& s = impl_->stream;
    if (impl_->started && s.driver)
        s.driver->stop();
    impl_->started = false;
}

void AsioBackend::close() {
    Stream& s = impl_->stream;
    stop(); // the driver stops calling us before this returns
    Stream* expected = &s;
    gStream.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel); // late callbacks see null
    if (s.driver) {
        if (impl_->buffersCreated)
            s.driver->disposeBuffers();
        impl_->buffersCreated = false;
        s.driver->Release();
        s.driver = nullptr;
    }
    s.callback = nullptr;
    if (impl_->comInit) {
        CoUninitialize();
        impl_->comInit = false;
    }
    sampleRate_ = bufferFrames_ = latencyFrames_ = 0;
}

std::string AsioBackend::describe() const {
    std::string d = impl_->description;
    switch (impl_->stream.lostReason.load(std::memory_order_relaxed)) {
    case 1:
        d += "; the driver asked for a reset";
        break;
    case 2:
        d += "; the driver changed its sample rate to " +
             std::to_string(impl_->stream.lostValue.load(std::memory_order_relaxed)) + " Hz";
        break;
    default:
        break;
    }
    return d;
}

bool AsioBackend::openControlPanel() {
    IASIO* d = impl_->stream.driver;
    return d && d->controlPanel() == ASE_OK;
}

} // namespace dsptb
