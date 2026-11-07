#include "png_writer.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <system_error>

using Microsoft::WRL::ComPtr;

namespace dsptb::app {

bool writePng(const std::filesystem::path& file, const std::vector<uint8_t>& rgba, uint32_t width,
              uint32_t height) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4 || width == 0 || height == 0)
        return false;
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(initHr); // S_FALSE also needs a matching CoUninitialize

    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        // The PNG encoder takes BGRA, not RGBA, so swizzle first.
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
            bgra[i + 0] = rgba[i + 2];
            bgra[i + 1] = rgba[i + 1];
            bgra[i + 2] = rgba[i + 0];
            bgra[i + 3] = rgba[i + 3];
        }
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        const UINT stride = width * 4;

        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory))) &&
             SUCCEEDED(factory->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
             format == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(frame->WritePixels(height, stride, stride * height, bgra.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (uninit)
        CoUninitialize();
    return ok;
}

} // namespace dsptb::app
