// The host side of the GPU runtime (docs/GPU_RUNTIME.md section 6.5 and 9.1): the loader, module gating by
// requires.gpuRuntime, `dsptb-cli gpu-info` and `validate --allow-skip`. A fake dsptb_gpu.dll stands in for
// the real one, so none of this needs a GPU. The cases that use the real runtime exist only in GPU builds.
#include <doctest/doctest.h>

#include <dsptb/core/gpu_runtime_loader.hpp>
#include <dsptb/core/module_manifest.hpp>
#include <dsptb/core/module_registry.hpp>
#include <dsptb/core/settings.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace dsptb;

namespace {

struct RunResult {
    int exitCode = -1;
    std::string out;
};

RunResult runCli(const std::string& args) {
    const char* const redirect = args.find("--json") != std::string::npos ? " 2>nul" : " 2>&1";
    const std::string cmd = "\"\"" + std::string(DSPTB_CLI_PATH) + "\" " + args + redirect + "\"";
    RunResult r;
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        r.out += buf;
    r.exitCode = _pclose(pipe);
    return r;
}

// Sets an environment variable for this test case and restores it afterwards. The CLI child inherits it.
struct EnvVar {
    std::string name;
    explicit EnvVar(const char* n, const std::string& value) : name(n) { _putenv_s(n, value.c_str()); }
    ~EnvVar() { _putenv_s(name.c_str(), ""); }
};

// Shuts the process-wide loader down when a case ends, so cases don't see each other's state.
struct LoaderReset {
    ~LoaderReset() { GpuRuntimeLoader::instance().shutdown(); }
};

fs::path emptyDir(const char* name) {
    const fs::path d = fs::temp_directory_path() / (std::string("dsptb_test_gpu_") + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

GpuRuntimeRequest requestFor(const fs::path& dir) {
    GpuRuntimeRequest r;
    r.dir = dir;
    return r;
}

fs::path testModule(const char* id) {
    return fs::path(DSPTB_TEST_MODULES_DIR) / id;
}

ModuleRegistry scanGpuFixtures() {
    ModuleRegistry registry;
    registry.scan({fs::path(DSPTB_TEST_MODULES_DIR)});
    return registry;
}

} // namespace

TEST_CASE("gpu: manifest field requires.gpuRuntime") {
    ModuleManifest m;
    std::string err;
    const std::string base =
        R"({"schema":1,"id":"a.b","version":"1.0.0","abi":1,"dll":"a.dll","modules":[{"id":"a.b","kind":"processor"}])";
    REQUIRE(parseManifest(base + "}", m, err));
    CHECK(m.requiresGpuRuntime == 0);
    CHECK(m.requiresUnknown.empty());
    REQUIRE(parseManifest(base + R"(,"requires":{"gpuRuntime":1}})", m, err));
    CHECK(m.requiresGpuRuntime == 1);
    REQUIRE(parseManifest(base + R"(,"requires":{"gpuRuntime":1,"holography":2}})", m, err));
    CHECK(m.requiresUnknown == "holography");
    CHECK_FALSE(parseManifest(base + R"(,"requires":{"gpuRuntime":0}})", m, err));
    CHECK_FALSE(parseManifest(base + R"(,"requires":{"gpuRuntime":"yes"}})", m, err));
    CHECK_FALSE(parseManifest(base + R"(,"requires":3})", m, err));
}

TEST_CASE("gpu: settings keep the gpu section, with safe defaults") {
    Settings s;
    CHECK(s.gpu.sync == "blocking");
    CHECK(s.gpu.device == 0);
    s.gpu.runtimeDir = "D:/x";
    s.gpu.device = 1;
    s.gpu.sync = "spin";
    Settings back;
    REQUIRE(back.fromJsonString(s.toJsonString()));
    CHECK(back.gpu.runtimeDir == "D:/x");
    CHECK(back.gpu.device == 1);
    CHECK(back.gpu.sync == "spin");
    Settings odd;
    REQUIRE(odd.fromJsonString(R"({"gpu":{"device":-4,"sync":"warp"}})"));
    CHECK(odd.gpu.device == 0);
    CHECK(odd.gpu.sync == "blocking");
}

TEST_CASE("gpu: a missing runtime folder is not an error") {
    LoaderReset reset;
    GpuRuntimeLoader& rt = GpuRuntimeLoader::instance();
    rt.load(requestFor(emptyDir("none")));
    CHECK(rt.state().status == DSPTB_GPU_DLL_MISSING);
    CHECK_FALSE(rt.state().dllFound);
    CHECK_FALSE(rt.available());
    CHECK(rt.state().statusText.find("not installed") != std::string::npos);
    CHECK(rt.devices().empty());
}

TEST_CASE("gpu: the fake runtime reports each status to the loader") {
    LoaderReset reset;
    GpuRuntimeLoader& rt = GpuRuntimeLoader::instance();
    for (int status : {DSPTB_GPU_NO_DRIVER, DSPTB_GPU_NO_DEVICE, DSPTB_GPU_DLL_MISSING,
                       DSPTB_GPU_VERSION_MISMATCH, DSPTB_GPU_INIT_FAILED}) {
        EnvVar env("DSPTB_FAKE_GPU_STATUS", std::to_string(status));
        rt.load(requestFor(DSPTB_FAKE_GPU_DIR));
        INFO("status ", status);
        CHECK(rt.state().dllFound);
        CHECK(rt.state().status == status);
        CHECK_FALSE(rt.available());
        CHECK(rt.state().statusText.find("fake runtime") != std::string::npos);
        rt.shutdown();
    }
    rt.load(requestFor(DSPTB_FAKE_GPU_DIR));
    REQUIRE(rt.available());
    CHECK(rt.state().cudnn == 92400);
    CHECK(rt.state().ortVersion == "1.0.0");
    REQUIRE(rt.devices().size() == 1);
    CHECK(rt.devices()[0].name == "Fake GPU");
    // load() is once only until shutdown()
    rt.load(requestFor(emptyDir("ignored")));
    CHECK(rt.available());
}

TEST_CASE("gpu: modules that need the runtime are gated, others are not") {
    LoaderReset reset;
    GpuRuntimeLoader& rt = GpuRuntimeLoader::instance();

    SUBCASE("without the runtime") {
        rt.load(requestFor(emptyDir("gate-none")));
        ModuleRegistry registry = scanGpuFixtures();
        ModulePackage* gated = registry.findPackage("test.gpu-gated");
        REQUIRE(gated);
        CHECK(gated->status == PackageStatus::NeedsGpuRuntime);
        CHECK(gated->statusText.find("Needs the GPU runtime") == 0);
        CHECK(std::string(statusLabel(gated->status)) == "Needs GPU runtime");
        CHECK_FALSE(registry.ensureLoaded(*gated));
        std::string err;
        CHECK(registry.descriptor("test.gpu-gated", nullptr, err) == nullptr);
        CHECK_FALSE(err.empty());
        ModulePackage* probe = registry.findPackage("test.probe");
        REQUIRE(probe);
        CHECK(probe->status == PackageStatus::Ok); // other modules are unaffected
    }
    SUBCASE("with a working runtime") {
        rt.load(requestFor(DSPTB_FAKE_GPU_DIR));
        REQUIRE(rt.available());
        ModuleRegistry registry = scanGpuFixtures();
        CHECK(registry.findPackage("test.gpu-gated")->status == PackageStatus::Ok);
        ModulePackage* future = registry.findPackage("test.gpu-future-abi");
        REQUIRE(future);
        CHECK(future->status == PackageStatus::NeedsGpuRuntime);
        CHECK(future->statusText.find("ABI 2") != std::string::npos);
    }
    SUBCASE("a requirement the host doesn't know makes the package incompatible") {
        rt.load(requestFor(DSPTB_FAKE_GPU_DIR));
        ModuleRegistry registry = scanGpuFixtures();
        ModulePackage* p = registry.findPackage("test.gpu-unknown-requires");
        REQUIRE(p);
        CHECK(p->status == PackageStatus::Incompatible);
        CHECK(p->statusText.find("holography") != std::string::npos);
        CHECK(std::string(statusLabel(p->status)) == "Incompatible");
    }
}

TEST_CASE("cli: gpu-info without a runtime exits 1 and says why") {
    EnvVar env("DSPTB_GPU_RUNTIME_DIR", emptyDir("cli-none").string());
    const RunResult text = runCli("gpu-info");
    CHECK(text.exitCode == 1);
    CHECK(text.out.find("NOT AVAILABLE") != std::string::npos);
    const RunResult json = runCli("gpu-info --json");
    CHECK(json.exitCode == 1);
    const auto j = nlohmann::json::parse(json.out);
    CHECK(j["status"] == "missing");
    CHECK(j["devices"].empty());
}

TEST_CASE("cli: gpu-info with the fake runtime, and with each failure") {
    EnvVar dir("DSPTB_GPU_RUNTIME_DIR", DSPTB_FAKE_GPU_DIR);
    {
        const RunResult r = runCli("gpu-info --json");
        REQUIRE(r.exitCode == 0);
        const auto j = nlohmann::json::parse(r.out);
        CHECK(j["status"] == "ok");
        CHECK(j["cudnn"] == "9.24");
        CHECK(j["devices"][0]["name"] == "Fake GPU");
        CHECK(j["devices"][0]["computeCapability"] == "8.6");
    }
    {
        EnvVar status("DSPTB_FAKE_GPU_STATUS", std::to_string(DSPTB_GPU_NO_DRIVER));
        const RunResult r = runCli("gpu-info --json");
        CHECK(r.exitCode == 1);
        CHECK(nlohmann::json::parse(r.out)["status"] == "no_driver");
    }
}

TEST_CASE("cli: validate --allow-skip skips a package that needs a missing runtime") {
    EnvVar dir("DSPTB_GPU_RUNTIME_DIR", emptyDir("cli-skip").string());
    const std::string pkg = "\"" + testModule("test.gpu-gated").string() + "\"";
    const RunResult strict = runCli("validate " + pkg);
    CHECK(strict.exitCode == 1);
    CHECK(strict.out.find("SKIPPED: needs the GPU runtime") != std::string::npos);
    const RunResult allowed = runCli("validate " + pkg + " --allow-skip");
    CHECK(allowed.exitCode == 0);
    CHECK(allowed.out.find("SKIPPED") != std::string::npos);
    const auto j = nlohmann::json::parse(runCli("validate " + pkg + " --allow-skip --json").out);
    CHECK(j["skipped"] == true);
    CHECK(j["passed"] == false);
    // An ordinary module still validates with the same flag.
    const RunResult plain =
        runCli("validate \"" + (fs::path(DSPTB_MODULES_DIR) / "org.dsptb.gain").string() + "\" --allow-skip");
    CHECK(plain.exitCode == 0);
    CHECK(plain.out.find("PASS") != std::string::npos);
}

#ifdef DSPTB_GPU_RUNTIME_STAGE_DIR
// Only in builds with the GPU runtime: the real folder, and a copy of it that lacks cudnn64_9.dll.
TEST_CASE("cli: gpu-info on the real runtime, and on a folder without cudnn64_9.dll") {
    {
        EnvVar dir("DSPTB_GPU_RUNTIME_DIR", DSPTB_GPU_RUNTIME_STAGE_DIR);
        const RunResult r = runCli("gpu-info --json");
        const auto j = nlohmann::json::parse(r.out);
        if (j["status"] == "no_driver" || j["status"] == "no_device") {
            MESSAGE("skipped: ", j["text"].get<std::string>());
        } else {
            REQUIRE(r.exitCode == 0);
            CHECK(j["status"] == "ok");
            CHECK(j["devices"][0]["name"].get<std::string>().find("NVIDIA") != std::string::npos);
            CHECK(j["cudnn"].get<std::string>().find("9.") == 0);
            CHECK(j["onnxruntime"] == "1.31.1");
            CHECK(j["libtorch"].get<std::string>().find("2.14.1") == 0);
        }
    }
    // Hard links keep the copy cheap.
    const fs::path src = DSPTB_GPU_RUNTIME_STAGE_DIR;
    // Next to the runtime (same volume), because hard links cannot cross volumes.
    const fs::path dst = src.parent_path() / "test-nocudnn";
    fs::remove_all(dst);
    fs::create_directories(dst);
    std::error_code ec;
    fs::copy_file(src / "gpu-runtime.json", dst / "gpu-runtime.json", ec);
    fs::copy_file(src / "dsptb_gpu.dll", dst / "dsptb_gpu.dll", ec);
    fs::create_directories(dst / "bin", ec);
    for (const auto& e : fs::directory_iterator(src / "bin"))
        if (e.path().filename() != "cudnn64_9.dll") {
            std::error_code linkEc;
            fs::create_hard_link(e.path(), dst / "bin" / e.path().filename(), linkEc);
            REQUIRE_FALSE(linkEc);
        }
    EnvVar dir("DSPTB_GPU_RUNTIME_DIR", dst.string());
    const RunResult r = runCli("gpu-info --json");
    CHECK(r.exitCode == 1);
    const auto j = nlohmann::json::parse(r.out);
    CHECK(j["status"] == "missing");
    CHECK(j["text"].get<std::string>().find("cudnn64_9.dll") != std::string::npos);
    // A GPU module is then gated with that reason, and the rest of the app keeps working.
    const RunResult v = runCli("validate \"" + testModule("test.gpu-gated").string() + "\" --allow-skip");
    CHECK(v.exitCode == 0);
    CHECK(v.out.find("cudnn64_9.dll") != std::string::npos);
    fs::remove_all(dst, ec);
}
#endif
