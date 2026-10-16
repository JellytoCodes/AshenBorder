#pragma once

#include "Types/Math.h"

namespace Engine
{
    class Transform
    {
    public:
        bool SetPosition(Vector3 value) noexcept;
        bool SetRotation(Quaternion value) noexcept;
        bool SetScale(Vector3 value) noexcept;
        Vector3 Position() const noexcept;
        Quaternion Rotation() const noexcept;
        Vector3 Scale() const noexcept;
        Matrix WorldMatrix() const noexcept;

    private:
        Vector3 _position = {0.0f, 0.0f, 0.0f};
        Quaternion _rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        Vector3 _scale = {1.0f, 1.0f, 1.0f};
    };
}
