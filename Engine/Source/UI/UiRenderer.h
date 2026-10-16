#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Engine
{
    class GraphicsDevice;
    struct UiFrame;

    class UiRenderer
    {
    public:
        static constexpr size_t kMaximumVertices = 131072;
        // Unicode BMP codepoints, including spaces and LF, rather than UTF-8 bytes.
        static constexpr size_t kMaximumCharacters = 4096;

        explicit UiRenderer(GraphicsDevice& graphics);
        ~UiRenderer();
        UiRenderer(const UiRenderer&) = delete;
        UiRenderer& operator=(const UiRenderer&) = delete;

        bool Initialize(const std::filesystem::path& shaderRoot);
        static bool Validate(const UiFrame& frame) noexcept;
        bool Render(const UiFrame& frame);
        void Shutdown();

    private:
        struct Vertex
        {
            float x;
            float y;
            float u;
            float v;
            float r;
            float g;
            float b;
            float a;
        };

        struct Glyph
        {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            int offsetX = 0, offsetY = 0, width = 0, height = 0, advance = 0;
            bool cached = false;
        };
        struct FontState;

        static constexpr uint32_t kGlyphCell = 48;
        static constexpr uint32_t kAtlasColumns = 64;
        static constexpr uint32_t kAtlasWidth = kGlyphCell * kAtlasColumns;
        static constexpr uint32_t kAtlasHeight = kGlyphCell * 65;

        void AddQuad(float left, float top, float right, float bottom,
            float r, float g, float b, float a, float u0, float v0, float u1, float v1);
        bool PrepareGlyphs(const UiFrame& frame);
        bool CacheGlyph(uint16_t character);
        void ResetGlyphs();

        GraphicsDevice& _graphics;
        std::vector<Vertex> _vertices;
        std::unique_ptr<FontState> _font;
        std::vector<Glyph> _glyphs;
        std::vector<uint16_t> _frameGlyphs;
        std::vector<uint8_t> _atlasPixels;
        std::vector<uint8_t> _glyphBitmap;
        size_t _cachedGlyphCount = 0;
        bool _atlasDirty = false;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixelShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> _inputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depthState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blendState;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _atlasTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _atlasView;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
    };
}
