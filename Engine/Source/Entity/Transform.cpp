#include "Entity/Transform.h"

#include <cmath>

namespace Engine
{
    bool Transform::SetPosition(Vector3 value) noexcept
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
        {
            return false;
        }
        _position = value;
        return true;
    }

    bool Transform::SetRotation(Quaternion value) noexcept
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z) || !std::isfinite(value.w))
        {
            return false;
        }

        const double x = value.x;
        const double y = value.y;
        const double z = value.z;
        const double w = value.w;
        const double length = std::sqrt(x * x + y * y + z * z + w * w);
        if (length == 0.0)
        {
            return false;
        }

        _rotation = {static_cast<float>(x / length), static_cast<float>(y / length),
            static_cast<float>(z / length), static_cast<float>(w / length)};
        return true;
    }

    bool Transform::SetScale(Vector3 value) noexcept
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) ||
            value.x == 0.0f || value.y == 0.0f || value.z == 0.0f)
        {
            return false;
        }
        _scale = value;
        return true;
    }

    Vector3 Transform::Position() const noexcept
    {
        return _position;
    }

    Quaternion Transform::Rotation() const noexcept
    {
        return _rotation;
    }

    Vector3 Transform::Scale() const noexcept
    {
        return _scale;
    }

    Matrix Transform::WorldMatrix() const noexcept
    {
        const DirectX::XMMATRIX scale = DirectX::XMMatrixScaling(_scale.x, _scale.y, _scale.z);
        const DirectX::XMMATRIX rotation = DirectX::XMMatrixRotationQuaternion(
            DirectX::XMLoadFloat4(&_rotation));
        const DirectX::XMMATRIX translation = DirectX::XMMatrixTranslation(
            _position.x, _position.y, _position.z);
        Matrix world;
        DirectX::XMStoreFloat4x4(&world, scale * rotation * translation);
        return world;
    }
}
