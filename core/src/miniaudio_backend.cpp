#include "dsptb/core/miniaudio_backend.hpp"

#include "dsptb/core/duplex_bridge.hpp"

#include <miniaudio.h>

#include <Windows.h>

#include <audioclient.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dsptb {

namespace {

// The smallest shared-mode period Windows 10+ offers this device (IAudioClient3), for Audio Settings. 0 on older
// systems and drivers without it.
uint32_t sharedModeMinPeriod(void* audioClient) {
    if (!audioClient)
        return 0;
    IAudioClient3* client = nullptr;
    if (FAILED(static_cast<IAudioClient*>(audioClient)->QueryInterface(__uuidof(IAudioClient3),
                                                                       reinterpret_cast<void**>(&client))))
        return 0;
    uint32_t result = 0;
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix))) {
        UINT32 defaultPeriod = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
        if (SUCCEEDED(client->GetSharedModeEnginePeriod(mix, &defaultPeriod, &fundamental, &minPeriod, &maxPeriod)))
            result = minPeriod;
        CoTaskMemFree(mix);
    }
    client->Release();
    return result;
}

std::string idToHex(const ma_device_id& id) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    for (size_t i = 0; i < sizeof(ma_device_id); ++i) {
        s.push_back(kHex[bytes[i] >> 4]);
        s.push_back(kHex[bytes[i] & 15]);
    }
    return s;
}

bool hexToId(const std::string& hex, ma_device_id& out) {
    if (hex.size() != sizeof(ma_device_id) * 2)
        return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (size_t i = 0; i < sizeof(ma_device_id); ++i) {
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes[i] = static_cast<unsigned char>(hi * 16 + lo);
    }
    return true;
}

bool initContext(ma_context& ctx, std::string& error) {
    const ma_backend backends[] = {ma_backend_wasapi};
    const ma_result r = ma_context_init(backends, 1, nullptr, &ctx);
    if (r != MA_SUCCESS) {
        error = std::string("audio system unavailable: ") + ma_result_description(r);
        return false;
    }
    return true;
}

} // namespace

struct MiniaudioBackend::Impl {
    ma_context context{};
    bool contextInit = false;
    ma_device device{}; // playback, or the native duplex device
    bool deviceInit = false;

    // Split duplex (WASAPI exclusive + microphone): the capture device is separate and a DuplexBridge joins
    // the two streams, because they run on independent hardware clocks.
    ma_device captureDevice{};
    bool captureInit = false;
    bool split = false;
    DuplexBridge bridge;
    std::vector<float> pullScratch;
};

MiniaudioBackend::MiniaudioBackend() : impl_(std::make_unique<Impl>()) {}

MiniaudioBackend::~MiniaudioBackend() {
    close();
}

bool MiniaudioBackend::enumerate(std::vector<DeviceInfo>& out, std::string& error) {
    ma_context ctx;
    if (!initContext(ctx, error))
        return false;
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playbackCount = 0, captureCount = 0;
    const ma_result r = ma_context_get_devices(&ctx, &playback, &playbackCount, &capture, &captureCount);
    if (r == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < playbackCount; ++i)
            out.push_back({idToHex(playback[i].id), playback[i].name, playback[i].isDefault != 0, false});
        for (ma_uint32 i = 0; i < captureCount; ++i)
            out.push_back({idToHex(capture[i].id), capture[i].name, capture[i].isDefault != 0, true});
    } else {
        error = std::string("cannot list devices: ") + ma_result_description(r);
    }
    ma_context_uninit(&ctx);
    return r == MA_SUCCESS;
}

// The notification callback needs the backend, but ma_device only carries one user pointer (the engine
// callback). Keep a tiny trampoline struct alive for the stream's lifetime instead.
namespace {
struct StreamContext {
    IAudioCallback* callback = nullptr;
    std::atomic<bool>* lost = nullptr;
    std::atomic<bool>* stopping = nullptr;
    // split duplex only
    DuplexBridge* bridge = nullptr;
    float* scratch = nullptr;
    uint32_t scratchFrames = 0;
};

int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
StreamContext gStream; // one stream per process; the engine owns exactly one backend

void audioCallbackTrampoline(ma_device*, void* output, const void* input, ma_uint32 frames) {
    gStream.callback->onAudio(static_cast<const float*>(input), static_cast<float*>(output), frames);
}

// Split duplex: the playback device's callback pulls microphone frames out of the bridge (drift-compensated)
// and runs the engine; the capture device's callback only feeds the bridge.
void splitPlaybackTrampoline(ma_device*, void* output, const void*, ma_uint32 frames) {
    auto* out = static_cast<float*>(output);
    for (ma_uint32 done = 0; done < frames;) {
        const uint32_t n = std::min<uint32_t>(frames - done, gStream.scratchFrames);
        gStream.bridge->pull(gStream.scratch, n, nowNs());
        gStream.callback->onAudio(gStream.scratch, out + static_cast<size_t>(done) * 2, n);
        done += n;
    }
}

void splitCaptureTrampoline(ma_device*, void*, const void* input, ma_uint32 frames) {
    gStream.bridge->pushCapture(static_cast<const float*>(input), frames, nowNs());
}

void notificationTrampoline(const ma_device_notification* n) {
    if (n->type == ma_device_notification_type_stopped && gStream.lost && gStream.stopping &&
        !gStream.stopping->load(std::memory_order_relaxed))
        gStream.lost->store(true, std::memory_order_relaxed);
}
} // namespace

bool MiniaudioBackend::open(const AudioConfig& config, IAudioCallback* callback, std::string& error) {
    close();
    if (!callback) {
        error = "no callback";
        return false;
    }
    if (!initContext(impl_->context, error))
        return false;
    impl_->contextInit = true;

    const bool exclusive = config.backend == BackendKind::WasapiExclusive;
    // Exclusive mode with a microphone runs two devices on two clocks: see DuplexBridge.
    impl_->split = exclusive && config.capture;
    ma_device_config dc = ma_device_config_init(
        impl_->split ? ma_device_type_playback
                     : (config.capture ? ma_device_type_duplex : ma_device_type_playback));
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.playback.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    dc.capture.format = ma_format_f32;
    dc.capture.channels = 2; // a mono microphone is duplicated to both channels by miniaudio
    dc.capture.shareMode = dc.playback.shareMode;
    // Shared mode runs at the device's mix rate (the engine adapts); exclusive mode needs a rate it supports.
    dc.sampleRate = exclusive ? config.sampleRate : 0;
    dc.periodSizeInFrames = config.bufferFrames;
    dc.periods = 2;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noClip = MA_TRUE;               // the engine's safety stage clips
    dc.noFixedSizedCallback = MA_TRUE; // let the callback size vary: the engine handles any size
    dc.dataCallback = impl_->split ? splitPlaybackTrampoline : audioCallbackTrampoline;
    dc.notificationCallback = notificationTrampoline;
    dc.pUserData = callback;

    ma_device_id outId{}, inId{};
    if (!config.outputDeviceId.empty()) {
        if (!hexToId(config.outputDeviceId, outId)) {
            error = "unknown output device id";
            close();
            return false;
        }
        dc.playback.pDeviceID = &outId;
    }
    if (!config.inputDeviceId.empty()) {
        if (!hexToId(config.inputDeviceId, inId)) {
            error = "unknown input device id";
            close();
            return false;
        }
        dc.capture.pDeviceID = &inId;
    }

    callback_ = callback;
    lost_ = false;
    stopping_ = false;
    gStream = {callback, &lost_, &stopping_, nullptr, nullptr, 0};

    if (impl_->split) {
        ma_device_config cc = ma_device_config_init(ma_device_type_capture);
        cc.capture = dc.capture;
        cc.sampleRate = dc.sampleRate;
        cc.periodSizeInFrames = dc.periodSizeInFrames;
        cc.periods = dc.periods;
        cc.performanceProfile = dc.performanceProfile;
        cc.noClip = MA_TRUE;
        cc.noFixedSizedCallback = MA_TRUE;
        cc.dataCallback = splitCaptureTrampoline;
        cc.notificationCallback = notificationTrampoline;
        cc.pUserData = callback;
        const ma_result cr = ma_device_init(&impl_->context, &cc, &impl_->captureDevice);
        if (cr != MA_SUCCESS) {
            error = std::string("The microphone could not be opened in WASAPI exclusive mode: ") +
                    ma_result_description(cr);
            close();
            return false;
        }
        impl_->captureInit = true;
    }

    const ma_result r = ma_device_init(&impl_->context, &dc, &impl_->device);
    if (r != MA_SUCCESS) {
        error = std::string(exclusive ? "WASAPI exclusive mode could not be opened: "
                                      : "audio device could not be opened: ") +
                ma_result_description(r);
        close();
        return false;
    }
    impl_->deviceInit = true;

    sampleRate_ = impl_->device.sampleRate;
    minPeriodFrames_ = exclusive ? 0 : sharedModeMinPeriod(reinterpret_cast<void*>(impl_->device.wasapi.pAudioClientPlayback));
    const ma_uint32 period = impl_->device.playback.internalPeriodSizeInFrames;
    const ma_uint32 periods = impl_->device.playback.internalPeriods;
    // Report the period the device really uses: shared mode often raises a small request to the engine's
    // period.
    bufferFrames_ = period;
    uint64_t latency = static_cast<uint64_t>(period) * periods;
    if (impl_->split) {
        const ma_device& cap = impl_->captureDevice;
        if (cap.sampleRate != impl_->device.sampleRate) {
            error = "the microphone and the output device could not agree on a sample rate in exclusive mode";
            close();
            return false;
        }
        latency +=
            static_cast<uint64_t>(cap.capture.internalPeriodSizeInFrames) * cap.capture.internalPeriods;
        // Devices that deliver in bursts stall for a few capture periods; start with a cushion for that and let the
        // bridge shrink it if the hardware turns out to be smooth.
        const double capPeriodMs = 1000.0 * cap.capture.internalPeriodSizeInFrames / static_cast<double>(sampleRate_);
        impl_->bridge.prepare(8192, cap.capture.internalPeriodSizeInFrames, period, sampleRate_, std::max(10.0, 4.5 * capPeriodMs));
        impl_->pullScratch.assign(4096 * 2, 0.0f);
        gStream.bridge = &impl_->bridge;
        gStream.scratch = impl_->pullScratch.data();
        gStream.scratchFrames = 4096;
        latency += impl_->bridge.targetFrames();
        baseLatencyFrames_ = static_cast<uint32_t>(latency - impl_->bridge.targetFrames());
    } else if (config.capture) {
        latency += static_cast<uint64_t>(impl_->device.capture.internalPeriodSizeInFrames) *
                   impl_->device.capture.internalPeriods;
    }
    // Internal frames are at the device's native rate; express them at the stream rate.
    const ma_uint32 internalRate =
        impl_->device.playback.internalSampleRate ? impl_->device.playback.internalSampleRate : sampleRate_;
    latencyFrames_ = static_cast<uint32_t>(latency * sampleRate_ / internalRate);
    return true;
}

bool MiniaudioBackend::start(std::string& error) {
    if (!impl_->deviceInit) {
        error = "not open";
        return false;
    }
    if (impl_->split) { // fill the bridge before playback begins pulling from it
        const ma_result cr = ma_device_start(&impl_->captureDevice);
        if (cr != MA_SUCCESS) {
            error = std::string("could not start the microphone: ") + ma_result_description(cr);
            return false;
        }
    }
    const ma_result r = ma_device_start(&impl_->device);
    if (r != MA_SUCCESS) {
        error = std::string("could not start the audio stream: ") + ma_result_description(r);
        if (impl_->split)
            ma_device_stop(&impl_->captureDevice);
        return false;
    }
    return true;
}

void MiniaudioBackend::stop() {
    if (!impl_->deviceInit)
        return;
    stopping_ = true;
    ma_device_stop(&impl_->device);
    if (impl_->captureInit)
        ma_device_stop(&impl_->captureDevice);
    stopping_ = false;
}

uint32_t MiniaudioBackend::latencyFrames() const {
    if (impl_->split)
        return baseLatencyFrames_ + impl_->bridge.targetFrames();
    return latencyFrames_;
}

uint32_t MiniaudioBackend::cushionFrames() const {
    return impl_->split ? impl_->bridge.targetFrames() : 0;
}

std::string MiniaudioBackend::describe() const {
    if (!impl_->deviceInit)
        return {};
    const ma_device& d = impl_->device;
    char buf[900];
    if (impl_->split) {
        const ma_device& c = impl_->captureDevice;
        std::snprintf(buf, sizeof buf,
                      "split duplex (two devices + drift-compensating bridge): playback %u Hz, period %u "
                      "frames x %u; capture %u Hz, "
                      "period %u frames x %u; bridge cushion %u frames (peak capture gap %.2f ms), fill %u, "
                      "underflows %llu, overflow frames %llu, clock drift %.1f ppm, max capture gap %.2f ms, "
                      "max playback gap %.2f ms, min fill at pull %u; capture gaps(<1,1-2,2-3.5,3.5-5,5-8,>8 "
                      "ms)=%u/%u/%u/%u/%u/%u, capture block %u..%u",
                      d.sampleRate, d.playback.internalPeriodSizeInFrames, d.playback.internalPeriods,
                      c.sampleRate, c.capture.internalPeriodSizeInFrames, c.capture.internalPeriods,
                      impl_->bridge.targetFrames(), static_cast<double>(impl_->bridge.peakGapMs()),
                      impl_->bridge.fillFrames(), static_cast<unsigned long long>(impl_->bridge.underflows()),
                      static_cast<unsigned long long>(impl_->bridge.overflowFrames()),
                      static_cast<double>(impl_->bridge.driftEstimatePpm()), impl_->bridge.maxPushGapMs(),
                      impl_->bridge.maxPullGapMs(), impl_->bridge.minAvailFrames(),
                      impl_->bridge.gapBucket(0), impl_->bridge.gapBucket(1), impl_->bridge.gapBucket(2),
                      impl_->bridge.gapBucket(3), impl_->bridge.gapBucket(4), impl_->bridge.gapBucket(5),
                      impl_->bridge.minCaptureBlock(), impl_->bridge.maxCaptureBlock());
        return buf;
    }
    std::snprintf(buf, sizeof buf,
                  "playback: %u Hz, period %u frames x %u (internal rate %u); capture: %u Hz, period %u "
                  "frames x %u (internal rate %u)",
                  d.sampleRate, d.playback.internalPeriodSizeInFrames, d.playback.internalPeriods,
                  d.playback.internalSampleRate, d.sampleRate, d.capture.internalPeriodSizeInFrames,
                  d.capture.internalPeriods, d.capture.internalSampleRate);
    return buf;
}

void MiniaudioBackend::close() {
    if (impl_->captureInit) {
        stopping_ = true;
        ma_device_uninit(&impl_->captureDevice);
        impl_->captureInit = false;
        stopping_ = false;
    }
    if (impl_->deviceInit) {
        stopping_ = true;
        ma_device_uninit(&impl_->device);
        impl_->deviceInit = false;
        stopping_ = false;
    }
    impl_->split = false;
    if (impl_->contextInit) {
        ma_context_uninit(&impl_->context);
        impl_->contextInit = false;
    }
    callback_ = nullptr;
    gStream = {};
}

} // namespace dsptb
