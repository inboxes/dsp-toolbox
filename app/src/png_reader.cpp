#include "png_reader.hpp"

#include <wincodec.h>

#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dsptb::app {

bool loadPngTexture(ID3D11Device* device, const std::filesystem::path& file,
                    ComPtr<ID3D11ShaderResourceView>& srv, UINT& width, UINT& height) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return false;

    const UINT stride = width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), stride, 0};
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
}

} // namespace dsptb::app
