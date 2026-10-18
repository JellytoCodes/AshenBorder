#include "App/Application.h"

#include "Core/Log.h"

#include <cmath>
#include <exception>
#include <limits>
#include <string>

namespace Engine
{
    Application::~Application()
    {
        Shutdown();
    }

    bool Application::Initialize(const ApplicationDesc& desc, std::unique_ptr<IGameApp> app)
    {
        if (_initialized || _running || _initializing || _shuttingDown || _app || !app || desc.title.empty() ||
            desc.width == 0 || desc.height == 0 || desc.width > 16384 ||
            desc.height > 16384 || !std::isfinite(desc.fixedSeconds) ||
            desc.fixedSeconds <= 0.0 ||
            desc.fixedSeconds > static_cast<double>(std::numeric_limits<float>::max()) ||
            !std::isfinite(static_cast<float>(desc.fixedSeconds)) ||
            static_cast<float>(desc.fixedSeconds) <= 0.0f || desc.maxCatchUp == 0 || desc.maximumFrameLatency > 16)
        {
            Log::Write("Application: invalid initialization arguments or already initialized");
            return false;
        }

        bool success = false;
        _initializing = true;
        try
        {
            success = [&]()
            {
                _desc = desc;
                if (!Log::Open(_desc.logFile))
                {
                    return false;
                }
                _logOpened = true;
                _app = std::move(app);
                _exitRequested = false;
                _simulationPaused = false;

                if (_desc.maxFramesPerSecond != 0 || _desc.backgroundFramesPerSecond != 0)
                {
                    LARGE_INTEGER frequency{};
                    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
                    {
                        Log::Write("Application: frame pacing frequency failed");
                        return false;
                    }
                    _frameCounterFrequency = frequency.QuadPart;
                    _frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                        TIMER_MODIFY_STATE | SYNCHRONIZE);
                    if (_frameTimer == nullptr && GetLastError() == ERROR_INVALID_PARAMETER)
                    {
                        _frameTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
                    }
                    if (_frameTimer == nullptr)
                    {
                        Log::Error("Application: create frame timer", static_cast<long>(GetLastError()));
                        return false;
                    }
                }

                if (!_window.Create(_desc) || !_graphics.Initialize(_window, _desc.maximumFrameLatency) ||
                    !_clock.Configure(_desc.fixedSeconds, _desc.maxCatchUp))
                {
                    return false;
                }

                _renderer = std::make_unique<Renderer>(_graphics);
                if (!_renderer->Initialize(_desc.shaderRoot))
                {
                    return false;
                }

                _input.Clear();
                _clientEntered = true;
                if (!_app->Initialize(*this) || _exitRequested)
                {
                    Log::Write("Application: client initialization failed or cancelled");
                    return false;
                }

                _clock.Reset();
                _initialized = true;
                Log::Write("Application: frame limits active=" + std::to_string(_desc.maxFramesPerSecond) +
                    ", background=" + std::to_string(_desc.backgroundFramesPerSecond) +
                    ", vsync=" + std::to_string(_desc.vsync) +
                    ", maximum frame latency=" + std::to_string(_desc.maximumFrameLatency));
                Log::Write("Application: initialized");
                return true;
            }();
        }
        catch (const std::exception& error)
        {
            Log::Write(std::string("Application initialization exception: ") + error.what());
        }
        catch (...)
        {
            Log::Write("Application initialization exception");
        }
        _initializing = false;
        if (!success)
        {
            Shutdown();
        }
        return success;
    }

    int Application::Run()
    {
        if (!_initialized || !_app || _running)
        {
            Log::Write("Application: Run requires an initialized, idle application");
            return 1;
        }

        _running = true;
        int result = 0;
        bool wasInactive = !_window.IsFocused() || _window.IsMinimized();
        std::int64_t lastFrameStart = 0;
        try
        {
            while (!_exitRequested)
            {
                _input.BeginFrame();
                bool inactive = wasInactive;
                for (;;)
                {
                    if (!_window.PumpEvents(_input))
                    {
                        RequestExit();
                        break;
                    }
                    inactive = !_window.IsFocused() || _window.IsMinimized();
                    if (inactive != wasInactive)
                    {
                        _input.Clear();
                        _clock.Reset();
                        wasInactive = inactive;
                    }
                    if (_frameTimer == nullptr)
                    {
                        break;
                    }
                    LARGE_INTEGER now{};
                    if (!QueryPerformanceCounter(&now))
                    {
                        Log::Write("Application: frame pacing counter failed");
                        result = 1;
                        RequestExit();
                        break;
                    }
                    const auto rate = inactive ? _desc.backgroundFramesPerSecond : _desc.maxFramesPerSecond;
                    const auto period = rate == 0 ? 0 : _frameCounterFrequency / rate + (_frameCounterFrequency % rate != 0);
                    const auto remaining = period - (now.QuadPart - lastFrameStart);
                    if (rate == 0 || lastFrameStart == 0 || remaining <= 0)
                    {
                        lastFrameStart = now.QuadPart;
                        break;
                    }
                    // Keep input edges across wake-ups; messages do not authorize an early frame.
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>(std::ceil(
                        static_cast<double>(remaining) * 10000000.0 / _frameCounterFrequency));
                    if (!SetWaitableTimer(_frameTimer, &due, 0, nullptr, nullptr, FALSE))
                    {
                        Log::Error("Application: set frame timer", static_cast<long>(GetLastError()));
                        result = 1;
                        RequestExit();
                        break;
                    }
                    if (MsgWaitForMultipleObjectsEx(1, &_frameTimer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                    {
                        Log::Error("Application: wait for frame or input", static_cast<long>(GetLastError()));
                        result = 1;
                        RequestExit();
                        break;
                    }
                }
                if (_exitRequested)
                {
                    break;
                }

                uint32_t width = 0;
                uint32_t height = 0;
                if (_window.ConsumeResize(width, height) && width > 0 && height > 0)
                {
                    if (!_graphics.Resize(width, height))
                    {
                        result = 1;
                        break;
                    }
                }

                const float measuredDt = _clock.BeginFrame();
                if (!std::isfinite(measuredDt) || measuredDt < 0.0f)
                {
                    Log::Write("Application: frame clock failed");
                    result = 1;
                    break;
                }
                const float frameDt = inactive ? 0.0f : measuredDt;
                if (inactive || _simulationPaused)
                {
                    _clock.Reset();
                }
                else
                {
                    while (_clock.ConsumeFixedStep())
                    {
                        const InputState fixedInput = _input.ConsumeFixedState();
                        _app->FixedUpdate(fixedInput, static_cast<float>(_desc.fixedSeconds));
                        if (_exitRequested)
                        {
                            break;
                        }
                    }
                    const double discarded = _clock.DiscardExcess();
                    if (discarded > 0.0)
                    {
                        Log::Write("Application: discarded catch-up seconds=" + std::to_string(discarded));
                    }
                }
                if (_exitRequested)
                {
                    break;
                }

                const AppAction action = _app->Update(_input.FrameState(), frameDt);
                if (action == AppAction::Exit)
                {
                    RequestExit();
                }
                else if (action == AppAction::PauseSimulation || action == AppAction::ResumeSimulation)
                {
                    const bool pause = action == AppAction::PauseSimulation;
                    if (pause != _simulationPaused)
                    {
                        _simulationPaused = pause;
                        _clock.Reset();
                        _input.Clear();
                        Log::Write(pause ? "Application: simulation paused" : "Application: simulation resumed");
                    }
                }
                if (_exitRequested)
                {
                    break;
                }

                if (!_window.IsMinimized() && _window.Width() > 0 && _window.Height() > 0)
                {
                    _frame.Clear();
                    _app->BuildRenderFrame(_frame);
                    if (_exitRequested)
                    {
                        break;
                    }
                    if (!_renderer->Render(_frame) || !_graphics.Present(_desc.vsync ? 1 : 0))
                    {
                        result = 1;
                        break;
                    }
                }
                if (inactive && _desc.backgroundFramesPerSecond == 0)
                {
                    Sleep(8);
                }
            }
        }
        catch (const std::exception& error)
        {
            Log::Write(std::string("Application update exception: ") + error.what());
            result = 1;
        }
        catch (...)
        {
            Log::Write("Application update exception");
            result = 1;
        }
        _running = false;
        Log::Write("Application: Run finished, code=" + std::to_string(result));
        return result;
    }

    void Application::Shutdown()
    {
        _exitRequested = true;
        if (_running || _initializing || _shuttingDown)
        {
            return;
        }

        _shuttingDown = true;
        _initialized = false;
        if (_clientEntered)
        {
            _clientEntered = false;
            try
            {
                _app->Shutdown();
            }
            catch (...)
            {
                Log::Write("Application: client Shutdown threw an exception");
            }
        }
        _app.reset();
        _frame.Clear();
        if (_renderer)
        {
            _renderer->Shutdown();
            _renderer.reset();
        }
        _input.Clear();
        _clock.Reset();
        if (_frameTimer != nullptr)
        {
            if (!CloseHandle(_frameTimer))
            {
                Log::Error("Application: close frame timer", static_cast<long>(GetLastError()));
            }
            _frameTimer = nullptr;
        }
        _frameCounterFrequency = 0;
        _graphics.Shutdown();
        _window.Destroy();
        if (_logOpened)
        {
            Log::Write("Application: shutdown complete");
            Log::Close();
            _logOpened = false;
        }
        _shuttingDown = false;
    }

    void Application::RequestExit()
    {
        _exitRequested = true;
    }

    GraphicsDevice& Application::Graphics()
    {
        return _graphics;
    }

    const GraphicsDevice& Application::Graphics() const
    {
        return _graphics;
    }

    HWND Application::NativeWindow() const
    {
        return _window.Handle();
    }

    bool Application::SimulationPaused() const
    {
        return _simulationPaused;
    }
}
