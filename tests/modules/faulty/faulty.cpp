// Test fixture: a processor that misbehaves when its "mode" parameter is set.
#include <dsptb/sdk.hpp>

#include <limits>
#include <stdexcept>

DSPTB_DEFINE_RT_GUARD();

class Faulty final : public dsptb::Processor<Faulty> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.faulty",
                                            .name = "Faulty (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description = "Test fixture: misbehaves on request.",
                                            .caps = DSPTB_CAP_REALTIME};
    static constexpr const char* kModes[] = {"None", "Throw", "NaN", "Crash"};
    static constexpr auto params = dsptb::params(dsptb::Enum("mode", "Mode", kModes, 0));

    void process(dsptb::ProcessBlock& b) override {
        switch (b.paramInt(0)) {
        case 1:
            throw std::runtime_error("faulty module: deliberate C++ exception");
        case 2:
            for (float* ch : b.channels())
                for (uint32_t i = 0; i < b.frames(); ++i)
                    ch[i] = std::numeric_limits<float>::quiet_NaN();
            break;
        case 3: {
            // A real access violation (not a C++ exception): only the host's SEH wrapper can catch this.
            volatile int* p = nullptr;
            *p = 1;
            break;
        }
        default:
            break;
        }
    }
};

DSPTB_REGISTER_MODULES(Faulty);
