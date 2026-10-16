#pragma once

#include "Types/Math.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Engine
{
    inline constexpr uint32_t kMaxSkinJoints = 128;

    struct Vertex
    {
        Vector3 position;
        Vector3 normal;
        Vector2 uv;
    };

    struct VertexInfluence
    {
        std::array<uint32_t, 4> joints{};
        Vector4 weights{};
    };

    struct SkinnedVertex
    {
        Vertex vertex;
        VertexInfluence influence;
    };

    struct SkinBinding
    {
        std::vector<uint32_t> jointNodeIndices;
        std::vector<Matrix> inverseBind;
    };

    struct NodeData
    {
        std::string name;
        int32_t parent = -1;
        Matrix bindLocal = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    struct MaterialBlendData
    {
        Vector4 baseColor = {1, 1, 1, 1};
        std::filesystem::path textureFile;
        float startHeight = 0.0f;
        float endHeight = 1.0f;
    };

    struct MaterialData
    {
        std::string name;
        Vector4 baseColor = {1, 1, 1, 1};
        std::filesystem::path textureFile;
        std::optional<MaterialBlendData> blend = std::nullopt;
    };

    struct MeshData
    {
        std::string name;
        uint32_t nodeIndex = 0;
        uint32_t materialIndex = 0;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<VertexInfluence> influences;
        std::optional<SkinBinding> skin = std::nullopt;
    };

    struct ModelData
    {
        std::vector<NodeData> nodes;
        std::vector<MaterialData> materials;
        std::vector<MeshData> meshes;
    };
}
