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
            static_cast<float>(desc.fixedSeconds) <= 0.0f || desc.maxCatchUp == 0)
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

                if (!_window.Create(_desc) || !_graphics.Initialize(_window) ||
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
        try
        {
            while (!_exitRequested)
            {
                _input.BeginFrame();
                if (!_window.PumpEvents(_input))
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

                const bool inactive = !_window.IsFocused() || _window.IsMinimized();
                if (inactive != wasInactive)
                {
                    _input.Clear();
                    _clock.Reset();
                    wasInactive = inactive;
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

                _frame.Clear();
                _app->BuildRenderFrame(_frame);
                if (_exitRequested)
                {
                    break;
                }
                if (!_window.IsMinimized() && _window.Width() > 0 && _window.Height() > 0)
                {
                    if (!_renderer->Render(_frame) || !_graphics.Present())
                    {
                        result = 1;
                        break;
                    }
                }
                if (inactive)
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
