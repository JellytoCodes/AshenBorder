#pragma once

#include "Graphics/ModelData.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    class GraphicsDevice;

    struct Material
    {
        Vector4 baseColor = {1, 1, 1, 1};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuseTexture;
        Vector4 blendColor = {1, 1, 1, 1};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blendTexture;
        Vector2 blendHeights = {0.0f, 1.0f};
        bool blendEnabled = false;
    };

    struct MeshGpu
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        uint32_t indexCount = 0;
        uint32_t materialIndex = 0;
        Matrix nodeWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        uint32_t nodeIndex = 0;
        std::optional<SkinBinding> skin = std::nullopt;
    };

    class Model
    {
    public:
        Model() = default;
        Model(const Model&) = delete;
        Model& operator=(const Model&) = delete;
        bool Upload(const ModelData& data, GraphicsDevice& graphics, std::string& error);
        std::span<const MeshGpu> Meshes() const noexcept;
        std::span<const Material> Materials() const noexcept;
        std::span<const NodeData> Nodes() const noexcept;
        Vector3 BoundsMin() const noexcept;
        Vector3 BoundsMax() const noexcept;
        std::string_view SkeletonSignature() const noexcept;

    private:
        std::vector<MeshGpu> _meshes;
        std::vector<Material> _materials;
        std::vector<NodeData> _nodes;
        Vector3 _boundsMin = {};
        Vector3 _boundsMax = {};
        std::string _skeletonSignature;
    };
}
