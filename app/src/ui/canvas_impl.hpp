#pragma once

#include <dsptb/core/session_model.hpp>
#include <dsptb/plugin_abi.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <map>
#include <vector>

struct ImDrawList;

namespace dsptb::ui {

struct ShellContext;

// Implements dsptb_canvas_api on top of ImGui and D3D11. One host serves every instance:
// begin() binds it to the instance being drawn, so ids, textures and the "param_widget" widget all belong to
// that instance. UI thread only.
class CanvasHost {
public:
    CanvasHost(ID3D11Device* device, ID3D11DeviceContext* context);
    ~CanvasHost();
    CanvasHost(const CanvasHost&) = delete;
    CanvasHost& operator=(const CanvasHost&) = delete;

    // Returns the API table to hand to the module's draw(). Call end() afterwards.
    const dsptb_canvas_api* begin(ShellContext& shell, InstanceRecord& rec);
    void end();

    // Textures are owned by the host and freed with their instance.
    void retainInstances(const std::vector<InstanceId>& live);
    size_t textureCount() const { return textures_.size(); }

private:
    friend struct CanvasCallbacks;
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        InstanceId owner = 0;
        uint32_t width = 0, height = 0;
    };

    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    dsptb_canvas_api api_{};

    // per-draw state
    ShellContext* shell_ = nullptr;
    InstanceRecord* rec_ = nullptr;
    ImDrawList* drawList_ = nullptr;
    float originX_ = 0.0f, originY_ = 0.0f; // screen position of the current region's top-left
    float regionW_ = 0.0f, regionH_ = 0.0f;
    bool regionHovered_ = false, regionActive_ = false;
    int regionDepth_ = 0;

    std::map<uint64_t, Texture> textures_;
    uint64_t nextTexture_ = 1;
};

} // namespace dsptb::ui
