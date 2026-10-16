#pragma once

namespace Engine
{
    class Application;
    struct InputState;
    struct RenderFrame;

    enum class AppAction
    {
        Continue,
        PauseSimulation,
        ResumeSimulation,
        Exit
    };

    class IGameApp
    {
    public:
        virtual ~IGameApp() = default;

        virtual bool Initialize(Application& app) = 0;
        virtual void FixedUpdate(const InputState& input, float dt) = 0;
        virtual AppAction Update(const InputState& input, float dt) = 0;
        virtual void BuildRenderFrame(RenderFrame& out) = 0;
        virtual void Shutdown() = 0;
    };
}
