#pragma once

#include "Types/Math.h"

#include <bitset>
#include <cstddef>
#include <cstdint>

namespace Engine
{
    inline constexpr std::size_t kKeyCount = 256;

    enum class Key : std::uint16_t
    {
        MouseLeft = 0x01,
        MouseRight = 0x02,
        MouseMiddle = 0x04,
        MouseX1 = 0x05,
        MouseX2 = 0x06,
        Enter = 0x0D,
        Escape = 0x1B,
        Space = 0x20,
        A = 0x41,
        D = 0x44,
        M = 0x4D,
        P = 0x50,
        Q = 0x51,
        R = 0x52,
        S = 0x53,
        W = 0x57,
        F2 = 0x71,
        F3 = 0x72,
        F4 = 0x73,
        F5 = 0x74,
        F6 = 0x75,
        F7 = 0x76,
        F8 = 0x77,
        F9 = 0x78,
        F10 = 0x79,
        F11 = 0x7A,
        F12 = 0x7B
    };

    struct InputState
    {
        std::bitset<kKeyCount> held;
        std::bitset<kKeyCount> pressed;
        std::bitset<kKeyCount> released;
        Vector2 mousePosition{0.0f, 0.0f};

        bool IsHeld(Key key) const noexcept;
        bool IsPressed(Key key) const noexcept;
        bool IsReleased(Key key) const noexcept;
    };

    class Input
    {
    public:
        void BeginFrame() noexcept;
        void OnKey(Key key, bool down) noexcept;
        void OnMouseMove(float x, float y) noexcept;
        const InputState& FrameState() const noexcept;
        InputState ConsumeFixedState() noexcept;
        void Clear() noexcept;

    private:
        InputState _frame;
        InputState _fixedPending;
    };
}
