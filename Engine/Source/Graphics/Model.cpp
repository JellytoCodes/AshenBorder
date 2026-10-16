#include "Graphics/Model.h"

#include "Graphics/GraphicsDevice.h"
#include "Resource/ModelCache.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>

namespace
{
    using Microsoft::WRL::ComPtr;

    bool Fail(std::string& error, const char* operation, HRESULT result)
    {
        std::ostringstream text;
        text << operation << " failed, HRESULT=0x" << std::hex << static_cast<uint32_t>(result);
        error = text.str();
        return false;
    }

    bool Finite(const Engine::Matrix& value)
    {
        for (const auto& row : value.m)
        {
            for (const float element : row)
            {
                if (!std::isfinite(element))
                {
                    return false;
                }
            }
        }
        const double determinant =
            static_cast<double>(value._11) * (static_cast<double>(value._22) * value._33 - static_cast<double>(value._23) * value._32) -
            static_cast<double>(value._12) * (static_cast<double>(value._21) * value._33 - static_cast<double>(value._23) * value._31) +
            static_cast<double>(value._13) * (static_cast<double>(value._21) * value._32 - static_cast<double>(value._22) * value._31);
        return determinant != 0.0 && std::isfinite(determinant) &&
            value._14 == 0.0f && value._24 == 0.0f && value._34 == 0.0f && value._44 == 1.0f;
    }

    bool OpaqueColor(const Engine::Vector4& color)
    {
        return std::isfinite(color.x) && std::isfinite(color.y) &&
            std::isfinite(color.z) && std::isfinite(color.w) &&
            color.x >= 0.0f && color.x <= 1.0f && color.y >= 0.0f && color.y <= 1.0f &&
            color.z >= 0.0f && color.z <= 1.0f && color.w == 1.0f;
    }

    bool Texture(ID3D11Device& device, const std::filesystem::path& file,
        ComPtr<ID3D11ShaderResourceView>& output, std::string& error)
    {
        UINT width = 1;
        UINT height = 1;
        std::vector<uint8_t> pixels(4, 255);
        if (!file.empty())
        {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
            {
                return Fail(error, "CoInitializeEx", apartment);
            }
            struct ApartmentScope
            {
                bool entered;
                ~ApartmentScope()
                {
                    if (entered)
                    {
                        CoUninitialize();
                    }
                }
            } scope{SUCCEEDED(apartment)};

            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
            if (FAILED(result))
            {
                return Fail(error, "WIC factory", result);
            }
            result = factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
            if (FAILED(result))
            {
                return Fail(error, "WIC texture decode", result);
            }
            result = decoder->GetFrame(0, frame.GetAddressOf());
            if (FAILED(result))
            {
                return Fail(error, "WIC first frame", result);
            }
            result = frame->GetSize(&width, &height);
            if (FAILED(result))
            {
                return Fail(error, "WIC texture size", result);
            }
            const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
            if (width == 0 || height == 0 || width > 16384 || height > 16384 || bytes > 256ull * 1024 * 1024)
            {
                error = "Texture dimensions exceed the static model limit";
                return false;
            }
            result = factory->CreateFormatConverter(converter.GetAddressOf());
            if (FAILED(result))
            {
                return Fail(error, "WIC converter", result);
            }
            result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
            if (FAILED(result))
            {
                return Fail(error, "WIC RGBA conversion", result);
            }
            pixels.resize(static_cast<size_t>(bytes));
            result = converter->CopyPixels(nullptr, width * 4,
                static_cast<UINT>(pixels.size()), pixels.data());
            if (FAILED(result))
            {
                return Fail(error, "WIC texture pixels", result);
            }
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial = {pixels.data(), width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        HRESULT result = device.CreateTexture2D(&desc, &initial, texture.GetAddressOf());
        if (FAILED(result))
        {
            return Fail(error, "CreateTexture2D", result);
        }
        result = device.CreateShaderResourceView(texture.Get(), nullptr, output.GetAddressOf());
        return SUCCEEDED(result) || Fail(error, "CreateShaderResourceView", result);
    }
}

namespace Engine
{
    static_assert(sizeof(Vertex) == 32);
    static_assert(sizeof(VertexInfluence) == 32);
    static_assert(sizeof(SkinnedVertex) == 64);
    static_assert(offsetof(SkinnedVertex, influence) == 32);
    static_assert(offsetof(VertexInfluence, weights) == 16);

    bool Model::Upload(const ModelData& data, GraphicsDevice& graphics, std::string& error)
    {
        error.clear();
        if (!graphics.Device() || data.nodes.empty() || data.nodes.size() > 4096 ||
            data.materials.empty() || data.materials.size() > 4096 ||
            data.meshes.empty() || data.meshes.size() > 4096)
        {
            error = "Model requires a device and nonempty, bounded node/material/mesh arrays";
            return false;
        }
        std::vector<Matrix> nodeWorld(data.nodes.size());
        std::vector<bool> ready(data.nodes.size(), false);
        size_t remaining = data.nodes.size();
        for (const NodeData& node : data.nodes)
        {
            if (node.name.empty() || !Finite(node.bindLocal) || node.parent < -1 ||
                node.parent >= static_cast<int32_t>(data.nodes.size()))
            {
                error = "Invalid node name, affine transform or parent";
                return false;
            }
        }
        while (remaining > 0)
        {
            size_t progress = 0;
            for (size_t index = 0; index < data.nodes.size(); ++index)
            {
                const NodeData& node = data.nodes[index];
                if (ready[index] || (node.parent >= 0 && !ready[static_cast<size_t>(node.parent)]))
                {
                    continue;
                }
                const auto local = DirectX::XMLoadFloat4x4(&node.bindLocal);
                const auto parent = node.parent < 0 ? DirectX::XMMatrixIdentity()
                    : DirectX::XMLoadFloat4x4(&nodeWorld[static_cast<size_t>(node.parent)]);
                DirectX::XMStoreFloat4x4(&nodeWorld[index], local * parent);
                if (!Finite(nodeWorld[index]))
                {
                    error = "Node hierarchy produces an invalid transform";
                    return false;
                }
                ready[index] = true;
                --remaining;
                ++progress;
            }
            if (progress == 0)
            {
                error = "Node hierarchy contains a cycle";
                return false;
            }
        }

        Vector3 boundsMin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Vector3 boundsMax{-boundsMin.x, -boundsMin.y, -boundsMin.z};
        for (const MaterialData& material : data.materials)
        {
            if (material.name.empty() || !OpaqueColor(material.baseColor))
            {
                error = "Static opaque material requires a name and finite linear color in [0,1], alpha=1";
                return false;
            }
            if (material.blend)
            {
                const MaterialBlendData& blend = *material.blend;
                const double range = static_cast<double>(blend.endHeight) - blend.startHeight;
                if (!OpaqueColor(blend.baseColor) || !std::isfinite(blend.startHeight) ||
                    !std::isfinite(blend.endHeight) || blend.endHeight <= blend.startHeight ||
                    (blend.startHeight != 0.0f && !std::isnormal(blend.startHeight)) ||
                    (blend.endHeight != 0.0f && !std::isnormal(blend.endHeight)) ||
                    range > static_cast<double>(std::numeric_limits<float>::max()) ||
                    range < static_cast<double>(std::numeric_limits<float>::min()))
                {
                    error = "Material blend requires an opaque color and a finite increasing GPU height interval";
                    return false;
                }
            }
        }
        for (const MeshData& mesh : data.meshes)
        {
            const size_t vertexStride = mesh.skin ? sizeof(SkinnedVertex) : sizeof(Vertex);
            if (mesh.name.empty() || mesh.nodeIndex >= data.nodes.size() ||
                mesh.materialIndex >= data.materials.size() || mesh.vertices.empty() ||
                mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
                mesh.vertices.size() > std::numeric_limits<UINT>::max() / vertexStride ||
                mesh.indices.size() > std::numeric_limits<UINT>::max() / sizeof(uint32_t))
            {
                error = "Invalid mesh references, triangle count or buffer size";
                return false;
            }
            if (!mesh.skin && !mesh.influences.empty())
            {
                error = "Static mesh cannot contain skin influences without a binding";
                return false;
            }
            if (mesh.skin)
            {
                const SkinBinding& skin = *mesh.skin;
                const size_t jointCount = skin.jointNodeIndices.size();
                if (jointCount == 0 || jointCount > kMaxSkinJoints ||
                    skin.inverseBind.size() != jointCount || mesh.influences.size() != mesh.vertices.size())
                {
                    error = "Skin requires 1..128 joints, inverse binds and one influence record per vertex";
                    return false;
                }
                for (size_t joint = 0; joint < jointCount; ++joint)
                {
                    const uint32_t node = skin.jointNodeIndices[joint];
                    if (node >= data.nodes.size() || !Finite(skin.inverseBind[joint]) ||
                        std::find(skin.jointNodeIndices.begin(), skin.jointNodeIndices.begin() + joint, node)
                            != skin.jointNodeIndices.begin() + joint)
                    {
                        error = "Skin contains an invalid or repeated joint node or inverse bind";
                        return false;
                    }
                    Matrix bindingWorld;
                    DirectX::XMStoreFloat4x4(&bindingWorld,
                        DirectX::XMLoadFloat4x4(&skin.inverseBind[joint]) *
                        DirectX::XMLoadFloat4x4(&nodeWorld[node]));
                    if (!Finite(bindingWorld))
                    {
                        error = "Skin bind composition is invalid";
                        return false;
                    }
                    for (size_t row = 0; row < 4; ++row)
                    {
                        for (size_t column = 0; column < 4; ++column)
                        {
                            const float expected = nodeWorld[mesh.nodeIndex].m[row][column];
                            if (std::abs(static_cast<double>(bindingWorld.m[row][column]) - expected) >
                                0.0001 * std::max(1.0, std::abs(static_cast<double>(expected))))
                            {
                                error = "Inverse bind disagrees with mesh and joint bind worlds";
                                return false;
                            }
                        }
                    }
                }
                for (const VertexInfluence& influence : mesh.influences)
                {
                    const std::array<float, 4> weights = {influence.weights.x, influence.weights.y,
                        influence.weights.z, influence.weights.w};
                    double sum = 0.0;
                    for (size_t joint = 0; joint < weights.size(); ++joint)
                    {
                        if (!std::isfinite(weights[joint]) || weights[joint] < 0.0f ||
                            influence.joints[joint] >= jointCount)
                        {
                            error = "All four skin weights and joint indices must be finite, nonnegative and in range";
                            return false;
                        }
                        sum += weights[joint];
                    }
                    if (sum <= 0.0 || std::abs(sum - 1.0) > 0.0001)
                    {
                        error = "Skin weights must sum to one; unweighted vertices require an explicit writer fallback";
                        return false;
                    }
                }
            }
            for (const uint32_t index : mesh.indices)
            {
                if (index >= mesh.vertices.size())
                {
                    error = "Mesh index exceeds its vertex array";
                    return false;
                }
            }
            for (const Vertex& vertex : mesh.vertices)
            {
                const Vector3 p = vertex.position;
                const Vector3 n = vertex.normal;
                const double lengthSquared = static_cast<double>(n.x) * n.x + static_cast<double>(n.y) * n.y + static_cast<double>(n.z) * n.z;
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                    !std::isfinite(lengthSquared) || lengthSquared <= 0.0 ||
                    !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y))
                {
                    error = "Mesh contains nonfinite vertices or a zero normal";
                    return false;
                }
                Vector3 world;
                DirectX::XMStoreFloat3(&world, DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&p), DirectX::XMLoadFloat4x4(&nodeWorld[mesh.nodeIndex])));
                if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
                {
                    error = "Mesh bounds overflow after node transforms";
                    return false;
                }
                boundsMin.x = std::min(boundsMin.x, world.x);
                boundsMin.y = std::min(boundsMin.y, world.y);
                boundsMin.z = std::min(boundsMin.z, world.z);
                boundsMax.x = std::max(boundsMax.x, world.x);
                boundsMax.y = std::max(boundsMax.y, world.y);
                boundsMax.z = std::max(boundsMax.z, world.z);
            }
        }

        std::string skeletonSignature;
        if (!ComputeSkeletonSignature(data, skeletonSignature, error))
        {
            return false;
        }
        std::vector<Material> materials;
        std::vector<MeshGpu> meshes;
        auto nodes = data.nodes;
        materials.reserve(data.materials.size());
        meshes.reserve(data.meshes.size());
        for (const MaterialData& source : data.materials)
        {
            Material material;
            material.baseColor = source.baseColor;
            if (!Texture(*graphics.Device(), source.textureFile, material.diffuseTexture, error))
            {
                return false;
            }
            if (source.blend)
            {
                const MaterialBlendData& blend = *source.blend;
                material.blendColor = blend.baseColor;
                material.blendHeights = {blend.startHeight, blend.endHeight};
                material.blendEnabled = true;
                if (!Texture(*graphics.Device(), blend.textureFile, material.blendTexture, error))
                {
                    return false;
                }
            }
            else
            {
                material.blendColor = material.baseColor;
                material.blendTexture = material.diffuseTexture;
            }
            materials.push_back(std::move(material));
        }
        for (const MeshData& source : data.meshes)
        {
            auto vertices = source.vertices;
            for (Vertex& vertex : vertices)
            {
                const double x = vertex.normal.x;
                const double y = vertex.normal.y;
                const double z = vertex.normal.z;
                const double length = std::sqrt(x * x + y * y + z * z);
                vertex.normal = {static_cast<float>(x / length), static_cast<float>(y / length),
                    static_cast<float>(z / length)};
            }
            MeshGpu mesh;
            mesh.materialIndex = source.materialIndex;
            mesh.nodeIndex = source.nodeIndex;
            mesh.skin = source.skin;
            mesh.nodeWorld = nodeWorld[source.nodeIndex];
            mesh.indexCount = static_cast<uint32_t>(source.indices.size());
            D3D11_BUFFER_DESC desc = {};
            std::vector<SkinnedVertex> skinnedVertices;
            if (source.skin)
            {
                skinnedVertices.reserve(vertices.size());
                for (size_t index = 0; index < vertices.size(); ++index)
                {
                    VertexInfluence influence = source.influences[index];
                    const double sum = static_cast<double>(influence.weights.x) + influence.weights.y +
                        influence.weights.z + influence.weights.w;
                    influence.weights = {static_cast<float>(influence.weights.x / sum),
                        static_cast<float>(influence.weights.y / sum), static_cast<float>(influence.weights.z / sum),
                        static_cast<float>(influence.weights.w / sum)};
                    skinnedVertices.push_back({vertices[index], influence});
                }
            }
            const size_t stride = source.skin ? sizeof(SkinnedVertex) : sizeof(Vertex);
            desc.ByteWidth = static_cast<UINT>(source.vertices.size() * stride);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA initial = {
                source.skin ? static_cast<const void*>(skinnedVertices.data()) : vertices.data(), 0, 0};
            HRESULT result = graphics.Device()->CreateBuffer(&desc, &initial, mesh.vertexBuffer.GetAddressOf());
            if (FAILED(result))
            {
                return Fail(error, "Model vertex buffer", result);
            }
            desc.ByteWidth = static_cast<UINT>(source.indices.size() * sizeof(uint32_t));
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            initial.pSysMem = source.indices.data();
            result = graphics.Device()->CreateBuffer(&desc, &initial, mesh.indexBuffer.GetAddressOf());
            if (FAILED(result))
            {
                return Fail(error, "Model index buffer", result);
            }
            meshes.push_back(std::move(mesh));
        }
        _nodes = std::move(nodes);
        _materials = std::move(materials);
        _meshes = std::move(meshes);
        _boundsMin = boundsMin;
        _boundsMax = boundsMax;
        _skeletonSignature = std::move(skeletonSignature);
        return true;
    }

    std::span<const MeshGpu> Model::Meshes() const noexcept
    {
        return _meshes;
    }

    std::span<const Material> Model::Materials() const noexcept
    {
        return _materials;
    }

    std::span<const NodeData> Model::Nodes() const noexcept
    {
        return _nodes;
    }

    Vector3 Model::BoundsMin() const noexcept
    {
        return _boundsMin;
    }

    Vector3 Model::BoundsMax() const noexcept
    {
        return _boundsMax;
    }

    std::string_view Model::SkeletonSignature() const noexcept
    {
        return _skeletonSignature;
    }
}
