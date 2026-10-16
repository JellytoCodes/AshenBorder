#include "Core/FrameClock.h"

#include "Core/Log.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine
{
    bool FrameClock::Configure(double fixedSeconds, std::uint32_t maxCatchUp) noexcept
    {
        if (!std::isfinite(fixedSeconds) || fixedSeconds <= 0.0 || maxCatchUp == 0)
        {
            return false;
        }

        LARGE_INTEGER frequency{};
        LARGE_INTEGER counter{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0
            || !QueryPerformanceCounter(&counter))
        {
            return false;
        }

        _frequency = frequency.QuadPart;
        _previousCounter = counter.QuadPart;
        _fixedSeconds = fixedSeconds;
        _maxCatchUp = maxCatchUp;
        _accumulator = 0.0;
        _stepsThisFrame = 0;
        _configured = true;
        return true;
    }

    float FrameClock::BeginFrame()
    {
        if (!_configured)
        {
            Log::Write("FrameClock::BeginFrame called before successful Configure.");
            return std::numeric_limits<float>::quiet_NaN();
        }

        LARGE_INTEGER counter{};
        if (!QueryPerformanceCounter(&counter))
        {
            Log::Error("QueryPerformanceCounter failed.", static_cast<long>(GetLastError()));
            return std::numeric_limits<float>::quiet_NaN();
        }

        const double elapsed = (static_cast<double>(counter.QuadPart)
            - static_cast<double>(_previousCounter)) / static_cast<double>(_frequency);
        if (!AddElapsed(elapsed))
        {
            Log::Write("FrameClock received invalid or overflowing elapsed time.");
            return std::numeric_limits<float>::quiet_NaN();
        }

        _previousCounter = counter.QuadPart;
        return static_cast<float>(elapsed);
    }

    bool FrameClock::AddElapsed(double seconds) noexcept
    {
        if (!_configured || !std::isfinite(seconds) || seconds < 0.0)
        {
            return false;
        }

        const double accumulated = _accumulator + seconds;
        if (!std::isfinite(accumulated))
        {
            return false;
        }

        _accumulator = accumulated;
        _stepsThisFrame = 0;
        return true;
    }

    bool FrameClock::ConsumeFixedStep() noexcept
    {
        if (!_configured || _stepsThisFrame >= _maxCatchUp)
        {
            return false;
        }

        const double tolerance = _fixedSeconds * 1.0e-9;
        if (_accumulator < _fixedSeconds && _fixedSeconds - _accumulator > tolerance)
        {
            return false;
        }

        _accumulator = std::max(0.0, _accumulator - _fixedSeconds);
        ++_stepsThisFrame;
        return true;
    }

    double FrameClock::DiscardExcess() noexcept
    {
        if (!_configured)
        {
            return 0.0;
        }

        const double tolerance = _fixedSeconds * 1.0e-9;
        if (_accumulator < _fixedSeconds && _fixedSeconds - _accumulator > tolerance)
        {
            return 0.0;
        }

        double remainder = std::fmod(_accumulator, _fixedSeconds);
        if (_fixedSeconds - remainder <= tolerance)
        {
            remainder = 0.0;
        }

        const double discarded = _accumulator - remainder;
        _accumulator = remainder;
        return discarded;
    }

    void FrameClock::Reset() noexcept
    {
        _accumulator = 0.0;
        _stepsThisFrame = 0;
        if (_configured)
        {
            LARGE_INTEGER counter{};
            if (QueryPerformanceCounter(&counter))
            {
                _previousCounter = counter.QuadPart;
            }
            else
            {
                _configured = false;
            }
        }
    }

    double FrameClock::FixedSeconds() const noexcept
    {
        return _fixedSeconds;
    }
}
