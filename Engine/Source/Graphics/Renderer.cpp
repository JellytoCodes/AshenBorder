#include "Graphics/Renderer.h"

#include "Core/Log.h"
#include "Animation/AnimationData.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Model.h"
#include "Graphics/RenderFrame.h"
#include "UI/UiRenderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Engine
{
    struct Renderer::PreparedMesh
    {
        const DrawItem* item = nullptr;
        const MeshGpu* mesh = nullptr;
        const Material* material = nullptr;
        size_t meshIndex = 0;
        Matrix world;
        Matrix normalWorld;
        Vector4 color;
        Vector4 blendColor;
        bool mirrored = false;
        bool forwardVisible = true;
        bool shadowVisible = true;
    };
}

namespace
{
    struct alignas(16) DrawConstants
    {
        Engine::Matrix world;
        Engine::Matrix normalWorld;
        Engine::Matrix viewProjection;
        Engine::Vector4 cameraPosition;
        Engine::Vector4 lightDirectionAmbient;
        Engine::Vector4 lightColorIntensity;
        Engine::Vector4 materialColor;
        Engine::Vector4 blendColor;
        Engine::Vector4 blendParams;
    };

    struct alignas(16) SkinConstants
    {
        std::array<Engine::Matrix, Engine::kMaxSkinJoints> palettes;
    };

    struct alignas(16) EffectsConstants
    {
        Engine::Matrix lightViewProjection;
        Engine::Vector4 shadowParams;
        Engine::Vector4 objectParams;
        Engine::Vector4 specularParams;
    };

    struct alignas(16) ParticleConstants
    {
        Engine::Matrix viewProjection;
        Engine::Vector4 cameraRight;
        Engine::Vector4 cameraUp;
        Engine::Vector4 cameraPosition;
    };

    static_assert(sizeof(Engine::ParticleDraw) == 64);
    static_assert(std::is_trivially_copyable_v<Engine::ParticleDraw>);
    static_assert(sizeof(ParticleConstants) == 112);

    struct InstanceData
    {
        Engine::Matrix world;
        Engine::Matrix normalWorld;
        Engine::Vector4 tint;
        Engine::Vector4 effects;
    };

    static_assert(std::is_standard_layout_v<Engine::Vertex>);
    static_assert(sizeof(Engine::Vertex) == 32);
    static_assert(offsetof(Engine::Vertex, position) == 0);
    static_assert(offsetof(Engine::Vertex, normal) == 12);
    static_assert(offsetof(Engine::Vertex, uv) == 24);
    static_assert(std::is_standard_layout_v<Engine::SkinnedVertex>);
    static_assert(sizeof(Engine::SkinnedVertex) == 64);
    static_assert(offsetof(Engine::SkinnedVertex, vertex) == 0);
    static_assert(offsetof(Engine::SkinnedVertex, influence) == 32);
    static_assert(offsetof(Engine::VertexInfluence, joints) == 0);
    static_assert(offsetof(Engine::VertexInfluence, weights) == 16);
    static_assert(sizeof(SkinConstants) == 8192);
    static_assert(sizeof(SkinConstants) % 16 == 0);
    static_assert(sizeof(EffectsConstants) == 112);
    static_assert(offsetof(EffectsConstants, shadowParams) == 64);
    static_assert(offsetof(EffectsConstants, objectParams) == 80);
    static_assert(offsetof(EffectsConstants, specularParams) == 96);
    static_assert(sizeof(InstanceData) == 160);
    static_assert(offsetof(InstanceData, world) == 0);
    static_assert(offsetof(InstanceData, normalWorld) == 64);
    static_assert(offsetof(InstanceData, tint) == 128);
    static_assert(offsetof(InstanceData, effects) == 144);
    static_assert(std::is_standard_layout_v<DrawConstants>);
    static_assert(std::is_trivially_copyable_v<DrawConstants>);
    static_assert(sizeof(Engine::Matrix) == 64);
    static_assert(sizeof(DrawConstants) == 288);
    static_assert(sizeof(DrawConstants) % 16 == 0);
    static_assert(offsetof(DrawConstants, world) == 0);
    static_assert(offsetof(DrawConstants, normalWorld) == 64);
    static_assert(offsetof(DrawConstants, viewProjection) == 128);
    static_assert(offsetof(DrawConstants, cameraPosition) == 192);
    static_assert(offsetof(DrawConstants, lightDirectionAmbient) == 208);
    static_assert(offsetof(DrawConstants, lightColorIntensity) == 224);
    static_assert(offsetof(DrawConstants, materialColor) == 240);
    static_assert(offsetof(DrawConstants, blendColor) == 256);
    static_assert(offsetof(DrawConstants, blendParams) == 272);

    bool IsFinite(const Engine::Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFinite(const Engine::Vector4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z) && std::isfinite(value.w);
    }

    bool IsFinite(const Engine::Matrix& value)
    {
        for (const auto& row : value.m)
        {
            for (const float component : row)
            {
                if (!std::isfinite(component))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsIdentity(const Engine::Matrix& matrix)
    {
        for (size_t row = 0; row < 4; ++row)
        {
            for (size_t column = 0; column < 4; ++column)
            {
                if (matrix.m[row][column] != (row == column ? 1.0f : 0.0f))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool CullingScalarSafe(float value)
    {
        const double magnitude = std::abs(static_cast<double>(value));
        return std::isfinite(value) && (value == 0 || (magnitude >= 1e-12 && magnitude <= 1e12));
    }

    bool CullingMatrixSafe(const Engine::Matrix& matrix)
    {
        for (const auto& row : matrix.m)
        {
            for (const float component : row)
            {
                if (!CullingScalarSafe(component))
                {
                    return false;
                }
            }
        }
        return true;
    }

    struct WorldClipBounds
    {
        std::array<std::array<double, 4>, 8> placed{};
        std::array<std::array<double, 4>, 8> magnitude{};
        uint32_t ready = 0;
        bool safe = false;
    };

    WorldClipBounds PrepareClipBounds(const Engine::Vector3& minimum, const Engine::Vector3& maximum,
        const Engine::Matrix& world)
    {
        WorldClipBounds result;
        const std::array<float, 6> values{minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z};
        if (!std::all_of(values.begin(), values.end(), CullingScalarSafe) || !CullingMatrixSafe(world) ||
            !IsFinite(minimum) || !IsFinite(maximum) || minimum.x > maximum.x ||
            minimum.y > maximum.y || minimum.z > maximum.z)
        {
            return result;
        }
        result.safe = true;
        return result;
    }

    bool BoundsIntersectClip(const Engine::Vector3& minimum, const Engine::Vector3& maximum,
        const Engine::Matrix& world, const Engine::Matrix& viewProjection,
        bool projectionSafe, WorldClipBounds& bounds)
    {
        // Retain the original finite/FTZ/cancellation policy. Only this DrawItem's world
        // corners are shared between passes; lazy preparation preserves early-keep work.
        if (!projectionSafe || !bounds.safe)
        {
            return true;
        }
        constexpr double kRelativeError = 64.0 * std::numeric_limits<float>::epsilon();
        constexpr double kFloatMaximum = static_cast<double>(std::numeric_limits<float>::max());
        uint32_t commonOutside = 0x3f;
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            auto& placed = bounds.placed[corner];
            auto& placedMagnitude = bounds.magnitude[corner];
            if (!(bounds.ready & (1u << corner)))
            {
                const std::array<double, 4> point{
                    (corner & 1) != 0 ? maximum.x : minimum.x,
                    (corner & 2) != 0 ? maximum.y : minimum.y,
                    (corner & 4) != 0 ? maximum.z : minimum.z, 1.0};
                for (size_t column = 0; column < 4; ++column)
                {
                    for (size_t row = 0; row < 4; ++row)
                    {
                        const double term = point[row] * world.m[row][column];
                        placed[column] += term;
                        placedMagnitude[column] += std::abs(term);
                    }
                    if (!std::isfinite(placed[column]) || !std::isfinite(placedMagnitude[column]) ||
                        std::abs(placed[column]) > kFloatMaximum)
                    {
                        bounds.safe = false;
                        return true;
                    }
                }
                bounds.ready |= 1u << corner;
            }
            std::array<double, 4> clip{}, clipMagnitude{}, error{};
            for (size_t column = 0; column < 4; ++column)
            {
                for (size_t row = 0; row < 4; ++row)
                {
                    const double coefficient = viewProjection.m[row][column];
                    clip[column] += placed[row] * coefficient;
                    // Keep per-pass magnitude/error propagation after cancellation.
                    clipMagnitude[column] += placedMagnitude[row] * std::abs(coefficient);
                }
                if (!std::isfinite(clip[column]) || !std::isfinite(clipMagnitude[column]) ||
                    std::abs(clip[column]) > kFloatMaximum)
                {
                    return true;
                }
                error[column] = kRelativeError * std::max(1.0, clipMagnitude[column]);
            }
            const double x = clip[0], y = clip[1], z = clip[2], w = clip[3];
            uint32_t outside = 0;
            outside |= x + w < -(error[0] + error[3]) ? 1u : 0u;
            outside |= w - x < -(error[0] + error[3]) ? 2u : 0u;
            outside |= y + w < -(error[1] + error[3]) ? 4u : 0u;
            outside |= w - y < -(error[1] + error[3]) ? 8u : 0u;
            outside |= z < -error[2] ? 16u : 0u;
            outside |= w - z < -(error[3] + error[2]) ? 32u : 0u;
            commonOutside &= outside;
            if (commonOutside == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool TryNormalWorld(const Engine::Matrix& world, Engine::Matrix& normalWorld, bool& mirrored)
    {
        if (world._14 != 0.0f || world._24 != 0.0f || world._34 != 0.0f || world._44 != 1.0f)
        {
            return false;
        }
        const double a = world._11;
        const double b = world._12;
        const double c = world._13;
        const double d = world._21;
        const double e = world._22;
        const double f = world._23;
        const double g = world._31;
        const double h = world._32;
        const double i = world._33;
        const std::array<double, 9> cofactor = {
            e * i - f * h, f * g - d * i, d * h - e * g,
            c * h - b * i, a * i - c * g, b * g - a * h,
            b * f - c * e, c * d - a * f, a * e - b * d};
        const double determinant = a * cofactor[0] + b * cofactor[1] + c * cofactor[2];
        if (!std::isfinite(determinant) || determinant == 0.0)
        {
            return false;
        }

        std::array<double, 9> inverseTranspose;
        double largest = 0.0;
        for (size_t index = 0; index < cofactor.size(); ++index)
        {
            const double value = cofactor[index] / determinant;
            if (!std::isfinite(value) ||
                std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                return false;
            }
            inverseTranspose[index] = value;
            if (std::abs(value) > largest)
            {
                largest = std::abs(value);
            }
        }
        if (largest == 0.0)
        {
            return false;
        }

        // A common positive scale preserves normalized inverse-transpose directions.
        // Keeping the largest coefficient at 1 avoids GPU float overflow and denorm-only matrices.
        normalWorld = Engine::Matrix(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
        for (size_t row = 0; row < 3; ++row)
        {
            for (size_t column = 0; column < 3; ++column)
            {
                const double value = inverseTranspose[row * 3 + column] / largest;
                normalWorld.m[row][column] = static_cast<float>(value);
            }
        }
        mirrored = determinant < 0.0;
        return true;
    }

    bool CompileShader(const std::filesystem::path& file, const char* entry, const char* target,
        Microsoft::WRL::ComPtr<ID3DBlob>& bytecode)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
        // SkinnedMesh includes the common material shader beside the selected source file.
        const HRESULT result = D3DCompileFromFile(file.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
            flags, 0, bytecode.ReleaseAndGetAddressOf(), diagnostics.GetAddressOf());
        if (diagnostics && diagnostics->GetBufferSize() != 0)
        {
            const auto* text = static_cast<const char*>(diagnostics->GetBufferPointer());
            size_t length = diagnostics->GetBufferSize();
            if (text[length - 1] == '\0')
            {
                --length;
            }
            Engine::Log::Write(std::string_view(text, length));
        }
        if (FAILED(result))
        {
            Engine::Log::Error("D3DCompileFromFile mesh shader", result);
            return false;
        }
        return true;
    }
}

namespace Engine
{
    Renderer::Renderer(GraphicsDevice& graphics)
        : _graphics(graphics)
    {
    }

    Renderer::~Renderer()
    {
        Shutdown();
    }

    bool Renderer::Initialize(const std::filesystem::path& shaderRoot)
    {
        if (!_graphics.Device() || !_graphics.Context() || shaderRoot.empty() || _vertexShader)
        {
            Log::Write("Renderer initialization requires a device, shader root, and unused renderer.");
            return false;
        }

        const auto shaderFile = shaderRoot / L"StaticMesh.hlsl";
        Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> skinnedBytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> instancedBytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> shadowBytecode;
        if (!CompileShader(shaderFile, "VSMain", "vs_5_0", vertexBytecode) ||
            !CompileShader(shaderFile, "PSMain", "ps_5_0", pixelBytecode) ||
            !CompileShader(shaderRoot / L"SkinnedMesh.hlsl", "VSMain", "vs_5_0", skinnedBytecode) ||
            !CompileShader(shaderRoot / L"InstancedMesh.hlsl", "VSMain", "vs_5_0", instancedBytecode) ||
            !CompileShader(shaderFile, "PSShadow", "ps_5_0", shadowBytecode))
        {
            return false;
        }

        ID3D11Device* device = _graphics.Device();
        const auto checkResult = [this](const char* operation, HRESULT result)
        {
            if (FAILED(result))
            {
                Log::Error(operation, result);
                Shutdown();
                return false;
            }
            return true;
        };
        if (!checkResult("CreateVertexShader", device->CreateVertexShader(
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), nullptr,
            _vertexShader.GetAddressOf())) ||
            !checkResult("CreatePixelShader", device->CreatePixelShader(
                pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(), nullptr,
                _pixelShader.GetAddressOf())))
        {
            return false;
        }
        if (!checkResult("CreateVertexShader skinned", device->CreateVertexShader(
            skinnedBytecode->GetBufferPointer(), skinnedBytecode->GetBufferSize(), nullptr,
            _skinnedVertexShader.GetAddressOf())))
        {
            return false;
        }
        if (!checkResult("CreateVertexShader instanced", device->CreateVertexShader(
            instancedBytecode->GetBufferPointer(), instancedBytecode->GetBufferSize(), nullptr,
            _instancedVertexShader.GetAddressOf())) ||
            !checkResult("CreatePixelShader shadow", device->CreatePixelShader(
                shadowBytecode->GetBufferPointer(), shadowBytecode->GetBufferSize(), nullptr,
                _shadowPixelShader.GetAddressOf())))
        {
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        if (!checkResult("CreateInputLayout", device->CreateInputLayout(elements, 3,
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
            _inputLayout.GetAddressOf())))
        {
            return false;
        }
        const D3D11_INPUT_ELEMENT_DESC skinElements[] = {
            elements[0], elements[1], elements[2],
            {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        if (!checkResult("CreateInputLayout skinned", device->CreateInputLayout(skinElements, 5,
            skinnedBytecode->GetBufferPointer(), skinnedBytecode->GetBufferSize(),
            _skinnedInputLayout.GetAddressOf())))
        {
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC instanceElements[] = {
            elements[0], elements[1], elements[2],
            {"INSTANCEWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCENORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCENORMAL", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 80, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCENORMAL", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 96, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCENORMAL", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 112, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCETINT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 128, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEEFFECTS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 144, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        if (!checkResult("CreateInputLayout instanced", device->CreateInputLayout(instanceElements, 13,
            instancedBytecode->GetBufferPointer(), instancedBytecode->GetBufferSize(), _instancedInputLayout.GetAddressOf())))
        {
            return false;
        }

        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(DrawConstants));
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!checkResult("CreateBuffer DrawConstants", device->CreateBuffer(
            &bufferDesc, nullptr, _constantBuffer.GetAddressOf())))
        {
            return false;
        }
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(SkinConstants));
        if (!checkResult("CreateBuffer SkinConstants", device->CreateBuffer(
            &bufferDesc, nullptr, _skinConstantBuffer.GetAddressOf())))
        {
            return false;
        }
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(EffectsConstants));
        if (!checkResult("CreateBuffer EffectsConstants", device->CreateBuffer(
            &bufferDesc, nullptr, _effectsConstantBuffer.GetAddressOf())))
        {
            return false;
        }
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(InstanceData) * kMaxInstancesPerBatch);
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (!checkResult("CreateBuffer instances", device->CreateBuffer(
            &bufferDesc, nullptr, _instanceBuffer.GetAddressOf())))
        {
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (!checkResult("CreateSamplerState", device->CreateSamplerState(
            &samplerDesc, _sampler.GetAddressOf())))
        {
            return false;
        }

        D3D11_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_BACK;
        rasterizerDesc.DepthClipEnable = TRUE;
        if (!checkResult("CreateRasterizerState", device->CreateRasterizerState(
            &rasterizerDesc, _rasterizer.GetAddressOf())))
        {
            return false;
        }
        rasterizerDesc.FrontCounterClockwise = TRUE;
        if (!checkResult("CreateRasterizerState mirrored", device->CreateRasterizerState(
            &rasterizerDesc, _mirroredRasterizer.GetAddressOf())))
        {
            return false;
        }
        rasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
        rasterizerDesc.FrontCounterClockwise = FALSE;
        if (!checkResult("CreateRasterizerState wireframe", device->CreateRasterizerState(
            &rasterizerDesc, _wireframeRasterizer.GetAddressOf())))
        {
            return false;
        }
        rasterizerDesc.FrontCounterClockwise = TRUE;
        if (!checkResult("CreateRasterizerState mirrored wireframe", device->CreateRasterizerState(
            &rasterizerDesc, _mirroredWireframeRasterizer.GetAddressOf())))
        {
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depthDesc = {};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        depthDesc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depthDesc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        depthDesc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
            D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        depthDesc.BackFace = depthDesc.FrontFace;
        if (!checkResult("CreateDepthStencilState", device->CreateDepthStencilState(
            &depthDesc, _depthState.GetAddressOf())))
        {
            return false;
        }

        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (!checkResult("CreateBlendState", device->CreateBlendState(
            &blendDesc, _blendState.GetAddressOf())))
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC shadowDesc{};
        shadowDesc.Width = kShadowMapResolution;
        shadowDesc.Height = kShadowMapResolution;
        shadowDesc.MipLevels = 1;
        shadowDesc.ArraySize = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc.Count = 1;
        shadowDesc.Usage = D3D11_USAGE_DEFAULT;
        shadowDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC shadowDepth{};
        shadowDepth.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDepth.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC shadowResource{};
        shadowResource.Format = DXGI_FORMAT_R32_FLOAT;
        shadowResource.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        shadowResource.Texture2D.MipLevels = 1;
        if (!checkResult("Create shadow texture", device->CreateTexture2D(&shadowDesc, nullptr, _shadowTexture.GetAddressOf())) ||
            !checkResult("Create shadow DSV", device->CreateDepthStencilView(
                _shadowTexture.Get(), &shadowDepth, _shadowDepthView.GetAddressOf())) ||
            !checkResult("Create shadow SRV", device->CreateShaderResourceView(
                _shadowTexture.Get(), &shadowResource, _shadowResourceView.GetAddressOf())))
        {
            return false;
        }
        D3D11_SAMPLER_DESC comparison{};
        comparison.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        comparison.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        comparison.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        comparison.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        comparison.MaxAnisotropy = 1;
        comparison.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        comparison.BorderColor[0] = comparison.BorderColor[1] = comparison.BorderColor[2] = comparison.BorderColor[3] = 1;
        comparison.MaxLOD = D3D11_FLOAT32_MAX;
        if (!checkResult("Create shadow sampler", device->CreateSamplerState(&comparison, _shadowSampler.GetAddressOf())))
        {
            return false;
        }
        Microsoft::WRL::ComPtr<ID3DBlob> particleVertex, particlePixel;
        if (!CompileShader(shaderRoot / L"Particle.hlsl", "VSMain", "vs_5_0", particleVertex) ||
            !CompileShader(shaderRoot / L"Particle.hlsl", "PSMain", "ps_5_0", particlePixel))
        {
            Shutdown();
            return false;
        }
        const D3D11_INPUT_ELEMENT_DESC particleElements[] = {
            {"PARTICLEPOSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"PARTICLESIZE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"PARTICLECOLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"PARTICLEAXIS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        bufferDesc.ByteWidth = sizeof(ParticleDraw) * kMaxParticleInstances;
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_NONE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        if (!checkResult("Create particle VS", device->CreateVertexShader(particleVertex->GetBufferPointer(),
                particleVertex->GetBufferSize(), nullptr, _particleVertexShader.GetAddressOf())) ||
            !checkResult("Create particle PS", device->CreatePixelShader(particlePixel->GetBufferPointer(),
                particlePixel->GetBufferSize(), nullptr, _particlePixelShader.GetAddressOf())) ||
            !checkResult("Create particle layout", device->CreateInputLayout(particleElements, 4,
                particleVertex->GetBufferPointer(), particleVertex->GetBufferSize(), _particleInputLayout.GetAddressOf())) ||
            !checkResult("Create particle instances", device->CreateBuffer(&bufferDesc, nullptr, _particleBuffer.GetAddressOf())) ||
            !checkResult("Create particle rasterizer", device->CreateRasterizerState(&rasterizerDesc, _particleRasterizer.GetAddressOf())) ||
            !checkResult("Create particle depth", device->CreateDepthStencilState(&depthDesc, _particleDepth.GetAddressOf())) ||
            !checkResult("Create particle blend", device->CreateBlendState(&blendDesc, _particleBlend.GetAddressOf())))
        {
            return false;
        }
        bufferDesc.ByteWidth = sizeof(ParticleConstants);
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!checkResult("Create particle constants", device->CreateBuffer(&bufferDesc, nullptr, _particleConstants.GetAddressOf())))
        {
            return false;
        }
        try
        {
            _particles.reserve(kMaxParticleInstances);
        }
        catch (...)
        {
            Shutdown();
            return false;
        }
        _uiRenderer = std::make_unique<UiRenderer>(_graphics);
        if (!_uiRenderer->Initialize(shaderRoot))
        {
            Shutdown();
            return false;
        }

        Log::Write("Renderer initialized: static/skinned/instanced, shared pose Shadow/Forward, 1024 shadow, Bayer fade, pooled particles, UI.");
        return true;
    }

    bool Renderer::Render(const RenderFrame& frame)
    {
        _prepared.clear();
        struct PreparedReset
        {
            std::vector<PreparedMesh>& prepared;
            ~PreparedReset() noexcept
            {
                prepared.clear();
            }
        };
        const PreparedReset resetPrepared{_prepared};
        if (!_vertexShader || !_pixelShader || !_shadowPixelShader || !_inputLayout || !_constantBuffer ||
            !_skinnedVertexShader || !_skinnedInputLayout || !_skinConstantBuffer || !_instancedVertexShader ||
            !_instancedInputLayout || !_instanceBuffer || !_effectsConstantBuffer || !_shadowDepthView ||
            !_shadowResourceView || !_shadowSampler || !_uiRenderer || !_sampler || !_rasterizer ||
            !_mirroredRasterizer || !_wireframeRasterizer || !_mirroredWireframeRasterizer ||
            !_particleVertexShader || !_particlePixelShader || !_particleInputLayout || !_particleBuffer ||
            !_particleConstants || !_particleRasterizer || !_particleDepth || !_particleBlend ||
            !_depthState || !_blendState || !_graphics.Device() || !_graphics.Context())
        {
            Log::Write("Renderer Render called before complete initialization.");
            return false;
        }
        const auto invalid = [](const char* message)
        {
            Log::Write(message);
            return false;
        };
        // Validate the complete snapshot before clearing or submitting any part of the frame.
        for (const float value : frame.clearColor)
        {
            if (!std::isfinite(value))
            {
                return invalid("Renderer clear color must be finite.");
            }
        }
        if (!UiRenderer::Validate(frame.ui))
        {
            return invalid("Renderer UI snapshot is invalid.");
        }
        if (frame.shadow.enabled && (!IsFinite(frame.shadow.viewProjection) ||
            !std::isfinite(frame.shadow.depthBias) || frame.shadow.depthBias < 0 || frame.shadow.depthBias > 1 ||
            !std::isfinite(frame.shadow.normalBias) || frame.shadow.normalBias < 0 || frame.shadow.pcfRadius > 2))
        {
            return invalid("Renderer shadow requires a finite matrix, nonnegative bias, and PCF radius 0..2.");
        }

        if (frame.particles.size() > kMaxParticleInstances ||
            !std::all_of(frame.particles.begin(), frame.particles.end(), ValidParticleDraw))
        {
            return invalid("Renderer particle snapshot is invalid or exceeds capacity.");
        }

        DrawConstants common{};
        EffectsConstants effects{};
        effects.lightViewProjection = frame.shadow.viewProjection;
        effects.shadowParams = {frame.shadow.enabled ? 1.0f : 0.0f, frame.shadow.depthBias,
            frame.shadow.normalBias, 1.0f / static_cast<float>(kShadowMapResolution)};
        effects.specularParams = {frame.light.specularIntensity, frame.light.specularPower, 0, 0};
        if (!frame.draws.empty() || !frame.particles.empty())
        {
            if (!IsFinite(frame.camera.position) || !IsFinite(frame.camera.view) ||
                !IsFinite(frame.camera.projection) || !IsFinite(frame.light.direction) ||
                !IsFinite(frame.light.color) || !std::isfinite(frame.light.ambient) ||
                !std::isfinite(frame.light.intensity) || !std::isfinite(frame.light.specularIntensity) ||
                frame.light.specularIntensity < 0 || !std::isfinite(frame.light.specularPower) ||
                frame.light.specularPower < 1)
            {
                return invalid("Renderer received invalid camera or light data.");
            }
            DirectX::XMStoreFloat4x4(&common.viewProjection,
                DirectX::XMLoadFloat4x4(&frame.camera.view) * DirectX::XMLoadFloat4x4(&frame.camera.projection));
            if (!IsFinite(common.viewProjection))
            {
                return invalid("Renderer view-projection multiplication is nonfinite.");
            }
            common.cameraPosition = {frame.camera.position.x, frame.camera.position.y, frame.camera.position.z, 1};
            const double x = frame.light.direction.x;
            const double y = frame.light.direction.y;
            const double z = frame.light.direction.z;
            const double length = std::sqrt(x * x + y * y + z * z);
            if (!std::isfinite(length) || length == 0)
            {
                return invalid("Renderer directional light requires a nonzero direction.");
            }
            common.lightDirectionAmbient = {static_cast<float>(x / length), static_cast<float>(y / length),
                static_cast<float>(z / length), frame.light.ambient};
            common.lightColorIntensity = {frame.light.color.x, frame.light.color.y,
                frame.light.color.z, frame.light.intensity};
        }

        const bool forwardClipSafe = frame.frustumCullingEnabled && CullingMatrixSafe(common.viewProjection);
        const bool shadowClipSafe = frame.frustumCullingEnabled && frame.shadow.enabled &&
            CullingMatrixSafe(frame.shadow.viewProjection);
        auto& prepared = _prepared;
        uint32_t scratchGrowths = 0;
        for (const DrawItem& item : frame.draws)
        {
            if (!item.model || !IsFinite(item.world) || !IsFinite(item.tint) || item.model->Meshes().empty() ||
                !std::isfinite(item.fadeCoverage) || item.fadeCoverage < 0 || item.fadeCoverage > 1 ||
                !std::isfinite(item.alphaCutoff) || item.alphaCutoff < 0 || item.alphaCutoff > 1)
            {
                return invalid("Renderer draw requires a model, finite transforms, and coverage/cutoff in [0,1].");
            }
            const auto meshes = item.model->Meshes();
            const auto materials = item.model->Materials();
            if (item.pose)
            {
                if (item.pose->skeletonSignature != item.model->SkeletonSignature() ||
                    item.pose->nodeWorld.size() != item.model->Nodes().size() ||
                    item.pose->meshPalettes.size() != meshes.size())
                {
                    return invalid("Renderer pose signature, node count or mesh palette count disagrees with its Model.");
                }
                for (const Matrix& matrix : item.pose->nodeWorld)
                {
                    Matrix normal;
                    bool mirrored = false;
                    if (!IsFinite(matrix) || !TryNormalWorld(matrix, normal, mirrored))
                    {
                        return invalid("Renderer pose contains an invalid node world.");
                    }
                }
                for (size_t index = 0; index < meshes.size(); ++index)
                {
                    const auto& palette = item.pose->meshPalettes[index];
                    const auto& skin = meshes[index].skin;
                    if ((!skin && !palette.empty()) || (skin && (palette.empty() ||
                        palette.size() != skin->jointNodeIndices.size() || palette.size() > kMaxSkinJoints)))
                    {
                        return invalid("Renderer mesh palette size disagrees with its binding.");
                    }
                    for (const Matrix& matrix : palette)
                    {
                        Matrix normal;
                        bool mirrored = false;
                        if (!IsFinite(matrix) || !TryNormalWorld(matrix, normal, mirrored) || mirrored)
                        {
                            return invalid("Renderer bone palette must be finite, affine, nonsingular and positive determinant.");
                        }
                    }
                }
            }
            else if (std::any_of(meshes.begin(), meshes.end(), [](const MeshGpu& mesh) { return mesh.skin.has_value(); }))
            {
                return invalid("Renderer skinned Model requires an immutable pose snapshot.");
            }
            bool forwardVisible = true, shadowVisible = true;
            if (frame.frustumCullingEnabled && !item.pose &&
                std::all_of(meshes.begin(), meshes.end(), [](const MeshGpu& mesh)
                {
                    return !mesh.skin && IsIdentity(mesh.nodeWorld);
                }))
            {
                // Nonidentity nodes stay visible: float(node * item) can cancel differently from
                // the model-root bound followed by item. No finite epsilon covers every such case.
                // The remaining validation below always runs, even for an outside bound.
                const Vector3 minimum = item.model->BoundsMin();
                const Vector3 maximum = item.model->BoundsMax();
                auto bounds = PrepareClipBounds(minimum, maximum, item.world);
                forwardVisible = BoundsIntersectClip(minimum, maximum, item.world, common.viewProjection,
                    forwardClipSafe, bounds);
                if (frame.shadow.enabled && item.castsShadow)
                {
                    shadowVisible = BoundsIntersectClip(minimum, maximum, item.world, frame.shadow.viewProjection,
                        shadowClipSafe, bounds);
                }
            }
            for (size_t index = 0; index < meshes.size(); ++index)
            {
                const MeshGpu& mesh = meshes[index];
                if (!mesh.vertexBuffer || !mesh.indexBuffer || mesh.indexCount == 0 ||
                    mesh.materialIndex >= materials.size() || mesh.nodeIndex >= item.model->Nodes().size() ||
                    !IsFinite(mesh.nodeWorld))
                {
                    return invalid("Renderer mesh buffer, material index or node transform is invalid.");
                }
                const Material& material = materials[mesh.materialIndex];
                if (!material.diffuseTexture || !IsFinite(material.baseColor) ||
                    (material.blendEnabled && (!material.blendTexture || !IsFinite(material.blendColor) ||
                        !std::isfinite(material.blendHeights.x) || !std::isfinite(material.blendHeights.y) ||
                        material.blendHeights.y <= material.blendHeights.x)))
                {
                    return invalid("Renderer material textures, colors or blend heights are invalid.");
                }
                PreparedMesh draw;
                draw.item = &item;
                draw.mesh = &mesh;
                draw.material = &material;
                draw.meshIndex = index;
                draw.forwardVisible = forwardVisible;
                draw.shadowVisible = shadowVisible;
                if (mesh.skin)
                {
                    // Both passes use the same model-root palette and apply item.world exactly once.
                    draw.world = item.world;
                }
                else
                {
                    const Matrix& node = item.pose ? item.pose->nodeWorld[mesh.nodeIndex] : mesh.nodeWorld;
                    DirectX::XMStoreFloat4x4(&draw.world,
                        DirectX::XMLoadFloat4x4(&node) * DirectX::XMLoadFloat4x4(&item.world));
                }
                if (!IsFinite(draw.world) || !TryNormalWorld(draw.world, draw.normalWorld, draw.mirrored))
                {
                    return invalid("Renderer world is nonfinite, non-affine, singular or has an invalid normal matrix.");
                }
                const Vector4 second = material.blendEnabled ? material.blendColor : material.baseColor;
                draw.color = {material.baseColor.x * item.tint.x, material.baseColor.y * item.tint.y,
                    material.baseColor.z * item.tint.z, material.baseColor.w * item.tint.w};
                draw.blendColor = {second.x * item.tint.x, second.y * item.tint.y,
                    second.z * item.tint.z, second.w * item.tint.w};
                if (!IsFinite(draw.color) || !IsFinite(draw.blendColor) ||
                    prepared.size() == std::numeric_limits<uint32_t>::max())
                {
                    return invalid("Renderer material/tint is nonfinite or mesh count exceeds statistics capacity.");
                }
                const size_t previousCapacity = prepared.capacity();
                prepared.push_back(draw);
                scratchGrowths += prepared.capacity() != previousCapacity ? 1u : 0u;
            }
        }

        ID3D11DeviceContext* context = _graphics.Context();
        RenderingStats nextStats{};
        nextStats.rendererScratchGrowths = scratchGrowths;
        if (frame.frustumCullingEnabled)
        {
            for (const auto& draw : prepared)
            {
                nextStats.forwardCulledMeshes += !draw.forwardVisible ? 1u : 0u;
                nextStats.shadowCulledMeshes += frame.shadow.enabled && draw.item->castsShadow &&
                    !draw.shadowVisible ? 1u : 0u;
            }
        }
        const auto upload = [context](ID3D11Buffer* buffer, const void* data, size_t size, const char* operation)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT result = context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(result))
            {
                Log::Error(operation, result);
                return false;
            }
            std::memcpy(mapped.pData, data, size);
            context->Unmap(buffer, 0);
            return true;
        };
        const auto submit = [&](const PreparedMesh& draw, bool shadow, const InstanceData* instances, UINT count)
        {
            const bool instanced = instances != nullptr;
            const Material& material = *draw.material;
            DrawConstants constants = common;
            constants.viewProjection = shadow ? frame.shadow.viewProjection : common.viewProjection;
            constants.world = draw.world;
            constants.normalWorld = draw.normalWorld;
            constants.materialColor = instanced ? material.baseColor : draw.color;
            constants.blendColor = instanced
                ? (material.blendEnabled ? material.blendColor : material.baseColor) : draw.blendColor;
            constants.blendParams = {material.blendHeights.x, material.blendHeights.y,
                material.blendEnabled ? 1.0f : 0.0f, 0};
            effects.objectParams = {draw.item->fadeCoverage, draw.item->alphaCutoff,
                draw.item->receivesShadow ? 1.0f : 0.0f, static_cast<float>(frame.shadow.pcfRadius)};
            if (!upload(_constantBuffer.Get(), &constants, sizeof(constants), "Map DrawConstants") ||
                !upload(_effectsConstantBuffer.Get(), &effects, sizeof(effects), "Map EffectsConstants"))
            {
                return false;
            }
            if (draw.mesh->skin)
            {
                const auto& palette = draw.item->pose->meshPalettes[draw.meshIndex];
                // All four vertex joint indices are validated against this palette at upload.
                // Preserve the 128-joint buffer ABI; the discarded tail is never read.
                if (!upload(_skinConstantBuffer.Get(), palette.data(), palette.size() * sizeof(Matrix),
                    "Map SkinConstants"))
                {
                    return false;
                }
            }
            if (instanced && !upload(_instanceBuffer.Get(), instances, sizeof(InstanceData) * count, "Map instances"))
            {
                return false;
            }

            // A UI pass and Resize may change all state. Each draw restores its complete pipeline.
            context->VSSetShader(instanced ? _instancedVertexShader.Get() :
                (draw.mesh->skin ? _skinnedVertexShader.Get() : _vertexShader.Get()), nullptr, 0);
            const bool shadowAlphaTest = shadow && (instanced
                ? std::any_of(instances, instances + count,
                    [](const InstanceData& instance) { return instance.effects.y > 0.0f; })
                : draw.item->alphaCutoff > 0.0f);
            context->PSSetShader(shadow ? (shadowAlphaTest ? _shadowPixelShader.Get() : nullptr)
                : _pixelShader.Get(), nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->IASetInputLayout(instanced ? _instancedInputLayout.Get() :
                (draw.mesh->skin ? _skinnedInputLayout.Get() : _inputLayout.Get()));
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* buffers[] = {draw.mesh->vertexBuffer.Get(), instanced ? _instanceBuffer.Get() : nullptr};
            const UINT strides[] = {static_cast<UINT>(draw.mesh->skin ? sizeof(SkinnedVertex) : sizeof(Vertex)),
                instanced ? static_cast<UINT>(sizeof(InstanceData)) : 0};
            const UINT offsets[] = {0, 0};
            context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
            context->IASetIndexBuffer(draw.mesh->indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11Buffer* constantsBuffer = _constantBuffer.Get();
            ID3D11Buffer* skinBuffer = draw.mesh->skin ? _skinConstantBuffer.Get() : nullptr;
            ID3D11Buffer* effectsBuffer = _effectsConstantBuffer.Get();
            context->VSSetConstantBuffers(0, 1, &constantsBuffer);
            context->PSSetConstantBuffers(0, 1, &constantsBuffer);
            context->VSSetConstantBuffers(1, 1, &skinBuffer);
            context->VSSetConstantBuffers(2, 1, &effectsBuffer);
            context->PSSetConstantBuffers(2, 1, &effectsBuffer);
            ID3D11ShaderResourceView* textures[] = {material.diffuseTexture.Get(),
                material.blendEnabled ? material.blendTexture.Get() : material.diffuseTexture.Get(),
                !shadow && frame.shadow.enabled ? _shadowResourceView.Get() : nullptr};
            context->PSSetShaderResources(0, 3, textures);
            ID3D11SamplerState* samplers[] = {_sampler.Get(), !shadow ? _shadowSampler.Get() : nullptr};
            context->PSSetSamplers(0, 2, samplers);
            const bool wireframe = !shadow && draw.item->wireframe;
            context->RSSetState(wireframe
                ? (draw.mirrored ? _mirroredWireframeRasterizer.Get() : _wireframeRasterizer.Get())
                : (draw.mirrored ? _mirroredRasterizer.Get() : _rasterizer.Get()));
            context->OMSetDepthStencilState(_depthState.Get(), 0);
            context->OMSetBlendState(_blendState.Get(), nullptr, 0xffffffff);
            if (instanced)
            {
                context->DrawIndexedInstanced(draw.mesh->indexCount, count, 0, 0, 0);
            }
            else
            {
                context->DrawIndexed(draw.mesh->indexCount, 0, 0);
            }
            auto& calls = shadow ? nextStats.shadowDrawCalls : nextStats.forwardDrawCalls;
            auto& total = shadow ? nextStats.shadowInstances : nextStats.forwardInstances;
            ++calls;
            total += count;
            if (instanced)
            {
                ++(shadow ? nextStats.instancedShadowDrawCalls : nextStats.instancedForwardDrawCalls);
            }
            if (draw.mesh->skin)
            {
                ++(shadow ? nextStats.skinnedShadowDrawCalls : nextStats.skinnedForwardDrawCalls);
            }
            return true;
        };
        const auto renderPass = [&](bool shadow)
        {
            const size_t previousCapacity = _done.capacity();
            _done.assign(prepared.size(), 0);
            nextStats.rendererScratchGrowths += _done.capacity() != previousCapacity ? 1u : 0u;
            auto& done = _done;
            std::array<InstanceData, kMaxInstancesPerBatch> instances{};
            for (size_t first = 0; first < prepared.size(); ++first)
            {
                const PreparedMesh& draw = prepared[first];
                if (done[first] != 0 || (shadow && (!draw.item->castsShadow || !draw.shadowVisible)) ||
                    (!shadow && !draw.forwardVisible))
                {
                    continue;
                }
                const bool instanced = frame.instancingEnabled && !draw.item->pose && !draw.mesh->skin;
                if (!instanced)
                {
                    done[first] = 1;
                    if (!submit(draw, shadow, nullptr, 1))
                    {
                        return false;
                    }
                    continue;
                }
                UINT count = 0;
                for (size_t index = first; index < prepared.size(); ++index)
                {
                    const PreparedMesh& other = prepared[index];
                    if (done[index] != 0 || other.item->pose || other.mesh->skin ||
                        (shadow && (!other.item->castsShadow || !other.shadowVisible)) ||
                        (!shadow && !other.forwardVisible) || other.mesh != draw.mesh ||
                        other.material != draw.material || other.mirrored != draw.mirrored ||
                        other.item->wireframe != draw.item->wireframe)
                    {
                        continue;
                    }
                    done[index] = 1;
                    instances[count++] = {other.world, other.normalWorld, other.item->tint,
                        {other.item->fadeCoverage, other.item->alphaCutoff, other.item->receivesShadow ? 1.0f : 0.0f, 0}};
                    if (count == kMaxInstancesPerBatch)
                    {
                        if (!submit(draw, shadow, instances.data(), count))
                        {
                            return false;
                        }
                        count = 0;
                    }
                }
                if (count != 0 && !submit(draw, shadow, instances.data(), count))
                {
                    return false;
                }
            }
            return true;
        };

        if (frame.shadow.enabled && !prepared.empty())
        {
            ID3D11ShaderResourceView* unbound = nullptr;
            context->PSSetShaderResources(2, 1, &unbound);
            context->OMSetRenderTargets(0, nullptr, _shadowDepthView.Get());
            const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(kShadowMapResolution),
                static_cast<float>(kShadowMapResolution), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->ClearDepthStencilView(_shadowDepthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            if (!renderPass(true))
            {
                return false;
            }
        }
        // Clear restores the window targets and viewport after the shadow pass, including after Resize.
        if (!_graphics.Clear(frame.clearColor) || !renderPass(false) ||
            !RenderParticles(frame, nextStats) || !_uiRenderer->Render(frame.ui))
        {
            return false;
        }
        ID3D11ShaderResourceView* unbound = nullptr;
        context->PSSetShaderResources(2, 1, &unbound);
        const HRESULT removed = _graphics.Device()->GetDeviceRemovedReason();
        if (FAILED(removed))
        {
            Log::Error("Renderer GetDeviceRemovedReason", removed);
            return false;
        }
        _stats = nextStats;
        return true;
    }

    bool Renderer::RenderParticles(const RenderFrame& frame, RenderingStats& stats)
    {
        if (frame.particles.empty())
        {
            return true;
        }
        _particles.assign(frame.particles.begin(), frame.particles.end());
        const auto& view = frame.camera.view;
        const auto depth = [&](const ParticleDraw& value)
        {
            const auto& p = value.positionRotation;
            return static_cast<double>(p.x) * view.m[0][2] +
                static_cast<double>(p.y) * view.m[1][2] + static_cast<double>(p.z) * view.m[2][2];
        };
        // All shapes share one premultiplied blend state. Sort once, without per-frame allocation.
        std::sort(_particles.begin(), _particles.end(), [&](const ParticleDraw& a, const ParticleDraw& b)
            {
                return depth(a) > depth(b);
            });
        auto* context = _graphics.Context();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT result = context->Map(_particleBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
        {
            Log::Error("Map particle instances", result);
            return false;
        }
        const auto bytes = static_cast<UINT>(_particles.size() * sizeof(ParticleDraw));
        std::memcpy(mapped.pData, _particles.data(), bytes);
        context->Unmap(_particleBuffer.Get(), 0);
        ParticleConstants constants{};
        DirectX::XMStoreFloat4x4(&constants.viewProjection,
            DirectX::XMLoadFloat4x4(&view) * DirectX::XMLoadFloat4x4(&frame.camera.projection));
        constants.cameraRight = {view.m[0][0], view.m[1][0], view.m[2][0], 0};
        constants.cameraUp = {view.m[0][1], view.m[1][1], view.m[2][1], 0};
        constants.cameraPosition = {frame.camera.position.x, frame.camera.position.y, frame.camera.position.z, 1};
        result = context->Map(_particleConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
        {
            Log::Error("Map particle constants", result);
            return false;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(_particleConstants.Get(), 0);
        context->VSSetShader(_particleVertexShader.Get(), nullptr, 0);
        context->PSSetShader(_particlePixelShader.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->IASetInputLayout(_particleInputLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* buffer = _particleBuffer.Get();
        const UINT stride = sizeof(ParticleDraw), offset = 0;
        context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        buffer = _particleConstants.Get();
        context->VSSetConstantBuffers(0, 1, &buffer);
        context->RSSetState(_particleRasterizer.Get());
        context->OMSetDepthStencilState(_particleDepth.Get(), 0);
        context->OMSetBlendState(_particleBlend.Get(), nullptr, 0xffffffff);
        context->DrawInstanced(6, static_cast<UINT>(_particles.size()), 0, 0);
        stats.particleDrawCalls = 1;
        stats.particleInstances = static_cast<UINT>(_particles.size());
        stats.particleUploadBytes = bytes;
        return true;
    }

    RenderingStats Renderer::Stats() const noexcept
    {
        return _stats;
    }

    void Renderer::Shutdown()
    {
        std::vector<ParticleDraw>().swap(_particles);
        _particleVertexShader.Reset();
        _particlePixelShader.Reset();
        _particleInputLayout.Reset();
        _particleBuffer.Reset();
        _particleConstants.Reset();
        _particleRasterizer.Reset();
        _particleDepth.Reset();
        _particleBlend.Reset();
        std::vector<PreparedMesh>().swap(_prepared);
        std::vector<uint8_t>().swap(_done);
        if (_uiRenderer)
        {
            _uiRenderer->Shutdown();
            _uiRenderer.reset();
        }
        _stats = {};
        _shadowSampler.Reset();
        _shadowResourceView.Reset();
        _shadowDepthView.Reset();
        _shadowTexture.Reset();
        _effectsConstantBuffer.Reset();
        _instanceBuffer.Reset();
        _instancedInputLayout.Reset();
        _instancedVertexShader.Reset();
        _shadowPixelShader.Reset();
        _blendState.Reset();
        _depthState.Reset();
        _mirroredWireframeRasterizer.Reset();
        _wireframeRasterizer.Reset();
        _mirroredRasterizer.Reset();
        _rasterizer.Reset();
        _sampler.Reset();
        _constantBuffer.Reset();
        _skinConstantBuffer.Reset();
        _skinnedInputLayout.Reset();
        _skinnedVertexShader.Reset();
        _inputLayout.Reset();
        _pixelShader.Reset();
        _vertexShader.Reset();
    }
}
