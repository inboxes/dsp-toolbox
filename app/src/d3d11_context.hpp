#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace dsptb::app {

// Owns the D3D11 device and a flip-model swap chain for one window. UI thread only.
class D3D11Context {
public:
    // Feature level 11.0, hardware first, WARP fallback (headless CI runners have no GPU).
    bool create(HWND hwnd);
    void destroy();

    void resize(UINT width, UINT height);
    // Binds the backbuffer and clears it.
    void beginFrame(const float clearColor[4]);
    void present(bool vsync);

    // Copies the current backbuffer to tightly packed RGBA8. Must be called before present(): a flip-model
    // backbuffer's contents are undefined after Present.
    bool readBackbuffer(std::vector<uint8_t>& rgba, UINT& width, UINT& height);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    bool usingWarp() const { return warp_; }

private:
    void createRenderTarget();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    bool warp_ = false;
};

} // namespace dsptb::app
