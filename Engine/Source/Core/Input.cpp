#include "Core/Input.h"

#include <cmath>

namespace Engine
{
    bool InputState::IsHeld(Key key) const noexcept
    {
        const auto index = static_cast<std::size_t>(key);
        return index < kKeyCount && held.test(index);
    }

    bool InputState::IsPressed(Key key) const noexcept
    {
        const auto index = static_cast<std::size_t>(key);
        return index < kKeyCount && pressed.test(index);
    }

    bool InputState::IsReleased(Key key) const noexcept
    {
        const auto index = static_cast<std::size_t>(key);
        return index < kKeyCount && released.test(index);
    }

    void Input::BeginFrame() noexcept
    {
        _frame.pressed.reset();
        _frame.released.reset();
    }

    void Input::OnKey(Key key, bool down) noexcept
    {
        const auto index = static_cast<std::size_t>(key);
        if (index >= kKeyCount || _frame.held.test(index) == down)
        {
            return;
        }

        _frame.held.set(index, down);
        _fixedPending.held.set(index, down);
        if (down)
        {
            _frame.pressed.set(index);
            _fixedPending.pressed.set(index);
        }
        else
        {
            _frame.released.set(index);
            _fixedPending.released.set(index);
        }
    }

    void Input::OnMouseMove(float x, float y) noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            return;
        }

        _frame.mousePosition = Vector2{x, y};
        _fixedPending.mousePosition = _frame.mousePosition;
    }

    const InputState& Input::FrameState() const noexcept
    {
        return _frame;
    }

    InputState Input::ConsumeFixedState() noexcept
    {
        const InputState result = _fixedPending;
        _fixedPending.pressed.reset();
        _fixedPending.released.reset();
        return result;
    }

    void Input::Clear() noexcept
    {
        _frame = InputState{};
        _fixedPending = InputState{};
    }
}
