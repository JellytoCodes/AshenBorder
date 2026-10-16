#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <vector>

namespace Engine
{
    class GraphicsDevice;
    class UiRenderer;
    struct RenderFrame;

    inline constexpr uint32_t kMaxInstancesPerBatch = 128;
    inline constexpr uint32_t kShadowMapResolution = 1024;

    struct RenderingStats
    {
        uint32_t forwardDrawCalls = 0;
        uint32_t shadowDrawCalls = 0;
        uint32_t forwardInstances = 0;
        uint32_t shadowInstances = 0;
        uint32_t instancedForwardDrawCalls = 0;
        uint32_t instancedShadowDrawCalls = 0;
        uint32_t skinnedForwardDrawCalls = 0;
        uint32_t skinnedShadowDrawCalls = 0;
        uint32_t rendererScratchGrowths = 0;
        uint32_t forwardCulledMeshes = 0;
        uint32_t shadowCulledMeshes = 0;
    };

    class Renderer
    {
    public:
        explicit Renderer(GraphicsDevice& graphics);
        ~Renderer();
        bool Initialize(const std::filesystem::path& shaderRoot);
        bool Render(const RenderFrame& frame);
        void Shutdown();
        RenderingStats Stats() const noexcept;

    private:
        struct PreparedMesh;

        GraphicsDevice& _graphics;
        std::vector<PreparedMesh> _prepared;
        std::vector<uint8_t> _done;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _shadowPixelShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> _inputLayout;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _skinnedVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> _skinnedInputLayout;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _instancedVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> _instancedInputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _instanceBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _effectsConstantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _skinConstantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _mirroredRasterizer;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _wireframeRasterizer;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _mirroredWireframeRasterizer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depthState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blendState;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _shadowTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> _shadowDepthView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _shadowResourceView;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _shadowSampler;
        std::unique_ptr<UiRenderer> _uiRenderer;
        RenderingStats _stats;
    };
}
