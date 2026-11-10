// gpu-info
#include "commands.hpp"

#include <dsptb/core/gpu_runtime_loader.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>

namespace dsptb::cli {

namespace {

const char* statusName(int status) {
    switch (status) {
    case DSPTB_GPU_OK:
        return "ok";
    case DSPTB_GPU_NO_DRIVER:
        return "no_driver";
    case DSPTB_GPU_NO_DEVICE:
        return "no_device";
    case DSPTB_GPU_DLL_MISSING:
        return "missing";
    case DSPTB_GPU_VERSION_MISMATCH:
        return "version_mismatch";
    case DSPTB_GPU_INIT_FAILED:
        return "init_failed";
    default:
        return "not_initialized";
    }
}

std::string cudaText(int v) {
    return v > 0 ? std::to_string(v / 1000) + "." + std::to_string(v % 1000 / 10) : std::string("-");
}
std::string cudnnText(int v) {
    return v > 0 ? std::to_string(v / 10000) + "." + std::to_string(v % 10000 / 100) : std::string("-");
}
double gb(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

} // namespace

void addGpuInfoCommand(CLI::App& app, int& exitCode) {
    auto* cmd = app.add_subcommand("gpu-info", "Show the GPU runtime: status, device and library versions");
    auto asJson = std::make_shared<bool>(false);
    cmd->add_flag("--json", *asJson, "Print JSON");
    cmd->callback([&exitCode, asJson] {
        loadGpuRuntimeFromSettings();
        const GpuRuntimeLoader& rt = GpuRuntimeLoader::instance();
        const GpuRuntimeState& s = rt.state();
        const std::vector<GpuDevice> devices = rt.devices();
        if (*asJson) {
            nlohmann::json j;
            j["status"] = statusName(s.status);
            j["statusCode"] = s.status;
            j["text"] = s.statusText;
            j["runtimeDir"] = s.runtimeDir;
            j["abi"] = s.abiVersion;
            j["driverCuda"] = cudaText(s.driverCuda);
            j["runtimeCuda"] = cudaText(s.runtimeCuda);
            j["cudnn"] = cudnnText(s.cudnn);
            j["onnxruntime"] = s.ortVersion;
            j["libtorch"] = s.torchVersion;
            j["deviceCount"] = s.deviceCount;
            j["selectedDevice"] = s.selectedDevice;
            j["sync"] = s.syncMode == DSPTB_GPU_SYNC_BLOCKING ? "blocking" : "spin";
            j["devices"] = nlohmann::json::array();
            for (const GpuDevice& d : devices)
                j["devices"].push_back({{"index", d.index},
                                        {"name", d.name},
                                        {"computeCapability", std::to_string(d.ccMajor) + "." + std::to_string(d.ccMinor)},
                                        {"vramTotalBytes", d.vramTotal},
                                        {"vramFreeBytes", d.vramFree}});
            std::cout << j.dump(2) << "\n";
        } else {
            std::printf("GPU runtime: %s\n  %s\n  folder: %s\n", s.ok() ? "OK" : "NOT AVAILABLE", s.statusText.c_str(),
                        s.runtimeDir.c_str());
            if (s.ok()) {
                std::printf("  driver CUDA %s, runtime CUDA %s, cuDNN %s\n  ONNX Runtime %s, LibTorch %s\n  sync: %s\n",
                            cudaText(s.driverCuda).c_str(), cudaText(s.runtimeCuda).c_str(), cudnnText(s.cudnn).c_str(),
                            s.ortVersion.c_str(), s.torchVersion.c_str(),
                            s.syncMode == DSPTB_GPU_SYNC_BLOCKING ? "blocking" : "spin");
                for (const GpuDevice& d : devices)
                    std::printf("  device %d%s: %s (cc %d.%d, %.1f GB, %.1f GB free)\n", d.index,
                                d.index == s.selectedDevice ? " (selected)" : "", d.name.c_str(), d.ccMajor, d.ccMinor,
                                gb(d.vramTotal), gb(d.vramFree));
            }
        }
        exitCode = s.ok() ? 0 : 1;
    });
}

} // namespace dsptb::cli
