#pragma once

#include "App/ApplicationDesc.h"
#include "App/IGameApp.h"
#include "App/Window.h"
#include "Core/FrameClock.h"
#include "Core/Input.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/RenderFrame.h"
#include "Graphics/Renderer.h"

#include <cstdint>
#include <memory>

namespace Engine
{
    class Application
    {
    public:
        Application() = default;
        ~Application();
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        bool Initialize(const ApplicationDesc& desc, std::unique_ptr<IGameApp> app);
        int Run();
        void Shutdown();
        void RequestExit();
        GraphicsDevice& Graphics();
        const GraphicsDevice& Graphics() const;
        HWND NativeWindow() const;
        bool SimulationPaused() const;

    private:
        ApplicationDesc _desc;
        Window _window;
        GraphicsDevice _graphics;
        FrameClock _clock;
        Input _input;
        std::unique_ptr<IGameApp> _app;
        std::unique_ptr<Renderer> _renderer;
        RenderFrame _frame;
        HANDLE _frameTimer = nullptr;
        std::int64_t _frameCounterFrequency = 0;
        bool _initialized = false;
        bool _clientEntered = false;
        bool _logOpened = false;
        bool _running = false;
        bool _initializing = false;
        bool _shuttingDown = false;
        bool _exitRequested = false;
        bool _simulationPaused = false;
    };
}
