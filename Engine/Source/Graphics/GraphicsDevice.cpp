#include "Graphics/GraphicsDevice.h"

#include "App/Window.h"
#include "Core/Log.h"

#include <cmath>
#include <dxgi1_2.h>
#include <string>
#include <vector>

namespace Engine
{
    GraphicsDevice::~GraphicsDevice()
    {
        Shutdown();
    }

    bool GraphicsDevice::Initialize(const Window& window)
    {
        if (_device != nullptr || window.Handle() == nullptr || window.Width() == 0 || window.Height() == 0 ||
            window.Width() > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            window.Height() > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            Log::Error("GraphicsDevice::Initialize invalid state or window", E_INVALIDARG);
            return false;
        }

        UINT flags = 0;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        const auto createDevice = [this, &featureLevels, &featureLevel](UINT creationFlags)
        {
            HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
                featureLevels, 2, D3D11_SDK_VERSION, _device.ReleaseAndGetAddressOf(), &featureLevel,
                _context.ReleaseAndGetAddressOf());
            if (result == E_INVALIDARG)
            {
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
                    featureLevels + 1, 1, D3D11_SDK_VERSION, _device.ReleaseAndGetAddressOf(), &featureLevel,
                    _context.ReleaseAndGetAddressOf());
            }
            return result;
        };
        HRESULT result = createDevice(flags);
        if (result == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0)
        {
            Log::Write("D3D11 debug layer unavailable; retrying the hardware device without DEBUG.");
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = createDevice(flags);
        }
        if (FAILED(result))
        {
            LogFailure("D3D11CreateDevice hardware", result);
            Shutdown();
            return false;
        }
        _debugLayerEnabled = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
        Log::Write(_debugLayerEnabled ? "D3D11 hardware device: debug layer enabled." :
            "D3D11 hardware device: debug layer disabled.");
        Log::Write(featureLevel == D3D_FEATURE_LEVEL_11_1 ? "D3D11 feature level 11_1." :
            "D3D11 feature level 11_0.");
        if (_debugLayerEnabled)
        {
            result = _device.As(&_infoQueue);
            if (FAILED(result))
            {
                LogFailure("Query ID3D11InfoQueue", result);
                Shutdown();
                return false;
            }
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        result = _device.As(&dxgiDevice);
        if (SUCCEEDED(result))
        {
            result = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        }
        if (SUCCEEDED(result))
        {
            result = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
        }
        if (FAILED(result))
        {
            LogFailure("Get device DXGI factory", result);
            Shutdown();
            return false;
        }

        DXGI_ADAPTER_DESC adapterDesc = {};
        result = adapter->GetDesc(&adapterDesc);
        if (FAILED(result))
        {
            LogFailure("IDXGIAdapter::GetDesc", result);
            Shutdown();
            return false;
        }
        const int descriptionSize = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            adapterDesc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (descriptionSize == 0)
        {
            Log::Error("Convert adapter description to UTF-8", static_cast<long>(GetLastError()));
            Shutdown();
            return false;
        }
        std::vector<char> description(static_cast<size_t>(descriptionSize));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, adapterDesc.Description, -1,
            description.data(), descriptionSize, nullptr, nullptr) == 0)
        {
            Log::Error("Convert adapter description to UTF-8", static_cast<long>(GetLastError()));
            Shutdown();
            return false;
        }
        Log::Write(std::string("D3D11 adapter: ") + description.data());

        Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
        result = factory.As(&factory2);
        if (SUCCEEDED(result))
        {
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            desc.Width = window.Width();
            desc.Height = window.Height();
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
            result = factory2->CreateSwapChainForHwnd(_device.Get(), window.Handle(), &desc, nullptr,
                nullptr, swapChain1.GetAddressOf());
            if (SUCCEEDED(result))
            {
                result = swapChain1.As(&_swapChain);
                Log::Write("D3D11 swapchain: flip discard, two buffers.");
            }
            else if (result == DXGI_ERROR_UNSUPPORTED || result == DXGI_ERROR_INVALID_CALL || result == E_INVALIDARG)
            {
                Log::Error("CreateSwapChainForHwnd flip discard unsupported", result);
                Log::Write("Flip discard unavailable; trying a windowed legacy discard swapchain.");
            }
            else
            {
                LogFailure("CreateSwapChainForHwnd", result);
                Shutdown();
                return false;
            }
        }
        if (_swapChain == nullptr)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            desc.BufferDesc.Width = window.Width();
            desc.BufferDesc.Height = window.Height();
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BufferDesc.RefreshRate.Denominator = 1;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.OutputWindow = window.Handle();
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            result = factory->CreateSwapChain(_device.Get(), &desc, _swapChain.GetAddressOf());
            if (FAILED(result))
            {
                LogFailure("CreateSwapChain legacy", result);
                Shutdown();
                return false;
            }
            Log::Write("D3D11 swapchain: legacy discard, two buffers.");
        }
        result = factory->MakeWindowAssociation(window.Handle(), DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(result))
        {
            LogFailure("MakeWindowAssociation", result);
            Shutdown();
            return false;
        }
        if (!CreateViews() || !CheckDebugMessages())
        {
            Shutdown();
            return false;
        }
        return true;
    }

    bool GraphicsDevice::CreateViews()
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
        HRESULT result = _swapChain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
        if (FAILED(result))
        {
            LogFailure("GetBuffer", result);
            return false;
        }
        D3D11_TEXTURE2D_DESC backbufferDesc = {};
        backbuffer->GetDesc(&backbufferDesc);
        result = _device->CreateRenderTargetView(backbuffer.Get(), nullptr, renderTarget.GetAddressOf());
        if (FAILED(result))
        {
            LogFailure("CreateRenderTargetView", result);
            return false;
        }
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = backbufferDesc.Width;
        depthDesc.Height = backbufferDesc.Height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc = backbufferDesc.SampleDesc;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        result = _device->CreateTexture2D(&depthDesc, nullptr, depthTexture.GetAddressOf());
        if (SUCCEEDED(result))
        {
            result = _device->CreateDepthStencilView(depthTexture.Get(), nullptr, depthView.GetAddressOf());
        }
        if (FAILED(result))
        {
            LogFailure("Create depth texture/view", result);
            return false;
        }

        _renderTargetView = renderTarget;
        _depthTexture = depthTexture;
        _depthStencilView = depthView;
        _width = backbufferDesc.Width;
        _height = backbufferDesc.Height;
        _viewport = {0.0f, 0.0f, static_cast<float>(_width), static_cast<float>(_height), 0.0f, 1.0f};
        _context->RSSetViewports(1, &_viewport);
        return true;
    }

    bool GraphicsDevice::Resize(uint32_t width, uint32_t height)
    {
        if (_device == nullptr || _swapChain == nullptr || _context == nullptr)
        {
            Log::Error("GraphicsDevice::Resize before Initialize", E_UNEXPECTED);
            return false;
        }
        if (width == 0 || height == 0)
        {
            return true;
        }
        if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            Log::Error("GraphicsDevice::Resize exceeds feature level 11 texture dimensions", E_INVALIDARG);
            return false;
        }
        if (width == _width && height == _height && _renderTargetView != nullptr && _depthStencilView != nullptr)
        {
            return true;
        }

        _context->OMSetRenderTargets(0, nullptr, nullptr);
        _context->ClearState();
        _renderTargetView.Reset();
        _depthStencilView.Reset();
        _depthTexture.Reset();
        _context->Flush();
        _width = 0;
        _height = 0;
        _viewport = {};
        const HRESULT result = _swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(result))
        {
            LogFailure("ResizeBuffers", result);
            return false;
        }
        return CreateViews() && CheckDebugMessages();
    }

    bool GraphicsDevice::Clear(const std::array<float, 4>& color)
    {
        for (float component : color)
        {
            if (!std::isfinite(component))
            {
                Log::Error("GraphicsDevice::Clear requires finite color components", E_INVALIDARG);
                return false;
            }
        }
        if (_context == nullptr || _renderTargetView == nullptr || _depthStencilView == nullptr || _width == 0 || _height == 0)
        {
            Log::Error("GraphicsDevice::Clear has no complete viewport", E_UNEXPECTED);
            return false;
        }
        ID3D11RenderTargetView* renderTarget = _renderTargetView.Get();
        _context->OMSetRenderTargets(1, &renderTarget, _depthStencilView.Get());
        _context->RSSetViewports(1, &_viewport);
        _context->ClearRenderTargetView(renderTarget, color.data());
        _context->ClearDepthStencilView(_depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        const HRESULT result = _device->GetDeviceRemovedReason();
        if (FAILED(result))
        {
            LogFailure("Clear device state", result);
            return false;
        }
        return CheckDebugMessages();
    }

    bool GraphicsDevice::Present()
    {
        if (_swapChain == nullptr || _renderTargetView == nullptr || _depthStencilView == nullptr)
        {
            Log::Error("GraphicsDevice::Present before complete initialization", E_UNEXPECTED);
            return false;
        }
        const HRESULT result = _swapChain->Present(0, 0);
        if (FAILED(result))
        {
            LogFailure("Present", result);
            return false;
        }
        if (result == DXGI_STATUS_OCCLUDED && !_occludedLogged)
        {
            Log::Write("D3D11 Present: output occluded (hidden or minimized window).");
            _occludedLogged = true;
        }
        else if (result == S_OK)
        {
            _occludedLogged = false;
        }
        return CheckDebugMessages();
    }

    bool GraphicsDevice::CheckDebugMessages(bool liveReport)
    {
        if (_infoQueue == nullptr)
        {
            return true;
        }
        bool clean = true;
        const UINT64 count = _infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T size = 0;
            HRESULT result = _infoQueue->GetMessage(index, nullptr, &size);
            if (FAILED(result))
            {
                Log::Error("ID3D11InfoQueue::GetMessage size", result);
                clean = false;
                continue;
            }
            std::vector<char> bytes(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            result = _infoQueue->GetMessage(index, message, &size);
            if (FAILED(result))
            {
                Log::Error("ID3D11InfoQueue::GetMessage", result);
                clean = false;
                continue;
            }
            if (liveReport)
            {
                if (message->ID == D3D11_MESSAGE_ID_LIVE_DEVICE ||
                    message->ID == D3D11_MESSAGE_ID_LIVE_DEVICE_WIN7 ||
                    message->ID == D3D11_MESSAGE_ID_LIVE_OBJECT_SUMMARY ||
                    message->ID == D3D11_MESSAGE_ID_LIVE_OBJECT_SUMMARY_WIN7)
                {
                    continue;
                }
                Log::Write("D3D11 LIVE CHILD:");
                Log::Write(message->pDescription);
                clean = false;
            }
            else if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING ||
                message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)
            {
                Log::Write(message->Severity == D3D11_MESSAGE_SEVERITY_WARNING ? "D3D11 WARNING:" : "D3D11 ERROR:");
                Log::Write(message->pDescription);
                clean = false;
            }
        }
        _infoQueue->ClearStoredMessages();
        return clean;
    }

    void GraphicsDevice::LogFailure(const char* operation, HRESULT result) const
    {
        Log::Error(operation, result);
        if (_device != nullptr && (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
            result == DXGI_ERROR_DEVICE_HUNG))
        {
            Log::Error("GetDeviceRemovedReason", _device->GetDeviceRemovedReason());
        }
    }

    void GraphicsDevice::Shutdown()
    {
        if (_context != nullptr)
        {
            _context->ClearState();
        }
        _renderTargetView.Reset();
        _depthStencilView.Reset();
        _depthTexture.Reset();
        _swapChain.Reset();
        if (_context != nullptr)
        {
            _context->Flush();
        }
        _context.Reset();
        CheckDebugMessages();
        if (_debugLayerEnabled && _device != nullptr)
        {
            Microsoft::WRL::ComPtr<ID3D11Debug> debug;
            const HRESULT result = _device.As(&debug);
            if (SUCCEEDED(result))
            {
                _device.Reset();
                Log::Write("D3D11 live-object report: the reporting interfaces retain the device.");
                const HRESULT report = debug->ReportLiveDeviceObjects(
                    static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL));
                if (FAILED(report))
                {
                    Log::Error("ReportLiveDeviceObjects", report);
                }
                else if (_infoQueue != nullptr)
                {
                    const bool clean = CheckDebugMessages(true);
                    Log::Write(clean ? "D3D11 live-object check: no external child objects." :
                        "D3D11 live-object check: external child objects remain.");
                }
            }
            else
            {
                Log::Error("Query ID3D11Debug for live-object report", result);
            }
        }
        _infoQueue.Reset();
        _device.Reset();
        _viewport = {};
        _width = 0;
        _height = 0;
        _debugLayerEnabled = false;
        _occludedLogged = false;
    }

    ID3D11Device* GraphicsDevice::Device() const
    {
        return _device.Get();
    }

    ID3D11DeviceContext* GraphicsDevice::Context() const
    {
        return _context.Get();
    }

    bool GraphicsDevice::DebugLayerEnabled() const
    {
        return _debugLayerEnabled;
    }

    uint32_t GraphicsDevice::Width() const
    {
        return _width;
    }

    uint32_t GraphicsDevice::Height() const
    {
        return _height;
    }

    float GraphicsDevice::ViewportAspect() const
    {
        return _height != 0 ? static_cast<float>(_width) / static_cast<float>(_height) : 0.0f;
    }
}
