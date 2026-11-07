// Gain (org.dsptb.gain): the reference module. It doubles as the example in
// MODULE_SDK.md and the basis of tools/module-template/, so keep it small.

#include <dsptb/sdk.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>

DSPTB_DEFINE_RT_GUARD();

class Gain final : public dsptb::Processor<Gain> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "org.dsptb.gain",
                                            .name = "Gain",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox",
                                            .description = "Simple gain with smoothing.",
                                            .caps = DSPTB_CAP_REALTIME | DSPTB_CAP_OFFLINE};
    static constexpr auto params =
        dsptb::params(dsptb::Float("gain_db", "Gain", "dB", -60.f, 24.f, 0.f).smoothing(20.f),
                      dsptb::Bool("invert", "Invert polarity"), dsptb::Bool("mute", "Mute").smoothing(5.f));

    enum Param : uint32_t { kGainDb = 0, kInvert = 1, kMute = 2 };

    bool prepare(const dsptb::PrepareInfo& p) override {
        sampleRate_ = p.sampleRate;
        first_ = true; // the first block starts at its target: no fade-in from silence
        return true;
    }

    void process(dsptb::ProcessBlock& b) override {
        const float target = dsptb::dbToGain(b.param(kGainDb)) * (b.paramBool(kInvert) ? -1.0f : 1.0f);
        const float muteTarget = b.paramBool(kMute) ? 0.0f : 1.0f;
        if (first_) {
            gain_.reset(sampleRate_, 20.0, target);
            mute_.reset(sampleRate_, 5.0, muteTarget);
            first_ = false;
        }
        gain_.setTarget(target);
        mute_.setTarget(muteTarget);
        float g = gain_.current() * mute_.current();
        for (uint32_t i = 0; i < b.frames(); ++i) {
            g = gain_.next() * mute_.next();
            for (float* ch : b.channels())
                ch[i] *= g;
        }
        // For the custom view: the gain actually applied at the end of this block, in dB.
        const float mag = std::fabs(g);
        applied_.store(mag > 1e-3f ? 20.0f * std::log10(mag) : -60.0f, std::memory_order_relaxed);
    }

    void draw(dsptb::Canvas& c) override {
        // A horizontal bar showing the applied gain (-60..+24 dB). It exists to exercise region/rect/text.
        const dsptb_vec2 avail = c.availableSize();
        const float s = c.uiScale();
        const float w = avail.x > 40.0f * s ? avail.x : 40.0f * s;
        const float h = 22.0f * s;
        c.label("Invert polarity flips the waveform upside down (180 deg phase).");
        auto region = c.region("gain_bar", w, h);
        if (!region)
            return;
        const float db = applied_.load(std::memory_order_relaxed);
        const float t = (std::fmin(24.0f, std::fmax(-60.0f, db)) + 60.0f) / 84.0f;
        const float zero = 60.0f / 84.0f;
        c.fillRect({0, 0}, {w, h}, dsptb::rgba(30, 32, 38));
        const uint32_t col = db > 0.0f ? dsptb::rgba(224, 140, 60) : dsptb::rgba(70, 160, 220);
        c.fillRect({0, 0}, {w * t, h}, col);
        c.line({w * zero, 0}, {w * zero, h}, dsptb::rgba(255, 255, 255, 160), 1.0f);
        c.rect({0, 0}, {w, h}, dsptb::rgba(90, 94, 104), 1.0f);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+.1f dB", static_cast<double>(db));
        c.text({6.0f * s, 3.0f * s}, dsptb::rgba(240, 240, 240), buf);
    }

private:
    double sampleRate_ = 48000.0;
    bool first_ = true;
    dsptb::rt::SmoothedValue gain_, mute_;
    std::atomic<float> applied_{0.0f};
};

DSPTB_REGISTER_MODULES(Gain);
