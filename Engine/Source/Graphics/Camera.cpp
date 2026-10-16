#include "Graphics/Camera.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace
{
    bool TryStoreMatrix(const std::array<double, 16>& values, Engine::Matrix& out) noexcept
    {
        std::array<float, 16> stored;
        for (size_t index = 0; index < values.size(); ++index)
        {
            if (!std::isfinite(values[index]) ||
                std::abs(values[index]) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                return false;
            }
            stored[index] = static_cast<float>(values[index]);
        }
        out = Engine::Matrix(stored.data());
        return true;
    }
}

namespace Engine
{
    bool Camera::SetView(Vector3 eye, Vector3 target) noexcept
    {
        if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z) ||
            !std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z))
        {
            return false;
        }

        const double dx = static_cast<double>(target.x) - eye.x;
        const double dy = static_cast<double>(target.y) - eye.y;
        const double dz = static_cast<double>(target.z) - eye.z;
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double horizontalLength = std::sqrt(dx * dx + dz * dz);
        if (length == 0.0 || horizontalLength == 0.0)
        {
            return false;
        }

        // Double intermediates keep finite float inputs from overflowing before validation.
        const double zx = dx / length;
        const double zy = dy / length;
        const double zz = dz / length;
        const double xx = dz / horizontalLength;
        const double xz = -dx / horizontalLength;
        const double yx = zy * xz;
        const double yy = zz * xx - zx * xz;
        const double yz = -zy * xx;
        Matrix view;
        if (!TryStoreMatrix({
            xx, yx, zx, 0.0,
            0.0, yy, zy, 0.0,
            xz, yz, zz, 0.0,
            -(xx * eye.x + xz * eye.z),
            -(yx * eye.x + yy * eye.y + yz * eye.z),
            -(zx * eye.x + zy * eye.y + zz * eye.z), 1.0}, view))
        {
            return false;
        }

        _data.position = eye;
        _data.view = view;
        return true;
    }

    bool Camera::SetPerspective(float fovYRadians, float aspect, float nearZ, float farZ) noexcept
    {
        if (!std::isfinite(fovYRadians) || !std::isfinite(aspect) ||
            !std::isfinite(nearZ) || !std::isfinite(farZ) ||
            fovYRadians <= 0.0f || static_cast<double>(fovYRadians) >= std::numbers::pi ||
            aspect <= 0.0f || nearZ <= 0.0f || farZ <= nearZ)
        {
            return false;
        }

        // The LH row-vector projection avoids SDK debug asserts for very small valid inputs.
        const double height = 1.0 / std::tan(static_cast<double>(fovYRadians) * 0.5);
        const double width = height / aspect;
        const double range = static_cast<double>(farZ) / (static_cast<double>(farZ) - nearZ);
        Matrix projection;
        if (!TryStoreMatrix({
            width, 0.0, 0.0, 0.0,
            0.0, height, 0.0, 0.0,
            0.0, 0.0, range, 1.0,
            0.0, 0.0, -range * nearZ, 0.0}, projection) ||
            projection._11 <= 0.0f || projection._22 <= 0.0f || projection._43 >= 0.0f)
        {
            return false;
        }

        _data.projection = projection;
        return true;
    }

    CameraData Camera::Snapshot() const noexcept
    {
        return _data;
    }
}
