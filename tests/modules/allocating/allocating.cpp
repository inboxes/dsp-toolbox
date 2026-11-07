// Test fixture: calls `new` inside process() when its "allocate" parameter is on. In a build with the RT
// guard the module's own guard must record it.
#include <dsptb/sdk.hpp>

DSPTB_DEFINE_RT_GUARD();

class Allocating final : public dsptb::Processor<Allocating> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.allocating",
                                            .name = "Allocating (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description = "Test fixture: allocates in process().",
                                            .caps = DSPTB_CAP_REALTIME};
    static constexpr auto params = dsptb::params(dsptb::Bool("allocate", "Allocate", false));

    void process(dsptb::ProcessBlock& b) override {
        if (b.paramBool(0)) {
            volatile int* p = new int[64]; // forbidden on the audio thread
            p[0] = 1;
            delete[] p;
        }
    }
};

DSPTB_REGISTER_MODULES(Allocating);
