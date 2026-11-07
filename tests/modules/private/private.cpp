// Test fixture: a PRIVATE_MODULE. The package_contents test checks that it never reaches a release package.
#include <dsptb/sdk.hpp>

DSPTB_DEFINE_RT_GUARD();

class PrivateFixture final : public dsptb::Processor<PrivateFixture> {
public:
    static constexpr dsptb::ModuleInfo info{.id = "test.private",
                                            .name = "Private (test)",
                                            .version = "0.1.0",
                                            .vendor = "DSP Toolbox tests",
                                            .description = "Must never be packaged.",
                                            .caps = DSPTB_CAP_REALTIME | DSPTB_CAP_OFFLINE};
    static constexpr auto params = dsptb::noParams();

    void process(dsptb::ProcessBlock&) override {}
};

DSPTB_REGISTER_MODULES(PrivateFixture);
