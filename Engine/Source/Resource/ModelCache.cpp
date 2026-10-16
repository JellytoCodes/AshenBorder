#include "Resource/ModelCache.h"

#include "Animation/AnimationClip.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    constexpr std::uint64_t kMaximumBytes = 256ULL * 1024 * 1024;
    constexpr std::uint32_t kMaximumVertices = 1000000;
    constexpr std::uint32_t kMaximumIndices = 3000000;
    constexpr float kMaximumValue = 1000000.0f;
    constexpr std::uint32_t kHeaderBytes = 64;

    void Require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    bool EqualPath(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    bool ContainsPath(const std::filesystem::path& root, const std::filesystem::path& file)
    {
        auto part = file.begin();
        for (const auto& component : root)
        {
            if (part == file.end() || !EqualPath(component, *part))
            {
                return false;
            }
            ++part;
        }
        return true;
    }

    std::filesystem::path ResolveFile(const std::filesystem::path& root,
        const std::filesystem::path& relative)
    {
        Require(!relative.empty() && !relative.is_absolute() &&
            !relative.has_root_name() && !relative.has_root_directory(), "Invalid relative path");
        for (const auto& component : relative)
        {
            const auto name = component.native();
            Require(name != L"." && name != L".." && !name.empty() &&
                name.find(L':') == std::wstring::npos &&
                name.find(L'\0') == std::wstring::npos, "Unsafe path component");
        }
        const auto file = std::filesystem::canonical(root / relative);
        Require(ContainsPath(root, file), "Resolved path escapes the resource/bundle root");
        Require(std::filesystem::is_regular_file(file), "Expected a regular file");
        return file;
    }

    std::filesystem::path Utf8Path(const std::string& value)
    {
        Require(!value.empty() && value.find('\\') == std::string::npos &&
            value.find('\0') == std::string::npos, "Invalid UTF-8 relative path");
        return std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(value.data()), value.size()));
    }

    std::vector<std::uint8_t> ReadFile(const std::filesystem::path& file,
        std::uint64_t maximum)
    {
        const auto length = std::filesystem::file_size(file);
        Require(length > 0 && length <= maximum, "Invalid file length");
        std::ifstream stream(file, std::ios::binary);
        Require(stream.is_open(), "Cannot open file");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        Require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))), "Truncated file read");
        char extra = 0;
        Require(!stream.get(extra) && stream.eof(), "File length changed during read");
        return bytes;
    }

    std::string Sha256(std::span<const std::uint8_t> bytes)
    {
        BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
        Require(BCryptOpenAlgorithmProvider(&rawAlgorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) >= 0, "Cannot open SHA-256 provider");
        const auto closeAlgorithm = [](void* handle)
        {
            BCryptCloseAlgorithmProvider(handle, 0);
        };
        const std::unique_ptr<void, decltype(closeAlgorithm)> algorithm(rawAlgorithm, closeAlgorithm);
        DWORD objectLength = 0;
        DWORD resultLength = 0;
        Require(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) >= 0 &&
            resultLength == sizeof(objectLength) && objectLength > 0 && objectLength <= 1048576,
            "Invalid SHA-256 object length");
        std::vector<std::uint8_t> object(objectLength);
        BCRYPT_HASH_HANDLE rawHash = nullptr;
        Require(BCryptCreateHash(algorithm.get(), &rawHash, object.data(), objectLength,
            nullptr, 0, 0) >= 0, "Cannot create SHA-256 hash");
        const auto closeHash = [](void* handle)
        {
            BCryptDestroyHash(handle);
        };
        const std::unique_ptr<void, decltype(closeHash)> hash(rawHash, closeHash);
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const auto count = static_cast<ULONG>(
                std::min<std::size_t>(bytes.size() - offset, 65536));
            Require(BCryptHashData(hash.get(),
                const_cast<PUCHAR>(bytes.data() + offset), count, 0) >= 0,
                "SHA-256 hashing failed");
            offset += count;
        }
        std::array<std::uint8_t, 32> output{};
        Require(BCryptFinishHash(hash.get(), output.data(),
            static_cast<ULONG>(output.size()), 0) >= 0, "SHA-256 finalization failed");
        constexpr char kHex[] = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (const auto byte : output)
        {
            text.push_back(kHex[byte >> 4]);
            text.push_back(kHex[byte & 15]);
        }
        return text;
    }

    std::string JsonHash(const Json& value)
    {
        const std::string text = value.dump();
        return Sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    bool IsHash(const std::string& value)
    {
        return value.size() == 64 && std::all_of(value.begin(), value.end(),
            [](char character)
            {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
    }

    std::uint64_t Unsigned(const Json& object, const char* key)
    {
        const auto& value = object.at(key);
        Require(value.is_number_unsigned(), "Expected an unsigned integer metadata field");
        return value.get<std::uint64_t>();
    }

    std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t& offset)
    {
        Require(offset <= bytes.size() && bytes.size() - offset >= 4, "Truncated binary field");
        std::uint32_t value = 0;
        for (std::uint32_t byte = 0; byte < 4; ++byte)
        {
            value |= static_cast<std::uint32_t>(bytes[offset++]) << (byte * 8);
        }
        return value;
    }

    std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t& offset)
    {
        const auto low = ReadU32(bytes, offset);
        const auto high = ReadU32(bytes, offset);
        return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
    }

    float ReadFloat(std::span<const std::uint8_t> bytes, std::size_t& offset)
    {
        const float value = std::bit_cast<float>(ReadU32(bytes, offset));
        Require(std::isfinite(value) && std::abs(value) <= kMaximumValue,
            "Non-finite or excessive float in cache");
        return value;
    }

    double ReadDouble(std::span<const std::uint8_t> bytes, std::size_t& offset)
    {
        const double value = std::bit_cast<double>(ReadU64(bytes, offset));
        Require(std::isfinite(value) && value >= 0.0 && value <= kMaximumValue,
            "Invalid animation time");
        return value;
    }

    void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        for (uint32_t index = 0; index < 4; ++index)
        {
            bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    }

    void AppendString(std::vector<uint8_t>& bytes, const std::string& value)
    {
        Require(!value.empty() && value.size() <= 4096 && value.find('\0') == std::string::npos,
            "Invalid signature node name");
        AppendU32(bytes, static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void AppendMatrix(std::vector<uint8_t>& bytes, const Engine::Matrix& matrix)
    {
        for (const auto& row : matrix.m)
        {
            for (const float value : row)
            {
                Require(std::isfinite(value) && std::abs(value) <= kMaximumValue,
                    "Invalid signature matrix value");
                AppendU32(bytes, std::bit_cast<uint32_t>(value));
            }
        }
    }

    std::string ReadString(std::span<const std::uint8_t> bytes, std::size_t& offset, bool allowEmpty = false)
    {
        const auto length = ReadU32(bytes, offset);
        Require((length > 0 || allowEmpty) && length <= 4096 && offset <= bytes.size() &&
            length <= bytes.size() - offset, "Invalid string length");
        std::string value(reinterpret_cast<const char*>(bytes.data() + offset), length);
        offset += length;
        Require(value.find('\0') == std::string::npos && (value.empty() ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), nullptr, 0) > 0), "Invalid UTF-8 string");
        return value;
    }

    Json ReadMetadata(const std::filesystem::path& file)
    {
        const auto bytes = ReadFile(file, 1024 * 1024);
        std::vector<std::set<std::string>> objectKeys;
        const auto callback = [&objectKeys](int, Json::parse_event_t event, Json& parsed)
        {
            if (event == Json::parse_event_t::object_start)
            {
                objectKeys.emplace_back();
            }
            else if (event == Json::parse_event_t::key)
            {
                Require(!objectKeys.empty() &&
                    objectKeys.back().insert(parsed.get<std::string>()).second,
                    "Duplicate metadata key");
            }
            else if (event == Json::parse_event_t::object_end)
            {
                objectKeys.pop_back();
            }
            return true;
        };
        return Json::parse(bytes.begin(), bytes.end(), callback);
    }

    bool AffineNonsingular(const Engine::Matrix& matrix)
    {
        for (const auto& row : matrix.m)
        {
            for (const float value : row)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
        }
        const double determinant =
            static_cast<double>(matrix._11) * (static_cast<double>(matrix._22) * matrix._33 - static_cast<double>(matrix._23) * matrix._32) -
            static_cast<double>(matrix._12) * (static_cast<double>(matrix._21) * matrix._33 - static_cast<double>(matrix._23) * matrix._31) +
            static_cast<double>(matrix._13) * (static_cast<double>(matrix._21) * matrix._32 - static_cast<double>(matrix._22) * matrix._31);
        return matrix._14 == 0 && matrix._24 == 0 && matrix._34 == 0 && matrix._44 == 1 &&
            std::isfinite(determinant) && determinant != 0;
    }

    Engine::ClipData ReadClipBinary(std::span<const uint8_t> bytes,
        const Engine::ModelData& model, const Json& record, std::string_view expectedSignature)
    {
        Require(bytes.size() >= 64 && bytes[0] == 'E' && bytes[1] == 'C' &&
            bytes[2] == 'L' && bytes[3] == 'P', "Invalid ECLP magic/header");
        size_t offset = 4;
        Require(ReadU32(bytes, offset) == 1 && ReadU32(bytes, offset) == 64 &&
            ReadU32(bytes, offset) == 1 && ReadU64(bytes, offset) == bytes.size(), "Invalid ECLP version/length/flags");
        Engine::ClipData data;
        data.nodeCount = ReadU32(bytes, offset);
        const uint32_t channelCount = ReadU32(bytes, offset);
        data.durationSeconds = ReadDouble(bytes, offset);
        Require(data.nodeCount == model.nodes.size() && channelCount <= data.nodeCount &&
            data.durationSeconds > 0, "Invalid clip node/channel count or duration");
        Require(ReadU64(bytes, offset) == bytes.size() - 64 && ReadU64(bytes, offset) == 0 &&
            ReadU64(bytes, offset) == 0, "Invalid clip payload or reserved fields");
        data.name = ReadString(bytes, offset);
        data.sourceTake = ReadString(bytes, offset);
        data.skeletonSignature = ReadString(bytes, offset);
        Require(data.skeletonSignature == expectedSignature, "Clip skeleton signature mismatch");
        uint64_t totalKeys = 0;
        for (uint32_t channelIndex = 0; channelIndex < channelCount; ++channelIndex)
        {
            Engine::TRSChannel channel;
            channel.nodeIndex = ReadU32(bytes, offset);
            const uint32_t translations = ReadU32(bytes, offset);
            const uint32_t rotations = ReadU32(bytes, offset);
            const uint32_t scales = ReadU32(bytes, offset);
            totalKeys += static_cast<uint64_t>(translations) + rotations + scales;
            Require(channel.nodeIndex < data.nodeCount && totalKeys <= 1000000 &&
                offset <= bytes.size() && static_cast<uint64_t>(translations) * 20 +
                static_cast<uint64_t>(rotations) * 24 + static_cast<uint64_t>(scales) * 20 <= bytes.size() - offset,
                "Invalid channel key count or truncated payload");
            for (uint32_t key = 0; key < translations; ++key)
            {
                channel.translations.push_back({ReadDouble(bytes, offset),
                    {ReadFloat(bytes, offset), ReadFloat(bytes, offset), ReadFloat(bytes, offset)}});
            }
            for (uint32_t key = 0; key < rotations; ++key)
            {
                channel.rotations.push_back({ReadDouble(bytes, offset),
                    {ReadFloat(bytes, offset), ReadFloat(bytes, offset), ReadFloat(bytes, offset), ReadFloat(bytes, offset)}});
            }
            for (uint32_t key = 0; key < scales; ++key)
            {
                channel.scales.push_back({ReadDouble(bytes, offset),
                    {ReadFloat(bytes, offset), ReadFloat(bytes, offset), ReadFloat(bytes, offset)}});
            }
            data.channels.push_back(std::move(channel));
        }
        Require(offset == bytes.size(), "Clip trailing length mismatch");
        Require(record.at("name") == data.name && record.at("sourceTake") == data.sourceTake &&
            record.at("skeletonSignature") == data.skeletonSignature &&
            Unsigned(record, "nodeCount") == data.nodeCount && Unsigned(record, "channelCount") == channelCount &&
            record.at("durationSeconds").is_number() && record.at("durationSeconds").get<double>() == data.durationSeconds,
            "Clip metadata/content mismatch");
        Engine::AnimationClip validated;
        std::string error;
        if (!validated.Initialize(data, error))
        {
            throw std::runtime_error("Clip curve validation failed: " + error);
        }
        return data;
    }

    Engine::ModelData ReadBinary(const std::vector<std::uint8_t>& storage,
        const std::filesystem::path& bundle,
        const std::vector<std::filesystem::path>& textures)
    {
        const std::span<const std::uint8_t> bytes(storage);
        Require(bytes.size() >= kHeaderBytes, "Invalid header length");
        Require(bytes[0] == 'E' && bytes[1] == 'M' && bytes[2] == 'S' && bytes[3] == 'H',
            "Invalid EMSH magic");
        std::size_t offset = 4;
        const auto version = ReadU32(bytes, offset);
        Require(version == 1 || version == 2, "Unsupported EMSH version");
        Require(ReadU32(bytes, offset) == kHeaderBytes, "Unsupported header length");
        Require(ReadU32(bytes, offset) == version, "Unsupported model flags");
        Require(ReadU64(bytes, offset) == bytes.size(), "Declared file length mismatch");
        const auto nodeCount = ReadU32(bytes, offset);
        const auto materialCount = ReadU32(bytes, offset);
        const auto meshCount = ReadU32(bytes, offset);
        const auto skinCount = ReadU32(bytes, offset);
        Require(version != 1 || skinCount == 0, "Skin data is unsupported in static EMSH v1");
        Require(skinCount <= meshCount, "Invalid skin count for EMSH version");
        const auto nodeBytes = ReadU64(bytes, offset);
        const auto materialBytes = ReadU64(bytes, offset);
        const auto meshBytes = ReadU64(bytes, offset);
        Require(nodeCount > 0 && nodeCount <= 4096 && materialCount > 0 &&
            materialCount <= 1024 && meshCount > 0 && meshCount <= 4096, "Invalid model counts");
        auto remaining = static_cast<std::uint64_t>(bytes.size() - kHeaderBytes);
        Require(nodeBytes <= remaining, "Invalid node section length");
        remaining -= nodeBytes;
        Require(materialBytes <= remaining, "Invalid material section length");
        remaining -= materialBytes;
        Require(meshBytes == remaining, "Invalid mesh section length");

        Engine::ModelData model;
        model.nodes.reserve(nodeCount);
        model.materials.reserve(materialCount);
        model.meshes.reserve(meshCount);
        const auto nodeEnd = offset + static_cast<std::size_t>(nodeBytes);
        const auto nodeSpan = bytes.first(nodeEnd);
        for (std::uint32_t index = 0; index < nodeCount; ++index)
        {
            Engine::NodeData node;
            node.name = ReadString(nodeSpan, offset);
            node.parent = std::bit_cast<std::int32_t>(ReadU32(nodeSpan, offset));
            Require(node.parent == -1 || (node.parent >= 0 &&
                static_cast<std::uint32_t>(node.parent) < index), "Invalid node parent order");
            for (auto& row : node.bindLocal.m)
            {
                for (auto& value : row)
                {
                    value = ReadFloat(nodeSpan, offset);
                }
            }
            Require(node.bindLocal._14 == 0.0f && node.bindLocal._24 == 0.0f &&
                node.bindLocal._34 == 0.0f && node.bindLocal._44 == 1.0f,
                "Invalid affine node matrix");
            const auto& m = node.bindLocal;
            const double determinant =
                static_cast<double>(m._11) * (static_cast<double>(m._22) * m._33 - static_cast<double>(m._23) * m._32) -
                static_cast<double>(m._12) * (static_cast<double>(m._21) * m._33 - static_cast<double>(m._23) * m._31) +
                static_cast<double>(m._13) * (static_cast<double>(m._21) * m._32 - static_cast<double>(m._22) * m._31);
            Require(std::isfinite(determinant) && std::abs(determinant) > 1e-12,
                "Singular node matrix");
            model.nodes.push_back(std::move(node));
        }
        Require(offset == nodeEnd, "Node section length mismatch");
        std::vector<Engine::Matrix> bindGlobals;
        bindGlobals.reserve(model.nodes.size());
        for (const auto& node : model.nodes)
        {
            const auto parent = node.parent < 0 ? DirectX::XMMatrixIdentity()
                : DirectX::XMLoadFloat4x4(&bindGlobals[static_cast<size_t>(node.parent)]);
            Engine::Matrix global;
            DirectX::XMStoreFloat4x4(&global, DirectX::XMLoadFloat4x4(&node.bindLocal) * parent);
            Require(AffineNonsingular(global), "Invalid or overflowing bind global matrix");
            bindGlobals.push_back(global);
        }
        const auto materialEnd = offset + static_cast<std::size_t>(materialBytes);
        const auto materialSpan = bytes.first(materialEnd);
        for (std::uint32_t index = 0; index < materialCount; ++index)
        {
            Engine::MaterialData material;
            material.name = ReadString(materialSpan, offset);
            material.baseColor.x = ReadFloat(materialSpan, offset);
            material.baseColor.y = ReadFloat(materialSpan, offset);
            material.baseColor.z = ReadFloat(materialSpan, offset);
            material.baseColor.w = ReadFloat(materialSpan, offset);
            Require(material.baseColor.x >= 0.0f && material.baseColor.x <= 1.0f &&
                material.baseColor.y >= 0.0f && material.baseColor.y <= 1.0f &&
                material.baseColor.z >= 0.0f && material.baseColor.z <= 1.0f &&
                material.baseColor.w == 1.0f, "Unsupported material color/alpha");
            const auto textureName = ReadString(materialSpan, offset, version == 2);
            if (!textureName.empty())
            {
                material.textureFile = ResolveFile(bundle, Utf8Path(textureName));
                Require(std::any_of(textures.begin(), textures.end(), [&material](const auto& texture)
                {
                    return EqualPath(material.textureFile, texture);
                }), "Material texture is absent from verified outputs");
            }
            model.materials.push_back(std::move(material));
        }
        Require(offset == materialEnd, "Material section length mismatch");
        std::uint64_t totalVertices = 0;
        std::uint64_t totalIndices = 0;
        uint32_t skinsRead = 0;
        for (std::uint32_t index = 0; index < meshCount; ++index)
        {
            Engine::MeshData mesh;
            mesh.name = ReadString(bytes, offset);
            mesh.nodeIndex = ReadU32(bytes, offset);
            mesh.materialIndex = ReadU32(bytes, offset);
            const auto vertexCount = ReadU32(bytes, offset);
            const auto indexCount = ReadU32(bytes, offset);
            const auto jointCount = version == 2 ? ReadU32(bytes, offset) : 0;
            Require(jointCount <= Engine::kMaxSkinJoints, "Skin exceeds the joint limit");
            Require(mesh.nodeIndex < nodeCount && mesh.materialIndex < materialCount,
                "Invalid mesh node/material index");
            Require(vertexCount > 0 && vertexCount <= kMaximumVertices &&
                indexCount > 0 && indexCount <= kMaximumIndices && indexCount % 3 == 0,
                "Invalid vertex/index counts");
            totalVertices += vertexCount;
            totalIndices += indexCount;
            Require(totalVertices <= kMaximumVertices && totalIndices <= kMaximumIndices,
                "Total model count limit exceeded");
            const std::uint64_t required = static_cast<std::uint64_t>(vertexCount) * 32 +
                static_cast<std::uint64_t>(indexCount) * 4 + static_cast<uint64_t>(jointCount) * 68 +
                (jointCount > 0 ? static_cast<uint64_t>(vertexCount) * 32 : 0);
            Require(offset <= bytes.size() && required <= bytes.size() - offset,
                "Truncated mesh section length");
            mesh.vertices.reserve(vertexCount);
            mesh.indices.reserve(indexCount);
            for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
            {
                Engine::Vertex vertex;
                vertex.position.x = ReadFloat(bytes, offset);
                vertex.position.y = ReadFloat(bytes, offset);
                vertex.position.z = ReadFloat(bytes, offset);
                vertex.normal.x = ReadFloat(bytes, offset);
                vertex.normal.y = ReadFloat(bytes, offset);
                vertex.normal.z = ReadFloat(bytes, offset);
                vertex.uv.x = ReadFloat(bytes, offset);
                vertex.uv.y = ReadFloat(bytes, offset);
                const double normalLength = static_cast<double>(vertex.normal.x) * vertex.normal.x +
                    static_cast<double>(vertex.normal.y) * vertex.normal.y +
                    static_cast<double>(vertex.normal.z) * vertex.normal.z;
                Require(std::abs(normalLength - 1.0) <= 0.01, "Invalid unit normal");
                mesh.vertices.push_back(vertex);
            }
            for (std::uint32_t indexNumber = 0; indexNumber < indexCount; ++indexNumber)
            {
                const auto vertexIndex = ReadU32(bytes, offset);
                Require(vertexIndex < vertexCount, "Mesh vertex index outside bounds");
                mesh.indices.push_back(vertexIndex);
            }
            for (std::size_t triangle = 0; triangle < mesh.indices.size(); triangle += 3)
            {
                Require(mesh.indices[triangle] != mesh.indices[triangle + 1] &&
                    mesh.indices[triangle] != mesh.indices[triangle + 2] &&
                    mesh.indices[triangle + 1] != mesh.indices[triangle + 2],
                    "Degenerate triangle index");
            }
            if (jointCount > 0)
            {
                ++skinsRead;
                mesh.skin.emplace();
                for (uint32_t joint = 0; joint < jointCount; ++joint)
                {
                    const auto node = ReadU32(bytes, offset);
                    Require(node < nodeCount, "Skin joint node is outside the hierarchy");
                    Require(std::find(mesh.skin->jointNodeIndices.begin(), mesh.skin->jointNodeIndices.end(), node) ==
                        mesh.skin->jointNodeIndices.end(), "Duplicate skin joint node");
                    mesh.skin->jointNodeIndices.push_back(node);
                    Engine::Matrix inverseBind;
                    for (auto& row : inverseBind.m)
                    {
                        for (auto& value : row)
                        {
                            value = ReadFloat(bytes, offset);
                        }
                    }
                    Require(inverseBind._14 == 0 && inverseBind._24 == 0 && inverseBind._34 == 0 &&
                        inverseBind._44 == 1, "Invalid inverse bind matrix");
                    Require(AffineNonsingular(inverseBind), "Singular or nonfinite inverse bind matrix");
                    Engine::Matrix meshBind;
                    DirectX::XMStoreFloat4x4(&meshBind, DirectX::XMLoadFloat4x4(&inverseBind) *
                        DirectX::XMLoadFloat4x4(&bindGlobals[node]));
                    Require(AffineNonsingular(meshBind), "Invalid inverse bind composition");
                    for (size_t row = 0; row < 4; ++row)
                    {
                        for (size_t column = 0; column < 4; ++column)
                        {
                            const float expected = bindGlobals[mesh.nodeIndex].m[row][column];
                            Require(std::abs(static_cast<double>(meshBind.m[row][column]) - expected) <=
                                0.0001 * std::max(1.0, std::abs(static_cast<double>(expected))),
                                "Inverse bind disagrees with mesh and joint bind worlds");
                        }
                    }
                    mesh.skin->inverseBind.push_back(inverseBind);
                }
                mesh.influences.resize(vertexCount);
                for (auto& influence : mesh.influences)
                {
                    for (auto& joint : influence.joints)
                    {
                        joint = ReadU32(bytes, offset);
                        Require(joint < jointCount, "Vertex joint index is outside its skin binding");
                    }
                    influence.weights = {ReadFloat(bytes, offset), ReadFloat(bytes, offset),
                        ReadFloat(bytes, offset), ReadFloat(bytes, offset)};
                    const auto& w = influence.weights;
                    Require(w.x >= 0 && w.y >= 0 && w.z >= 0 && w.w >= 0 &&
                        std::abs(static_cast<double>(w.x) + w.y + w.z + w.w - 1.0) <= 0.0001,
                        "Invalid vertex weights");
                }
            }
            model.meshes.push_back(std::move(mesh));
        }
        Require(offset == bytes.size() && skinsRead == skinCount,
            "Mesh section, skin count or trailing file length mismatch");
        return model;
    }
}

namespace Engine
{
    bool ComputeSkeletonSignature(const ModelData& data, std::string& out, std::string& error)
    {
        error.clear();
        try
        {
            Require(!data.nodes.empty() && data.nodes.size() <= 4096 &&
                !data.meshes.empty() && data.meshes.size() <= 4096, "Invalid signature counts");
            std::vector<uint8_t> bytes{'L', 'F', 'S', 'K', '1'};
            AppendU32(bytes, static_cast<uint32_t>(data.nodes.size()));
            for (size_t index = 0; index < data.nodes.size(); ++index)
            {
                const auto& node = data.nodes[index];
                Require(node.parent == -1 || (node.parent >= 0 && static_cast<size_t>(node.parent) < index),
                    "Invalid signature hierarchy");
                AppendString(bytes, node.name);
                AppendU32(bytes, std::bit_cast<uint32_t>(node.parent));
                AppendMatrix(bytes, node.bindLocal);
            }
            AppendU32(bytes, static_cast<uint32_t>(data.meshes.size()));
            for (const auto& mesh : data.meshes)
            {
                Require(mesh.nodeIndex < data.nodes.size(), "Invalid signature mesh node");
                AppendU32(bytes, mesh.nodeIndex);
                const size_t count = mesh.skin ? mesh.skin->jointNodeIndices.size() : 0;
                Require(count <= kMaxSkinJoints && (!mesh.skin ||
                    (count > 0 && count == mesh.skin->inverseBind.size())), "Invalid signature skin binding");
                AppendU32(bytes, static_cast<uint32_t>(count));
                for (size_t joint = 0; joint < count; ++joint)
                {
                    Require(mesh.skin->jointNodeIndices[joint] < data.nodes.size(), "Invalid signature joint node");
                    AppendU32(bytes, mesh.skin->jointNodeIndices[joint]);
                    AppendMatrix(bytes, mesh.skin->inverseBind[joint]);
                }
            }
            auto signature = Sha256(bytes);
            out = std::move(signature);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("ComputeSkeletonSignature: ") + exception.what();
            return false;
        }
    }

    bool ReadModelCache(const std::filesystem::path& resourceRoot,
        const std::filesystem::path& relativeFile, ModelData& out, std::string& error)
    {
        error.clear();
        try
        {
            const auto root = std::filesystem::canonical(resourceRoot);
            Require(std::filesystem::is_directory(root), "Resource root is not a directory");
            const auto modelFile = ResolveFile(root, relativeFile);
            Require(modelFile.extension() == L".mesh", "Unsupported model extension");
            const auto bundle = modelFile.parent_path();
            const auto metaFile = ResolveFile(bundle, std::filesystem::path(L"asset.meta.json"));
            const Json metadata = ReadMetadata(metaFile);
            const auto sidecarVersion = Unsigned(metadata, "sidecarVersion");
            const auto formatVersion = Unsigned(metadata, "formatVersion");
            Require(metadata.is_object() && ((sidecarVersion == 1 && formatVersion == 1) ||
                (sidecarVersion == 2 && formatVersion == 2)), "Unsupported metadata version");
            Require(metadata.at("staticOnly").is_boolean() &&
                metadata.at("staticOnly").get<bool>() == (formatVersion == 1) &&
                (formatVersion == 2 || Unsigned(metadata, "skinCount") == 0) &&
                (metadata.at("nameProfile") == "preserve" ||
                    (formatVersion == 2 && metadata.at("nameProfile") == "mixamo")), "Unsupported model import scope");
            if (sidecarVersion == 2)
            {
                Require(Unsigned(metadata, "clipFormatVersion") == 1 &&
                    IsHash(metadata.at("skeletonSignature").get<std::string>()), "Invalid animation metadata");
            }
            const auto& coordinates = metadata.at("runtimeCoordinates");
            Require(coordinates.at("handedness") == "LH" && coordinates.at("up") == "+Y" &&
                coordinates.at("front") == "+Z" && coordinates.at("uvOrigin") == "top_left" &&
                coordinates.at("unitsPerMeter").is_number() &&
                coordinates.at("unitsPerMeter").get<double>() == 1.0, "Unsupported runtime coordinates");
            const auto generation = metadata.at("generation").get<std::string>();
            Require(IsHash(generation) && bundle.filename() == std::filesystem::path(generation),
                "Invalid generation directory");
            const auto& converter = metadata.at("converter");
            const auto converterHash = converter.at("sha256").get<std::string>();
            Require(IsHash(converterHash) && Unsigned(converter, "revision") > 0,
                "Invalid converter fingerprint");
            const auto& inputs = metadata.at("inputs");
            Require(inputs.is_array() && !inputs.empty() && inputs.size() <= 64,
                "Invalid input fingerprint list");
            for (const auto& input : inputs)
            {
                const auto inputPath = Utf8Path(input.at("path").get<std::string>());
                Require(!inputPath.is_absolute() && !inputPath.has_root_name() &&
                    !inputPath.has_root_directory(),
                    "Invalid source fingerprint path");
                for (const auto& component : inputPath)
                {
                    Require(component != L".." && component != L"." &&
                        component.native().find(L':') == std::wstring::npos,
                        "Unsafe source fingerprint path");
                }
                Require(Unsigned(input, "bytes") > 0 && Unsigned(input, "bytes") <= kMaximumBytes &&
                    IsHash(input.at("sha256").get<std::string>()), "Invalid input fingerprint");
            }
            Require(IsHash(metadata.at("importOptionsSha256").get<std::string>()) &&
                JsonHash(metadata.at("importOptions")) ==
                    metadata.at("importOptionsSha256").get<std::string>(),
                "Import options hash mismatch");
            const Json generationInput = {
                {"inputs", inputs}, {"importOptions", metadata.at("importOptions")},
                {"source", metadata.at("source")},
                {"converterRevision", converter.at("revision")},
                {"converterSha256", converterHash},
                {"sidecarVersion", metadata.at("sidecarVersion")},
                {"formatVersion", metadata.at("formatVersion")}
            };
            Require(JsonHash(generationInput) == generation, "Generation input hash mismatch");
            const auto& outputs = metadata.at("outputs");
            Require(outputs.is_array() && !outputs.empty() && outputs.size() <= 64,
                "Invalid output list");
            std::vector<std::filesystem::path> verifiedFiles;
            std::vector<std::filesystem::path> textures;
            std::vector<std::uint8_t> modelBytes;
            std::vector<std::pair<std::filesystem::path, std::vector<std::uint8_t>>> clipBytes;
            std::uint64_t totalBytes = 0;
            for (const auto& output : outputs)
            {
                const auto path = ResolveFile(bundle, Utf8Path(output.at("path").get<std::string>()));
                Require(!EqualPath(path, metaFile), "Sidecar must not hash itself");
                Require(std::none_of(verifiedFiles.begin(), verifiedFiles.end(),
                    [&path](const auto& previous)
                    {
                        return EqualPath(path, previous);
                    }), "Duplicate output path");
                verifiedFiles.push_back(path);
                const auto expectedBytes = Unsigned(output, "bytes");
                Require(expectedBytes > 0 && expectedBytes <= kMaximumBytes - totalBytes,
                    "Output bundle size limit exceeded");
                totalBytes += expectedBytes;
                auto bytes = ReadFile(path, kMaximumBytes);
                Require(bytes.size() == expectedBytes, "Output file length mismatch");
                const auto expectedHash = output.at("sha256").get<std::string>();
                Require(IsHash(expectedHash) && Sha256(bytes) == expectedHash, "Output SHA-256 hash mismatch");
                const auto kind = output.at("kind").get<std::string>();
                Require(kind == "model" || kind == "texture" || kind == "license" ||
                    (sidecarVersion == 2 && kind == "clip"),
                    "Unsupported output kind");
                if (EqualPath(path, modelFile))
                {
                    Require(kind == "model" && modelBytes.empty(), "Model output kind mismatch");
                    modelBytes = std::move(bytes);
                }
                else if (kind == "model")
                {
                    Require(false, "Static bundle supports one model output");
                }
                else if (kind == "texture")
                {
                    textures.push_back(path);
                }
                else if (kind == "clip")
                {
                    clipBytes.emplace_back(path, std::move(bytes));
                }
            }
            Require(!modelBytes.empty() && (formatVersion == 2 || !textures.empty()), "Missing model/texture outputs");
            size_t versionOffset = 4;
            Require(ReadU32(modelBytes, versionOffset) == formatVersion, "Metadata/model version mismatch");
            ModelData model = ReadBinary(modelBytes, bundle, textures);
            std::uint64_t vertices = 0;
            std::uint64_t indices = 0;
            for (const auto& mesh : model.meshes)
            {
                vertices += mesh.vertices.size();
                indices += mesh.indices.size();
            }
            const auto& counts = metadata.at("counts");
            Require(Unsigned(counts, "nodes") == model.nodes.size() &&
                Unsigned(counts, "materials") == model.materials.size() &&
                Unsigned(counts, "meshes") == model.meshes.size() &&
                Unsigned(counts, "vertices") == vertices && Unsigned(counts, "indices") == indices,
                "Metadata/model count mismatch");
            size_t actualSkinCount = 0;
            for (const auto& mesh : model.meshes)
            {
                actualSkinCount += mesh.skin.has_value();
            }
            Require(Unsigned(metadata, "skinCount") == actualSkinCount, "Metadata/model skin count mismatch");
            if (formatVersion == 2)
            {
                std::string signature;
                if (!ComputeSkeletonSignature(model, signature, error))
                {
                    throw std::runtime_error("Cannot compute model skeleton signature: " + error);
                }
                Require(signature == metadata.at("skeletonSignature").get<std::string>() &&
                    Unsigned(counts, "skins") == actualSkinCount, "Model skeleton signature or skin count mismatch");
                const auto& clips = metadata.at("clips");
                Require(clips.is_array() && clips.size() <= 64, "Invalid clip record list");
                std::vector<std::filesystem::path> clipPaths;
                for (const auto& clip : clips)
                {
                    const auto path = ResolveFile(bundle, Utf8Path(clip.at("path").get<std::string>()));
                    Require(path.extension() == L".clip" && std::none_of(clipPaths.begin(), clipPaths.end(),
                        [&path](const auto& previous)
                        {
                            return EqualPath(path, previous);
                        }) &&
                        clip.at("skeletonSignature").get<std::string>() == signature && Unsigned(clip, "nodeCount") == model.nodes.size(),
                        "Invalid clip signature, node count or duplicate name");
                    clipPaths.push_back(path);
                    Require(std::any_of(outputs.begin(), outputs.end(), [&clip](const auto& output)
                        {
                            return output.at("kind") == "clip" && output.at("path") == clip.at("path");
                        }), "Clip record missing from verified outputs");
                    const auto verified = std::find_if(clipBytes.begin(), clipBytes.end(),
                        [&path](const auto& output)
                        {
                            return EqualPath(path, output.first);
                        });
                    Require(verified != clipBytes.end(), "Clip record has no verified binary bytes");
                    static_cast<void>(ReadClipBinary(verified->second, model, clip, signature));
                }
                Require(static_cast<size_t>(std::count_if(outputs.begin(), outputs.end(), [](const auto& output)
                    {
                        return output.at("kind") == "clip";
                    })) == clips.size(), "Clip output/record count mismatch");
            }
            out = std::move(model);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("ReadModelCache: ") + exception.what();
        }
        catch (...)
        {
            error = "ReadModelCache: unknown failure";
        }
        return false;
    }

    bool ReadClipCache(const std::filesystem::path& resourceRoot,
        const std::filesystem::path& relativeFile, ClipData& out, std::string& error)
    {
        error.clear();
        try
        {
            const auto root = std::filesystem::canonical(resourceRoot);
            const auto file = ResolveFile(root, relativeFile);
            Require(file.extension() == L".clip", "Unsupported animation extension");
            auto bundle = file.parent_path();
            while (!std::filesystem::exists(bundle / L"asset.meta.json"))
            {
                Require(!EqualPath(bundle, root), "Animation bundle sidecar is missing within the resource root");
                bundle = bundle.parent_path();
                Require(ContainsPath(root, bundle), "Animation bundle escapes the resource root");
            }
            const auto metaFile = ResolveFile(bundle, std::filesystem::path(L"asset.meta.json"));
            const auto metadata = ReadMetadata(metaFile);
            Require(Unsigned(metadata, "sidecarVersion") == 2 && Unsigned(metadata, "formatVersion") == 2 &&
                Unsigned(metadata, "clipFormatVersion") == 1, "Unsupported animation bundle version");
            std::filesystem::path modelFile;
            const auto& outputs = metadata.at("outputs");
            for (const auto& output : outputs)
            {
                if (output.at("kind") == "model")
                {
                    Require(modelFile.empty(), "Animation bundle must contain one model");
                    modelFile = ResolveFile(bundle, Utf8Path(output.at("path").get<std::string>()));
                }
            }
            Require(!modelFile.empty(), "Animation bundle model is missing");
            ModelData verifiedModel;
            if (!ReadModelCache(root, modelFile.lexically_relative(root), verifiedModel, error))
            {
                throw std::runtime_error("Animation bundle/model validation failed: " + error);
            }
            Require(ReadMetadata(metaFile) == metadata, "Animation bundle changed during validation");
            const auto selected = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output)
            {
                return output.at("kind") == "clip" &&
                    EqualPath(ResolveFile(bundle, Utf8Path(output.at("path").get<std::string>())), file);
            });
            Require(selected != outputs.end(), "Requested clip is absent from verified bundle");
            const auto storage = ReadFile(file, kMaximumBytes);
            Require(storage.size() == Unsigned(*selected, "bytes") &&
                Sha256(storage) == selected->at("sha256").get<std::string>(), "Animation file changed after bundle validation");
            const auto& records = metadata.at("clips");
            const auto record = std::find_if(records.begin(), records.end(), [&](const auto& item)
            {
                return EqualPath(ResolveFile(bundle, Utf8Path(item.at("path").get<std::string>())), file);
            });
            Require(record != records.end(), "Clip metadata record is missing");
            ClipData data = ReadClipBinary(storage, verifiedModel, *record,
                metadata.at("skeletonSignature").get<std::string>());
            out = std::move(data);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("ReadClipCache: ") + exception.what();
            return false;
        }
        catch (...)
        {
            error = "ReadClipCache: unknown failure";
            return false;
        }
    }
}
