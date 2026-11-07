// Module system unit tests that need no DLL: manifest parsing, descriptor validation, parameter storage, the
// recording canvas, the SEH trampoline and session description parsing.
#include <doctest/doctest.h>

#include <dsptb/core/module_instance.hpp>
#include <dsptb/core/module_manifest.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/offline_render.hpp>
#include <dsptb/core/recording_canvas.hpp>
#include <dsptb/core/seh_guard.hpp>
#include <dsptb/sdk.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace dsptb;
namespace fs = std::filesystem;

namespace {

const char* kGoodManifest = R"({
  "schema": 1, "id": "org.dsptb.gain", "name": "Gain", "version": "0.1.0", "abi": 1, "dll": "gain.dll",
  "modules": [{"id": "org.dsptb.gain", "kind": "processor"}], "author": "x", "license": "MIT"})";

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("dsptb_test_" + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

dsptb_module_desc goodDesc() {
    static const dsptb_param_desc kParam = {sizeof(dsptb_param_desc),
                                            "gain_db",
                                            "Gain",
                                            "dB",
                                            DSPTB_PARAM_FLOAT,
                                            -60.0f,
                                            24.0f,
                                            0.0f,
                                            20.0f,
                                            0u,
                                            nullptr,
                                            0u};
    dsptb_module_desc d{};
    d.struct_size = sizeof(d);
    d.abi_version = DSPTB_ABI_VERSION;
    d.id = "org.test.x";
    d.name = "X";
    d.version = "1.0.0";
    d.vendor = "v";
    d.description = "d";
    d.kind = DSPTB_KIND_PROCESSOR;
    d.params = &kParam;
    d.num_params = 1;
    d.create = [](const dsptb_host_api*) -> void* { return nullptr; };
    d.destroy = [](void*) {};
    d.prepare = [](void*, const dsptb_prepare_info*) { return 0; };
    d.reset = [](void*) {};
    d.latency_frames = [](void*) { return 0u; };
    d.process = [](void*, const dsptb_process_ctx*) {};
    return d;
}

} // namespace

TEST_CASE("manifest: a valid module.json parses") {
    ModuleManifest m;
    std::string err;
    REQUIRE(parseManifest(kGoodManifest, m, err));
    CHECK(m.id == "org.dsptb.gain");
    CHECK(m.abi == 1);
    CHECK(m.dll == "gain.dll");
    REQUIRE(m.modules.size() == 1);
    CHECK(m.modules[0].kind == "processor");
}

TEST_CASE("manifest: bad input is rejected with a reason") {
    ModuleManifest m;
    std::string err;
    CHECK_FALSE(parseManifest("not json", m, err));
    CHECK_FALSE(parseManifest(R"({"schema":2})", m, err));
    CHECK_FALSE(parseManifest(
        R"({"schema":1,"id":"Bad Id","version":"1","abi":1,"dll":"a.dll","modules":[{"id":"a","kind":"processor"}]})",
        m, err));
    // a DLL name must not be a path
    CHECK_FALSE(parseManifest(
        R"({"schema":1,"id":"a.b","version":"1","abi":1,"dll":"..\\evil.dll","modules":[{"id":"a.b","kind":"processor"}]})",
        m, err));
    CHECK_FALSE(parseManifest(
        R"({"schema":1,"id":"a.b","version":"1","abi":1,"dll":"sub/x.dll","modules":[{"id":"a.b","kind":"processor"}]})",
        m, err));
    // unknown kind
    CHECK_FALSE(parseManifest(
        R"({"schema":1,"id":"a.b","version":"1","abi":1,"dll":"x.dll","modules":[{"id":"a.b","kind":"widget"}]})",
        m, err));
    CHECK(err.find("kind") != std::string::npos);
    // empty modules list
    CHECK_FALSE(
        parseManifest(R"({"schema":1,"id":"a.b","version":"1","abi":1,"dll":"x.dll","modules":[]})", m, err));
}

TEST_CASE("manifest: ids and versions") {
    CHECK(isValidModuleId("org.dsptb.spatializer"));
    CHECK(isValidModuleId("me.my-filter2"));
    CHECK_FALSE(isValidModuleId(""));
    CHECK_FALSE(isValidModuleId("Org.Upper"));
    CHECK_FALSE(isValidModuleId(".leading"));
    CHECK_FALSE(isValidModuleId("trailing."));
    CHECK_FALSE(isValidModuleId("has space"));
    CHECK(compareVersions("0.1.0", "0.1.0") == 0);
    CHECK(compareVersions("0.2.0", "0.1.9") > 0);
    CHECK(compareVersions("1.0.0", "1.0.1") < 0);
    CHECK(compareVersions("1.10.0", "1.9.0") > 0); // numeric, not lexical
}

TEST_CASE("registry: a package built for another ABI is listed as Incompatible ABI and never loaded") {
    const fs::path root = tempDir("abi");
    writeFile(
        root / "p1" / "module.json",
        R"({"schema":1,"id":"test.future","version":"1.0.0","abi":2,"dll":"future.dll","modules":[{"id":"test.future","kind":"processor"}]})");
    ModuleRegistry reg;
    REQUIRE(reg.scan({{root, false}}) == 1);
    ModulePackage* p = reg.findPackage("test.future");
    REQUIRE(p != nullptr);
    CHECK(p->status == PackageStatus::IncompatibleAbi);
    CHECK_FALSE(reg.ensureLoaded(*p));
    CHECK_FALSE(p->loaded);
    std::string err;
    CHECK(reg.descriptor("test.future", nullptr, err) == nullptr);
    CHECK(err.find("Incompatible ABI") != std::string::npos);
}

TEST_CASE(
    "registry: a package with a missing DLL is Failed to load; a folder without module.json is ignored") {
    const fs::path root = tempDir("missingdll");
    writeFile(root / "p1" / "module.json", kGoodManifest);
    fs::create_directories(root / "not-a-module");
    writeFile(root / "broken" / "module.json", "{ nope");
    ModuleRegistry reg;
    CHECK(reg.scan({{root, false}}) == 1);
    ModulePackage* p = reg.findPackage("org.dsptb.gain");
    REQUIRE(p != nullptr);
    CHECK(p->status == PackageStatus::FailedToLoad);
    CHECK(reg.packages().size() == 1);
}

TEST_CASE("registry: duplicate ids keep the higher version, then the user folder over the bundled one") {
    const auto manifest = [](const std::string& version) {
        return std::string(R"({"schema":1,"id":"org.dup","version":")") + version +
               R"(","abi":1,"dll":"dup.dll","modules":[{"id":"org.dup","kind":"processor"}]})";
    };
    {
        const fs::path bundled = tempDir("dup_a"), user = tempDir("dup_b");
        writeFile(bundled / "dup" / "module.json", manifest("0.1.0"));
        writeFile(user / "dup" / "module.json", manifest("0.2.0"));
        ModuleRegistry reg;
        reg.scan({{bundled, false}, {user, true}});
        REQUIRE(reg.packages().size() == 1);
        CHECK(reg.packages()[0]->manifest.version == "0.2.0");
    }
    {
        const fs::path bundled = tempDir("dup_c"), user = tempDir("dup_d");
        writeFile(bundled / "dup" / "module.json", manifest("0.1.0"));
        writeFile(user / "dup" / "module.json", manifest("0.1.0"));
        ModuleRegistry reg;
        reg.scan({{bundled, false}, {user, true}});
        REQUIRE(reg.packages().size() == 1);
        CHECK(reg.packages()[0]->userFolder); // equal versions: the user folder wins
    }
    {
        const fs::path bundled = tempDir("dup_e"), user = tempDir("dup_f");
        writeFile(bundled / "dup" / "module.json", manifest("0.3.0"));
        writeFile(user / "dup" / "module.json", manifest("0.1.0"));
        ModuleRegistry reg;
        reg.scan({{bundled, false}, {user, true}});
        REQUIRE(reg.packages().size() == 1);
        CHECK(reg.packages()[0]->manifest.version == "0.3.0");
    }
}

TEST_CASE("registry: rescan only adds packages it has not seen") {
    const fs::path root = tempDir("rescan");
    writeFile(root / "a" / "module.json", kGoodManifest);
    ModuleRegistry reg;
    CHECK(reg.scan({{root, false}}) == 1);
    CHECK(reg.scan({{root, false}}) == 0);
    writeFile(
        root / "b" / "module.json",
        R"({"schema":1,"id":"org.other","version":"1.0.0","abi":1,"dll":"o.dll","modules":[{"id":"org.other","kind":"processor"}]})");
    CHECK(reg.scan({{root, false}}) == 1);
    CHECK(reg.packages().size() == 2);
}

TEST_CASE("descriptor validation catches the mistakes the validator looks for") {
    CHECK(validateDescriptor(goodDesc()).empty());

    auto d = goodDesc();
    d.process = nullptr;
    CHECK(validateDescriptor(d).find("process") != std::string::npos);

    d = goodDesc();
    d.id = "Bad ID";
    CHECK_FALSE(validateDescriptor(d).empty());

    d = goodDesc();
    d.kind = DSPTB_KIND_ANALYZER; // an analyzer needs analyze
    CHECK(validateDescriptor(d).find("analyze") != std::string::npos);

    const auto withParams = [](const dsptb_param_desc* p, uint32_t n) {
        auto dd = goodDesc();
        dd.params = p;
        dd.num_params = n;
        return validateDescriptor(dd);
    };
    const dsptb_param_desc minMax = {
        sizeof(dsptb_param_desc), "a", "A", "", DSPTB_PARAM_FLOAT, 1.0f, 1.0f, 1.0f, 0.0f, 0u, nullptr, 0u};
    CHECK(withParams(&minMax, 1).find("min") != std::string::npos);
    const dsptb_param_desc badDefault = {
        sizeof(dsptb_param_desc), "a", "A", "", DSPTB_PARAM_FLOAT, 0.0f, 1.0f, 2.0f, 0.0f, 0u, nullptr, 0u};
    CHECK(withParams(&badDefault, 1).find("default") != std::string::npos);
    const dsptb_param_desc camel = {sizeof(dsptb_param_desc),
                                    "gainDb",
                                    "A",
                                    "",
                                    DSPTB_PARAM_FLOAT,
                                    0.0f,
                                    1.0f,
                                    0.0f,
                                    0.0f,
                                    0u,
                                    nullptr,
                                    0u};
    CHECK(withParams(&camel, 1).find("snake_case") != std::string::npos);
    const dsptb_param_desc dup[2] = {
        {sizeof(dsptb_param_desc), "a", "A", "", DSPTB_PARAM_FLOAT, 0.0f, 1.0f, 0.0f, 0.0f, 0u, nullptr, 0u},
        {sizeof(dsptb_param_desc), "a", "B", "", DSPTB_PARAM_FLOAT, 0.0f, 1.0f, 0.0f, 0.0f, 0u, nullptr, 0u}};
    CHECK(withParams(dup, 2).find("duplicate") != std::string::npos);
    static const char* const labels[] = {"one", "two"};
    const dsptb_param_desc badEnum = {
        sizeof(dsptb_param_desc), "e", "E", "", DSPTB_PARAM_ENUM, 0.0f, 5.0f, 0.0f, 0.0f, 0u, labels, 2u};
    CHECK(withParams(&badEnum, 1).find("enum") != std::string::npos);
    const dsptb_param_desc goodEnum = {
        sizeof(dsptb_param_desc), "e", "E", "", DSPTB_PARAM_ENUM, 0.0f, 1.0f, 0.0f, 0.0f, 0u, labels, 2u};
    CHECK(withParams(&goodEnum, 1).empty());
}

TEST_CASE("ParamBlock clamps, rounds discrete types and rejects non-finite values") {
    static constexpr const char* kLabels[] = {"a", "b", "c"};
    static constexpr auto kParams =
        dsptb::params(dsptb::Float("f", "F", "dB", -10.f, 10.f, 1.f), dsptb::Int("i", "I", "", 0, 10, 3),
                      dsptb::Bool("b", "B"), dsptb::Enum("e", "E", kLabels, 1));
    ParamBlock p;
    p.init(kParams.data(), static_cast<uint32_t>(kParams.size()));
    CHECK(p.size() == 4);
    CHECK(p.get(0) == 1.0f);
    CHECK(p.get(1) == 3.0f);
    CHECK(p.get(3) == 1.0f);
    CHECK(p.set(0, 99.0f) == 10.0f);
    CHECK(p.set(0, -99.0f) == -10.0f);
    CHECK(p.set(1, 4.6f) == 5.0f);
    CHECK(p.set(2, 0.7f) == 1.0f);
    CHECK(p.set(3, 9.0f) == 2.0f);
    CHECK(p.set(0, std::numeric_limits<float>::quiet_NaN()) == 1.0f); // falls back to the default
    CHECK(p.indexOf("i") == 1);
    CHECK(p.indexOf("nope") == -1);
    p.set(0, 5.0f);
    p.copyToRt();
    CHECK(p.rtValues()[0] == 5.0f);
    p.resetToDefaults();
    CHECK(p.get(0) == 1.0f);
}

TEST_CASE("SDK parameter builders produce valid descriptors") {
    static constexpr const char* kLabels[] = {"x", "y"};
    static constexpr auto kParams =
        dsptb::params(dsptb::Float("cutoff", "Cutoff", "Hz", 20.f, 20000.f, 1000.f).log().smoothing(10.f),
                      dsptb::Bool("on", "On", true), dsptb::Enum("mode", "Mode", kLabels, 1));
    static_assert(kParams.size() == 3);
    CHECK((kParams[0].flags & DSPTB_PARAM_LOG) != 0);
    CHECK(kParams[0].smoothing_ms == 10.0f);
    CHECK(kParams[1].default_value == 1.0f);
    CHECK(kParams[2].enum_count == 2);
    CHECK(kParams[2].max_value == 1.0f);
}

TEST_CASE("SEH trampoline contains an access violation and a C++ exception") {
    unsigned long code = 0;
    CHECK(guarded([] {}));
    CHECK_FALSE(guarded(
        [] {
            volatile int* p = nullptr;
            *p = 1;
        },
        &code));
    CHECK(code == 0xC0000005UL); // EXCEPTION_ACCESS_VIOLATION
    CHECK_FALSE(guarded([] { throw std::runtime_error("boom"); }, &code));
    CHECK(code == 0xE06D7363UL); // MSVC C++ exception
    CHECK(guarded([] {}));       // and it keeps working afterwards
}

TEST_CASE("RecordingCanvas flags malformed canvas use") {
    RecordingCanvas c;
    std::string problem;
    const dsptb_canvas_api* a = c.api();

    CHECK(a->begin_region(a->ctx, "r", 100, 20) == 1);
    a->rect(a->ctx, {0, 0}, {10, 10}, 0xffffffff, 1.0f, 1);
    a->end_region(a->ctx);
    a->push_id(a->ctx, "x");
    a->pop_id(a->ctx);
    CHECK(c.finishFrame(problem));

    a->begin_region(a->ctx, "r", 100, 20); // never ended
    CHECK_FALSE(c.finishFrame(problem));
    CHECK(problem.find("end_region") != std::string::npos);

    a->line(a->ctx, {0, 0}, {1, 1}, 0xffffffff, 1.0f); // outside a region
    CHECK_FALSE(c.finishFrame(problem));
    CHECK(problem.find("outside a region") != std::string::npos);

    a->pop_id(a->ctx); // unbalanced
    CHECK_FALSE(c.finishFrame(problem));

    const dsptb_texture t = a->texture_create(a->ctx, 4, 4, DSPTB_TEX_RGBA8);
    CHECK(t != 0);
    CHECK(c.liveTextures() == 1);
    a->texture_destroy(a->ctx, t);
    CHECK(c.liveTextures() == 0);
}

TEST_CASE("session description: instances, params and booleans parse") {
    SessionDescription d;
    std::string err;
    REQUIRE(parseSessionDescription(
        R"({"format":"dsptb-session","version":1,"audio":{"sampleRate":44100},
            "instances":[{"module":"org.dsptb.gain","enabled":false,"params":{"gain_db":-6.0,"invert":true}}]})",
        d, err));
    CHECK(d.sampleRate == 44100);
    REQUIRE(d.instances.size() == 1);
    CHECK(d.instances[0].module == "org.dsptb.gain");
    CHECK_FALSE(d.instances[0].enabled);
    CHECK(d.instances[0].params.at("gain_db") == doctest::Approx(-6.0));
    CHECK(d.instances[0].params.at("invert") == 1.0f);

    CHECK_FALSE(parseSessionDescription("[]", d, err));
    CHECK_FALSE(parseSessionDescription(R"({"format":"other"})", d, err));
    CHECK_FALSE(parseSessionDescription(R"({"format":"dsptb-session","instances":[{"params":{}}]})", d, err));
    CHECK(parseSessionDescription(R"({"format":"dsptb-session"})", d, err)); // empty session
    CHECK(d.instances.empty());
}
