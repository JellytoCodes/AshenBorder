#pragma once

#include <cstdint>

namespace Engine
{
    class FrameClock
    {
    public:
        bool Configure(double fixedSeconds, std::uint32_t maxCatchUp) noexcept;
        float BeginFrame();
        bool AddElapsed(double seconds) noexcept;
        bool ConsumeFixedStep() noexcept;
        double DiscardExcess() noexcept;
        void Reset() noexcept;
        double FixedSeconds() const noexcept;

    private:
        std::int64_t _frequency = 0;
        std::int64_t _previousCounter = 0;
        double _fixedSeconds = 1.0 / 60.0;
        double _accumulator = 0.0;
        std::uint32_t _maxCatchUp = 4;
        std::uint32_t _stepsThisFrame = 0;
        bool _configured = false;
    };
}
