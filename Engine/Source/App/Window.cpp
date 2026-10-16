#include "App/Window.h"

#include "App/ApplicationDesc.h"
#include "Core/Input.h"
#include "Core/Log.h"

#include <limits>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"Engine.AppWindow";
    constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
}

namespace Engine
{
    Window::~Window()
    {
        Destroy();
    }

    bool Window::Create(const ApplicationDesc& desc)
    {
        if (_handle != nullptr || desc.width == 0 || desc.height == 0 ||
            desc.width > static_cast<uint32_t>((std::numeric_limits<int>::max)()) / 2 ||
            desc.height > static_cast<uint32_t>((std::numeric_limits<int>::max)()) / 2)
        {
            Log::Error("Window::Create invalid state or dimensions", E_INVALIDARG);
            return false;
        }

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        if (GetClassInfoExW(instance, kWindowClassName, &windowClass) == FALSE)
        {
            windowClass.style = CS_HREDRAW | CS_VREDRAW;
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = kWindowClassName;
            if (RegisterClassExW(&windowClass) == 0)
            {
                Log::Error("RegisterClassExW", static_cast<long>(GetLastError()));
                return false;
            }
        }
        else if (windowClass.lpfnWndProc != WindowProc)
        {
            Log::Error("Window class name belongs to another window procedure", E_UNEXPECTED);
            return false;
        }

        RECT bounds = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        if (AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, GetDpiForSystem()) == FALSE)
        {
            Log::Error("AdjustWindowRectExForDpi", static_cast<long>(GetLastError()));
            return false;
        }

        _closeRequested = false;
        _focused = false;
        _minimized = false;
        _resizePending = false;
        const HWND handle = CreateWindowExW(0, kWindowClassName, desc.title.c_str(), kWindowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, this);
        if (handle == nullptr)
        {
            Log::Error("CreateWindowExW", static_cast<long>(GetLastError()));
            Destroy();
            return false;
        }

        UpdateClientSize(handle);
        if (_closeRequested || _width == 0 || _height == 0)
        {
            Destroy();
            return false;
        }

        // The window class is shared for this process; this object owns its HWND only.
        if (desc.visible)
        {
            ShowWindow(handle, SW_SHOW);
            _focused = GetFocus() == handle;
        }
        return true;
    }

    bool Window::PumpEvents(Input& input)
    {
        if (_handle == nullptr || _closeRequested)
        {
            return false;
        }

        _input = &input;
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                _closeRequested = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (_closeRequested)
            {
                break;
            }
        }
        _input = nullptr;
        return !_closeRequested && _handle != nullptr;
    }

    void Window::Destroy()
    {
        _input = nullptr;
        if (_handle != nullptr && DestroyWindow(_handle) == FALSE)
        {
            Log::Error("DestroyWindow", static_cast<long>(GetLastError()));
            return;
        }
        _handle = nullptr;
        _width = 0;
        _height = 0;
        _focused = false;
        _minimized = false;
        _resizePending = false;
        _closeRequested = true;
    }

    HWND Window::Handle() const
    {
        return _handle;
    }

    uint32_t Window::Width() const
    {
        return _width;
    }

    uint32_t Window::Height() const
    {
        return _height;
    }

    bool Window::IsFocused() const
    {
        return _focused;
    }

    bool Window::IsMinimized() const
    {
        return _minimized;
    }

    bool Window::ConsumeResize(uint32_t& width, uint32_t& height)
    {
        if (!_resizePending)
        {
            return false;
        }
        width = _width;
        height = _height;
        _resizePending = false;
        return true;
    }

    void Window::UpdateClientSize(HWND handle)
    {
        RECT client = {};
        if (GetClientRect(handle, &client) == FALSE)
        {
            Log::Error("GetClientRect", static_cast<long>(GetLastError()));
            _closeRequested = true;
            return;
        }
        _width = static_cast<uint32_t>(client.right - client.left);
        _height = static_cast<uint32_t>(client.bottom - client.top);
        _resizePending = true;
    }

    LRESULT CALLBACK Window::WindowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            window = static_cast<Window*>(create->lpCreateParams);
            SetLastError(ERROR_SUCCESS);
            if (SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window)) == 0 &&
                GetLastError() != ERROR_SUCCESS)
            {
                return FALSE;
            }
            window->_handle = handle;
        }
        if (window == nullptr)
        {
            return DefWindowProcW(handle, message, wParam, lParam);
        }
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
            window->_handle = nullptr;
            window->_focused = false;
            window->_closeRequested = true;
            return DefWindowProcW(handle, message, wParam, lParam);
        }
        return window->ProcessMessage(handle, message, wParam, lParam);
    }

    LRESULT Window::ProcessMessage(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        const auto key = [this](int virtualKey, bool down)
        {
            if (_input != nullptr)
            {
                _input->OnKey(static_cast<Key>(virtualKey), down);
            }
        };

        switch (message)
        {
        case WM_CLOSE:
        case WM_DESTROY:
            _closeRequested = true;
            _focused = false;
            return 0;
        case WM_SIZE:
            _width = LOWORD(lParam);
            _height = HIWORD(lParam);
            _resizePending = true;
            _minimized = wParam == SIZE_MINIMIZED || _width == 0 || _height == 0;
            if (!_minimized)
            {
                // A restored window publishes its current client size, not a stale size message.
                UpdateClientSize(handle);
                _minimized = _width == 0 || _height == 0;
            }
            return 0;
        case WM_DPICHANGED:
        {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (SetWindowPos(handle, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE) == FALSE)
            {
                Log::Error("SetWindowPos after WM_DPICHANGED", static_cast<long>(GetLastError()));
                _closeRequested = true;
            }
            return 0;
        }
        case WM_SETFOCUS:
            _focused = IsWindowVisible(handle) != FALSE;
            return 0;
        case WM_KILLFOCUS:
            _focused = false;
            if (_input != nullptr)
            {
                _input->Clear();
            }
            if (GetCapture() == handle)
            {
                ReleaseCapture();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
            key(static_cast<int>(wParam), message == WM_KEYDOWN);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            key(static_cast<int>(wParam), message == WM_SYSKEYDOWN);
            break;
        case WM_MOUSEMOVE:
            if (_input != nullptr)
            {
                _input->OnMouseMove(static_cast<float>(static_cast<short>(LOWORD(lParam))),
                    static_cast<float>(static_cast<short>(HIWORD(lParam))));
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
        {
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
            int virtualKey = VK_LBUTTON;
            if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
            {
                virtualKey = VK_RBUTTON;
            }
            else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            {
                virtualKey = VK_MBUTTON;
            }
            else if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)
            {
                virtualKey = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            }
            key(virtualKey, down);
            if (down)
            {
                SetCapture(handle);
            }
            else if (GetCapture() == handle &&
                (LOWORD(wParam) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) == 0)
            {
                ReleaseCapture();
            }
            return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ? TRUE : 0;
        }
        case WM_CAPTURECHANGED:
            key(VK_LBUTTON, false);
            key(VK_RBUTTON, false);
            key(VK_MBUTTON, false);
            key(VK_XBUTTON1, false);
            key(VK_XBUTTON2, false);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }
        return DefWindowProcW(handle, message, wParam, lParam);
    }
}
