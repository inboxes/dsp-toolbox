#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>

namespace dsptb::app {

// Loads a PNG file into a shader-resource view that ImGui can draw. COM must be initialised on this thread.
bool loadPngTexture(ID3D11Device* device, const std::filesystem::path& file,
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv, UINT& width, UINT& height);

} // namespace dsptb::app
