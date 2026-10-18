#pragma once

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace Engine
{
    class Window;

    class GraphicsDevice
    {
    public:
        GraphicsDevice() = default;
        ~GraphicsDevice();
        GraphicsDevice(const GraphicsDevice&) = delete;
        GraphicsDevice& operator=(const GraphicsDevice&) = delete;
        GraphicsDevice(GraphicsDevice&&) = delete;
        GraphicsDevice& operator=(GraphicsDevice&&) = delete;

        bool Initialize(const Window& window, uint32_t maximumFrameLatency = 0);
        bool Resize(uint32_t width, uint32_t height);
        bool Clear(const std::array<float, 4>& color);
        bool Present(uint32_t syncInterval = 0);
        float ViewportAspect() const;
        void Shutdown();

        ID3D11Device* Device() const;
        ID3D11DeviceContext* Context() const;
        bool DebugLayerEnabled() const;
        uint32_t Width() const;
        uint32_t Height() const;

    private:
        bool CreateViews();
        bool CheckDebugMessages(bool liveReport = false);
        void LogFailure(const char* operation, HRESULT result) const;

        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;
        Microsoft::WRL::ComPtr<IDXGISwapChain> _swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> _depthStencilView;
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> _infoQueue;
        D3D11_VIEWPORT _viewport = {};
        uint32_t _width = 0;
        uint32_t _height = 0;
        bool _debugLayerEnabled = false;
        bool _occludedLogged = false;
    };
}
