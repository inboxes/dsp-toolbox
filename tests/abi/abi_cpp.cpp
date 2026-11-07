// The ABI header must compile as C++ (and the C++ SDK on top of it), with the layout asserts holding.
#include <dsptb/plugin_abi.h>
#include <dsptb/sdk.hpp>

namespace {
struct Dummy final : dsptb::Processor<Dummy> {
    static constexpr dsptb::ModuleInfo info{.id = "test.dummy", .name = "Dummy"};
    static constexpr auto params = dsptb::noParams();
    void process(dsptb::ProcessBlock&) override {}
};
} // namespace

uint32_t dsptbAbiCppCheck() {
    return dsptb::detail::descriptorOf<Dummy>()->abi_version;
}
