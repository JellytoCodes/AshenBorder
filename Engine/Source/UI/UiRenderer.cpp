#include "UI/UiRenderer.h"

#include "Core/Log.h"
#include "Graphics/GraphicsDevice.h"
#include "UI/UiData.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>

#pragma comment(lib, "gdi32.lib")

namespace
{
    bool NextCharacter(std::string_view text, size_t& offset, uint32_t& value) noexcept
    {
        if (offset >= text.size())
        {
            return false;
        }
        const auto first = static_cast<unsigned char>(text[offset++]);
        size_t continuation = 0;
        uint32_t minimum = 0;
        if (first <= 0x7f)
        {
            value = first;
        }
        else if (first >= 0xc2 && first <= 0xdf)
        {
            value = first & 0x1f;
            continuation = 1;
            minimum = 0x80;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            value = first & 0x0f;
            continuation = 2;
            minimum = 0x800;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            value = first & 0x07;
            continuation = 3;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }
        if (continuation > text.size() - offset)
        {
            return false;
        }
        for (size_t index = 0; index < continuation; ++index)
        {
            const auto byte = static_cast<unsigned char>(text[offset++]);
            if ((byte & 0xc0) != 0x80)
            {
                return false;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        // This renderer addresses one UTF-16 BMP glyph; supplementary scalars
        // are explicitly unsupported, not replaced with a question mark.
        return value >= minimum && value <= 0xffff && !(value >= 0xd800 && value <= 0xdfff) &&
            (value == '\n' || (value >= 0x20 && !(value >= 0x7f && value <= 0x9f)));
    }

    bool Unit(float value) noexcept
    {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    }

    bool Color(Engine::Vector4 color) noexcept
    {
        return Unit(color.x) && Unit(color.y) && Unit(color.z) && Unit(color.w);
    }

    bool Compile(const std::filesystem::path& file, const char* entry, const char* target,
        Microsoft::WRL::ComPtr<ID3DBlob>& bytecode)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
        const HRESULT result = D3DCompileFromFile(file.c_str(), nullptr, nullptr,
            entry, target, flags, 0, bytecode.GetAddressOf(), diagnostics.GetAddressOf());
        if (diagnostics && diagnostics->GetBufferSize() != 0)
        {
            const auto* message = static_cast<const char*>(diagnostics->GetBufferPointer());
            size_t count = diagnostics->GetBufferSize();
            if (message[count - 1] == '\0')
            {
                --count;
            }
            Engine::Log::Write(std::string_view(message, count));
        }
        if (FAILED(result))
        {
            Engine::Log::Error("Compile UI shader", result);
            return false;
        }
        return true;
    }
}

namespace Engine
{
    struct UiRenderer::FontState
    {
        HDC dc = nullptr;
        HFONT font = nullptr;
        HGDIOBJ previousFont = nullptr;
        int ascent = 0;
        int lineHeight = 0;

        ~FontState()
        {
            if (dc && previousFont && previousFont != HGDI_ERROR)
            {
                SelectObject(dc, previousFont);
            }
            if (font)
            {
                DeleteObject(font);
            }
            if (dc)
            {
                DeleteDC(dc);
            }
        }
    };

    UiRenderer::UiRenderer(GraphicsDevice& graphics)
        : _graphics(graphics)
    {
    }

    UiRenderer::~UiRenderer()
    {
        Shutdown();
    }

    bool UiRenderer::Initialize(const std::filesystem::path& shaderRoot)
    {
        if (_vertexShader || _font || !_graphics.Device() || !_graphics.Context() || shaderRoot.empty())
        {
            Log::Write("UI initialization requires an unused renderer, device, and shader root.");
            return false;
        }
        try
        {
            _vertices.reserve(kMaximumVertices);
            _frameGlyphs.reserve(kMaximumCharacters);
            _glyphs.resize(65536);
            _atlasPixels.resize(static_cast<size_t>(kAtlasWidth) * kAtlasHeight);
            _glyphBitmap.resize(kGlyphCell * kGlyphCell);
            _font = std::make_unique<FontState>();
            _font->dc = CreateCompatibleDC(nullptr);
            _font->font = CreateFontW(-32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
            if (!_font->dc || !_font->font)
            {
                Log::Write("UI could not create the required Malgun Gothic font/DC.");
                Shutdown();
                return false;
            }
            _font->previousFont = SelectObject(_font->dc, _font->font);
            wchar_t selectedFace[LF_FACESIZE] = {};
            TEXTMETRICW metrics = {};
            if (!_font->previousFont || _font->previousFont == HGDI_ERROR ||
                GetTextFaceW(_font->dc, LF_FACESIZE, selectedFace) == 0 ||
                (CompareStringOrdinal(selectedFace, -1, L"Malgun Gothic", -1, TRUE) != CSTR_EQUAL &&
                    CompareStringOrdinal(selectedFace, -1, L"맑은 고딕", -1, TRUE) != CSTR_EQUAL) ||
                !GetTextMetricsW(_font->dc, &metrics) || metrics.tmHeight <= 0 || metrics.tmAscent <= 0)
            {
                Log::Write("UI requires installed Malgun Gothic; font substitution is not accepted.");
                Shutdown();
                return false;
            }
            _font->ascent = metrics.tmAscent;
            _font->lineHeight = metrics.tmHeight;
            ResetGlyphs();
        }
        catch (const std::exception&)
        {
            Log::Write("UI bounded font/vertex storage allocation failed.");
            Shutdown();
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vertexCode;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelCode;
        if (!Compile(shaderRoot / L"UI.hlsl", "VSMain", "vs_5_0", vertexCode) ||
            !Compile(shaderRoot / L"UI.hlsl", "PSMain", "ps_5_0", pixelCode))
        {
            Shutdown();
            return false;
        }
        const auto check = [this](HRESULT result, const char* operation)
        {
            if (FAILED(result))
            {
                Log::Error(operation, result);
                Shutdown();
                return false;
            }
            return true;
        };
        ID3D11Device* device = _graphics.Device();
        if (!check(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
            nullptr, _vertexShader.GetAddressOf()), "Create UI vertex shader") ||
            !check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                nullptr, _pixelShader.GetAddressOf()), "Create UI pixel shader"))
        {
            return false;
        }
        const D3D11_INPUT_ELEMENT_DESC elements[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        static_assert(sizeof(Vertex) == 32);
        if (!check(device->CreateInputLayout(elements, 3, vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), _inputLayout.GetAddressOf()), "Create UI layout"))
        {
            return false;
        }
        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = static_cast<UINT>(sizeof(Vertex) * kMaximumVertices);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_RASTERIZER_DESC rasterizer = {};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_BLEND_DESC blend = {};
        auto& target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_TEXTURE2D_DESC texture = {};
        texture.Width = kAtlasWidth;
        texture.Height = kAtlasHeight;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA pixels = {_atlasPixels.data(), kAtlasWidth, 0};
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        const bool ready = check(device->CreateBuffer(&buffer, nullptr, _vertexBuffer.GetAddressOf()), "Create UI vertices") &&
            check(device->CreateRasterizerState(&rasterizer, _rasterizer.GetAddressOf()), "Create UI rasterizer") &&
            check(device->CreateDepthStencilState(&depth, _depthState.GetAddressOf()), "Create UI depth state") &&
            check(device->CreateBlendState(&blend, _blendState.GetAddressOf()), "Create UI blend state") &&
            check(device->CreateTexture2D(&texture, &pixels, _atlasTexture.GetAddressOf()), "Create UI glyph atlas") &&
            check(device->CreateShaderResourceView(_atlasTexture.Get(), nullptr, _atlasView.GetAddressOf()), "Create UI atlas view") &&
            check(device->CreateSamplerState(&sampler, _sampler.GetAddressOf()), "Create UI atlas sampler");
        if (ready)
        {
            Log::Write("UI: strict UTF-8 BMP / Malgun Gothic GDI R8 atlas initialized");
        }
        return ready;
    }

    bool UiRenderer::Validate(const UiFrame& frame) noexcept
    {
        if (frame.rects.size() > 4096 || frame.texts.size() > 128)
        {
            return false;
        }
        size_t vertices = frame.rects.size() * 6;
        size_t characters = 0;
        for (const UiRect& rect : frame.rects)
        {
            if (!Unit(rect.position.x) || !Unit(rect.position.y) || !Unit(rect.size.x) ||
                !Unit(rect.size.y) || rect.position.x + rect.size.x > 1.0f ||
                rect.position.y + rect.size.y > 1.0f || !Color(rect.color))
            {
                return false;
            }
        }
        for (const UiText& text : frame.texts)
        {
            if (!Unit(text.position.x) || !Unit(text.position.y) || !Unit(text.height) ||
                text.height == 0.0f || !Color(text.color) || text.text.size() > kMaximumCharacters * 3 ||
                !Unit(text.maxWidth) ||
                (text.alignment != UiTextAlignment::Left && text.alignment != UiTextAlignment::Center &&
                    text.alignment != UiTextAlignment::Right))
            {
                return false;
            }
            const float left = text.alignment == UiTextAlignment::Center ?
                text.position.x - text.maxWidth * 0.5f : text.alignment == UiTextAlignment::Right ?
                text.position.x - text.maxWidth : text.position.x;
            if (text.maxWidth > 0 && (left < 0 || left + text.maxWidth > 1.0f))
            {
                return false;
            }
            size_t offset = 0;
            while (offset < text.text.size())
            {
                uint32_t character = 0;
                if (!NextCharacter(text.text, offset, character) || ++characters > kMaximumCharacters)
                {
                    return false;
                }
                vertices += character == '\n' ? 0 : 6;
            }
        }
        return vertices <= kMaximumVertices;
    }

    void UiRenderer::ResetGlyphs()
    {
        std::fill(_glyphs.begin(), _glyphs.end(), Glyph{});
        std::fill(_atlasPixels.begin(), _atlasPixels.end(), uint8_t{0});
        _atlasPixels[kAtlasWidth + 1] = 255;
        _cachedGlyphCount = 0;
        _atlasDirty = true;
    }

    bool UiRenderer::CacheGlyph(uint16_t character)
    {
        Glyph& glyph = _glyphs[character];
        if (glyph.cached)
        {
            return true;
        }
        const wchar_t text = static_cast<wchar_t>(character);
        WORD index = 0xffff;
        if (GetGlyphIndicesW(_font->dc, &text, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || index == 0xffff)
        {
            Log::Write("UI text contains a glyph absent from Malgun Gothic.");
            return false;
        }
        MAT2 transform = {};
        transform.eM11.value = 1;
        transform.eM22.value = 1;
        GLYPHMETRICS metrics = {};
        const DWORD format = GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX;
        const DWORD bytes = GetGlyphOutlineW(_font->dc, index, format, &metrics, 0, nullptr, &transform);
        if (bytes == GDI_ERROR || bytes > _glyphBitmap.size() || metrics.gmBlackBoxX > kGlyphCell - 4 ||
            metrics.gmBlackBoxY > kGlyphCell - 4 || metrics.gmCellIncX < 0 || _cachedGlyphCount >= kMaximumCharacters)
        {
            Log::Write("UI glyph exceeds its fixed atlas cell or cannot be rasterized.");
            return false;
        }
        const size_t slot = _cachedGlyphCount + 1;
        const uint32_t left = static_cast<uint32_t>(slot % kAtlasColumns) * kGlyphCell + 2;
        const uint32_t top = static_cast<uint32_t>(slot / kAtlasColumns) * kGlyphCell + 2;
        if (bytes != 0)
        {
            if (GetGlyphOutlineW(_font->dc, index, format, &metrics, bytes, _glyphBitmap.data(), &transform) == GDI_ERROR)
            {
                Log::Write("UI glyph bitmap read failed.");
                return false;
            }
            const uint32_t stride = (metrics.gmBlackBoxX + 3u) & ~3u;
            if (static_cast<size_t>(stride) * metrics.gmBlackBoxY > bytes)
            {
                return false;
            }
            for (uint32_t y = 0; y < metrics.gmBlackBoxY; ++y)
            {
                for (uint32_t x = 0; x < metrics.gmBlackBoxX; ++x)
                {
                    const uint32_t alpha = std::min<uint32_t>(_glyphBitmap[static_cast<size_t>(y) * stride + x], 64);
                    _atlasPixels[static_cast<size_t>(top + y) * kAtlasWidth + left + x] =
                        static_cast<uint8_t>((alpha * 255u + 32u) / 64u);
                }
            }
        }
        glyph.u0 = static_cast<float>(left) / kAtlasWidth;
        glyph.v0 = static_cast<float>(top) / kAtlasHeight;
        glyph.u1 = static_cast<float>(left + metrics.gmBlackBoxX) / kAtlasWidth;
        glyph.v1 = static_cast<float>(top + metrics.gmBlackBoxY) / kAtlasHeight;
        glyph.offsetX = metrics.gmptGlyphOrigin.x;
        glyph.offsetY = _font->ascent - metrics.gmptGlyphOrigin.y;
        glyph.width = static_cast<int>(metrics.gmBlackBoxX);
        glyph.height = static_cast<int>(metrics.gmBlackBoxY);
        glyph.advance = metrics.gmCellIncX;
        glyph.cached = true;
        ++_cachedGlyphCount;
        _atlasDirty = true;
        return true;
    }

    bool UiRenderer::PrepareGlyphs(const UiFrame& frame)
    {
        _frameGlyphs.clear();
        for (const UiText& text : frame.texts)
        {
            size_t offset = 0;
            while (offset < text.text.size())
            {
                uint32_t character = 0;
                if (!NextCharacter(text.text, offset, character))
                {
                    return false;
                }
                if (character != '\n')
                {
                    _frameGlyphs.push_back(static_cast<uint16_t>(character));
                }
            }
        }
        std::sort(_frameGlyphs.begin(), _frameGlyphs.end());
        _frameGlyphs.erase(std::unique(_frameGlyphs.begin(), _frameGlyphs.end()), _frameGlyphs.end());
        const auto missing = static_cast<size_t>(std::count_if(_frameGlyphs.begin(), _frameGlyphs.end(),
            [this](uint16_t character)
            {
                return !_glyphs[character].cached;
            }));
        if (_cachedGlyphCount + missing > kMaximumCharacters)
        {
            ResetGlyphs();
        }
        for (uint16_t character : _frameGlyphs)
        {
            if (!CacheGlyph(character))
            {
                return false;
            }
        }
        return true;
    }

    void UiRenderer::AddQuad(float left, float top, float right, float bottom,
        float r, float g, float b, float a, float u0, float v0, float u1, float v1)
    {
        left = left * 2.0f - 1.0f;
        right = right * 2.0f - 1.0f;
        top = 1.0f - top * 2.0f;
        bottom = 1.0f - bottom * 2.0f;
        _vertices.push_back({left, top, u0, v0, r, g, b, a});
        _vertices.push_back({right, top, u1, v0, r, g, b, a});
        _vertices.push_back({left, bottom, u0, v1, r, g, b, a});
        _vertices.push_back({left, bottom, u0, v1, r, g, b, a});
        _vertices.push_back({right, top, u1, v0, r, g, b, a});
        _vertices.push_back({right, bottom, u1, v1, r, g, b, a});
    }

    bool UiRenderer::Render(const UiFrame& frame)
    {
        if (!Validate(frame) || !_vertexBuffer || !_atlasView || !_sampler || !_font || !_graphics.Device() || !_graphics.Context() ||
            _graphics.Width() == 0 || _graphics.Height() == 0)
        {
            Log::Write("UI rendering rejected invalid frame or incomplete renderer.");
            return false;
        }
        ID3D11DeviceContext* context = _graphics.Context();
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        if (!target || !PrepareGlyphs(frame))
        {
            Log::Write("UI rendering requires a color target and available BMP font glyphs.");
            return false;
        }
        _vertices.clear();
        const float whiteU = 1.5f / kAtlasWidth;
        const float whiteV = 1.5f / kAtlasHeight;
        for (const UiRect& rect : frame.rects)
        {
            AddQuad(rect.position.x, rect.position.y, rect.position.x + rect.size.x,
                rect.position.y + rect.size.y, rect.color.x, rect.color.y, rect.color.z, rect.color.w,
                whiteU, whiteV, whiteU, whiteV);
        }
        const float width = static_cast<float>(_graphics.Width());
        const float height = static_cast<float>(_graphics.Height());
        for (const UiText& text : frame.texts)
        {
            // Include ink bearings as well as advances, so italic/overhanging
            // glyphs cannot escape a requested fit width. No per-frame font or cache.
            const auto measureLine = [this, &text](size_t start, size_t& end)
            {
                float advance = 0;
                float minimum = 0;
                float maximum = 0;
                end = start;
                while (end < text.text.size())
                {
                    uint32_t character = 0;
                    if (!NextCharacter(text.text, end, character) || character == '\n')
                    {
                        break;
                    }
                    const Glyph& glyph = _glyphs[character];
                    minimum = std::min(minimum, advance + static_cast<float>(glyph.offsetX));
                    maximum = std::max(maximum, advance + static_cast<float>(glyph.offsetX + glyph.width));
                    advance += static_cast<float>(glyph.advance);
                }
                return Vector2{minimum, std::max(maximum, advance)};
            };
            const float originalScaleY = text.height / static_cast<float>(_font->lineHeight);
            const float originalScaleX = originalScaleY * height / width;
            float widest = 0;
            size_t offset = 0;
            while (offset < text.text.size())
            {
                const Vector2 bounds = measureLine(offset, offset);
                widest = std::max(widest, bounds.y - bounds.x);
            }
            const float fit = text.maxWidth > 0 && widest * originalScaleX > text.maxWidth ?
                text.maxWidth / (widest * originalScaleX) : 1.0f;
            const float scaleY = originalScaleY * fit;
            const float scaleX = originalScaleX * fit;
            float y = text.position.y + (text.height - text.height * fit) * 0.5f;
            offset = 0;
            while (offset < text.text.size())
            {
                size_t end = offset;
                const Vector2 bounds = measureLine(offset, end);
                float x = text.position.x;
                if (text.alignment == UiTextAlignment::Center)
                {
                    x -= (bounds.x + bounds.y) * scaleX * 0.5f;
                }
                else if (text.alignment == UiTextAlignment::Right)
                {
                    x -= bounds.y * scaleX;
                }
                else if (text.maxWidth > 0)
                {
                    x -= bounds.x * scaleX;
                }
                while (offset < end)
                {
                    uint32_t character = 0;
                    if (!NextCharacter(text.text, offset, character))
                    {
                        return false;
                    }
                    if (character == '\n')
                    {
                        break;
                    }
                    const Glyph& glyph = _glyphs[character];
                    if (glyph.width != 0 && glyph.height != 0)
                    {
                        const float left = x + static_cast<float>(glyph.offsetX) * scaleX;
                        const float top = y + static_cast<float>(glyph.offsetY) * scaleY;
                        AddQuad(left, top, left + static_cast<float>(glyph.width) * scaleX,
                            top + static_cast<float>(glyph.height) * scaleY, text.color.x, text.color.y,
                            text.color.z, text.color.w, glyph.u0, glyph.v0, glyph.u1, glyph.v1);
                    }
                    x += static_cast<float>(glyph.advance) * scaleX;
                }
                y += text.height * fit * 1.25f;
            }
        }
        if (_vertices.empty())
        {
            return true;
        }
        if (_atlasDirty)
        {
            ID3D11ShaderResourceView* empty = nullptr;
            context->PSSetShaderResources(0, 1, &empty);
            context->UpdateSubresource(_atlasTexture.Get(), 0, nullptr, _atlasPixels.data(), kAtlasWidth, 0);
            _atlasDirty = false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT result = context->Map(_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
        {
            Log::Error("Map UI vertices", result);
            return false;
        }
        std::memcpy(mapped.pData, _vertices.data(), _vertices.size() * sizeof(Vertex));
        context->Unmap(_vertexBuffer.Get(), 0);

        const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(_graphics.Width()),
            static_cast<float>(_graphics.Height()), 0.0f, 1.0f};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(_rasterizer.Get());
        context->OMSetDepthStencilState(_depthState.Get(), 0);
        context->OMSetBlendState(_blendState.Get(), nullptr, 0xffffffff);
        context->IASetInputLayout(_inputLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* buffer = _vertexBuffer.Get();
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context->VSSetShader(_vertexShader.Get(), nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(_pixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* atlas = _atlasView.Get();
        ID3D11SamplerState* sampler = _sampler.Get();
        context->PSSetShaderResources(0, 1, &atlas);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(static_cast<UINT>(_vertices.size()), 0);
        const HRESULT deviceResult = _graphics.Device()->GetDeviceRemovedReason();
        if (FAILED(deviceResult))
        {
            Log::Error("UI render device", deviceResult);
            return false;
        }
        return true;
    }

    void UiRenderer::Shutdown()
    {
        if (_graphics.Context() && _atlasView)
        {
            ID3D11ShaderResourceView* empty = nullptr;
            _graphics.Context()->PSSetShaderResources(0, 1, &empty);
        }
        _sampler.Reset();
        _atlasView.Reset();
        _atlasTexture.Reset();
        _blendState.Reset();
        _depthState.Reset();
        _rasterizer.Reset();
        _vertexBuffer.Reset();
        _inputLayout.Reset();
        _pixelShader.Reset();
        _vertexShader.Reset();
        _vertices.clear();
        _frameGlyphs.clear();
        _glyphs.clear();
        _glyphBitmap.clear();
        _atlasPixels.clear();
        _font.reset();
        _cachedGlyphCount = 0;
        _atlasDirty = false;
    }
}
