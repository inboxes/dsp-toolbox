#include "d3d11_context.hpp"

#include <d3d11_4.h> // ID3D11Multithread

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace dsptb::app {

bool D3D11Context::create(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    // BGRA support is required by Direct2D/DirectWrite interop that modules and tooling may use later.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr =
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1,
                                      D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &got, &context_);
    warp_ = false;
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 1,
                                           D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &got, &context_);
        warp_ = true;
    }
    if (FAILED(hr))
        return false;

    // The GPU service and the UI thread will share this device; make immediate-context use safe.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(device_.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    createRenderTarget();
    return rtv_ != nullptr;
}

void D3D11Context::destroy() {
    rtv_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

void D3D11Context::createRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
}

void D3D11Context::resize(UINT width, UINT height) {
    if (!swapChain_ || width == 0 || height == 0)
        return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

void D3D11Context::beginFrame(const float clearColor[4]) {
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
}

void D3D11Context::present(bool vsync) {
    swapChain_->Present(vsync ? 1 : 0, 0);
}

bool D3D11Context::readBackbuffer(std::vector<uint8_t>& rgba, UINT& width, UINT& height) {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging)))
        return false;
    context_->CopyResource(staging.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    width = desc.Width;
    height = desc.Height;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        const auto* src =
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
        std::memcpy(dst, src, static_cast<size_t>(width) * 4);
        for (UINT x = 0; x < width; ++x)
            dst[x * 4 + 3] = 255; // the window is opaque; drop blended alpha
    }
    context_->Unmap(staging.Get(), 0);
    return true;
}

} // namespace dsptb::app
