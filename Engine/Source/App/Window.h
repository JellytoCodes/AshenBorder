#pragma once

#include <Windows.h>
#include <cstdint>

namespace Engine
{
    struct ApplicationDesc;
    class Input;

    class Window
    {
    public:
        Window() = default;
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) = delete;
        Window& operator=(Window&&) = delete;

        bool Create(const ApplicationDesc& desc);
        bool PumpEvents(Input& input);
        void Destroy();

        HWND Handle() const;
        uint32_t Width() const;
        uint32_t Height() const;
        bool IsFocused() const;
        bool IsMinimized() const;
        bool ConsumeResize(uint32_t& width, uint32_t& height);

    private:
        static LRESULT CALLBACK WindowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT ProcessMessage(HWND handle, UINT message, WPARAM wParam, LPARAM lParam);
        void UpdateClientSize(HWND handle);

        HWND _handle = nullptr;
        Input* _input = nullptr;
        uint32_t _width = 0;
        uint32_t _height = 0;
        bool _focused = false;
        bool _minimized = false;
        bool _resizePending = false;
        bool _closeRequested = false;
    };
}
