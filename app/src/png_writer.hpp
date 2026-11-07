#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dsptb::app {

// Writes tightly packed RGBA8 pixels to a PNG using WIC. Creates the parent folder. Returns false on failure.
bool writePng(const std::filesystem::path& file, const std::vector<uint8_t>& rgba, uint32_t width,
              uint32_t height);

} // namespace dsptb::app
