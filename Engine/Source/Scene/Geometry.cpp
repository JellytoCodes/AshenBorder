#include "Scene/Geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace
{
    bool Finite(Engine::Vector3 value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool Nonnegative(float value) noexcept
    {
        return std::isfinite(value) && value >= 0.0f;
    }

    bool HeightOverlap(double firstBase, double firstHeight, double secondBase, double secondHeight) noexcept
    {
        return firstBase <= secondBase + secondHeight && secondBase <= firstBase + firstHeight;
    }

    Engine::GeometryHit HitAt(const Engine::Segment& motion, const Engine::Cylinder& target,
        double fraction, bool verticalEntry, double deltaY) noexcept
    {
        const double x = std::lerp(static_cast<double>(motion.start.x), static_cast<double>(motion.end.x), fraction);
        const double y = std::lerp(static_cast<double>(motion.start.y), static_cast<double>(motion.end.y), fraction);
        const double z = std::lerp(static_cast<double>(motion.start.z), static_cast<double>(motion.end.z), fraction);
        Engine::GeometryHit hit;
        hit.position = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        hit.fraction = static_cast<float>(fraction);

        if (verticalEntry)
        {
            hit.normal = {0.0f, deltaY > 0.0 ? -1.0f : 1.0f, 0.0f};
        }
        else
        {
            const double offsetX = x - target.base.x;
            const double offsetZ = z - target.base.z;
            const double length = std::hypot(offsetX, offsetZ);
            if (length > 0.0)
            {
                hit.normal = {static_cast<float>(offsetX / length), 0.0f,
                    static_cast<float>(offsetZ / length)};
            }
        }
        return hit;
    }

    double DistanceToSegment(double x, double z, double endX, double endZ) noexcept
    {
        const double lengthSquared = std::fma(endX, endX, endZ * endZ);
        if (lengthSquared == 0.0)
        {
            return std::hypot(x, z);
        }
        const double fraction = std::clamp(std::fma(x, endX, z * endZ) / lengthSquared, 0.0, 1.0);
        return std::hypot(std::fma(-fraction, endX, x), std::fma(-fraction, endZ, z));
    }
}

namespace Engine
{
    bool IsValid(const Cylinder& cylinder) noexcept
    {
        return Finite(cylinder.base) && Nonnegative(cylinder.radius) && Nonnegative(cylinder.height);
    }

    bool Overlaps(const Cylinder& first, const Cylinder& second) noexcept
    {
        if (!IsValid(first) || !IsValid(second)
            || !HeightOverlap(first.base.y, first.height, second.base.y, second.height))
        {
            return false;
        }
        const double x = static_cast<double>(first.base.x) - second.base.x;
        const double z = static_cast<double>(first.base.z) - second.base.z;
        const double radius = static_cast<double>(first.radius) + second.radius;
        return std::hypot(x, z) <= radius;
    }

    std::optional<GeometryHit> SweepCylinder(const Segment& baseMotion, float movingRadius,
        float movingHeight, const Cylinder& target) noexcept
    {
        if (!Finite(baseMotion.start) || !Finite(baseMotion.end) || !Nonnegative(movingRadius)
            || !Nonnegative(movingHeight) || !IsValid(target))
        {
            return std::nullopt;
        }
        if (Overlaps({baseMotion.start, movingRadius, movingHeight}, target))
        {
            return HitAt(baseMotion, target, 0.0, false, 0.0);
        }

        const double offsetX = static_cast<double>(baseMotion.start.x) - target.base.x;
        const double offsetZ = static_cast<double>(baseMotion.start.z) - target.base.z;
        const double deltaX = static_cast<double>(baseMotion.end.x) - baseMotion.start.x;
        const double deltaY = static_cast<double>(baseMotion.end.y) - baseMotion.start.y;
        const double deltaZ = static_cast<double>(baseMotion.end.z) - baseMotion.start.z;
        const double radius = static_cast<double>(movingRadius) + target.radius;
        const double speedXZ = std::hypot(deltaX, deltaZ);
        const double infinity = std::numeric_limits<double>::infinity();
        double horizontalFirst = -infinity;
        double horizontalLast = infinity;

        if (speedXZ == 0.0)
        {
            if (std::hypot(offsetX, offsetZ) > radius)
            {
                return std::nullopt;
            }
        }
        else
        {
            // Equivalent to the quadratic roots, without subtracting two large squared path lengths.
            const double perpendicular = std::abs(std::fma(offsetX, deltaZ, -offsetZ * deltaX) / speedXZ);
            if (perpendicular > radius)
            {
                return std::nullopt;
            }
            const double along = std::fma(offsetX, deltaX, offsetZ * deltaZ) / speedXZ;
            const double extent = std::sqrt(std::max(0.0, (radius - perpendicular) * (radius + perpendicular)));
            horizontalFirst = (-along - extent) / speedXZ;
            horizontalLast = (-along + extent) / speedXZ;
        }

        const double low = static_cast<double>(target.base.y) - movingHeight;
        const double high = static_cast<double>(target.base.y) + target.height;
        double verticalFirst = -infinity;
        double verticalLast = infinity;
        if (deltaY == 0.0)
        {
            if (baseMotion.start.y < low || baseMotion.start.y > high)
            {
                return std::nullopt;
            }
        }
        else
        {
            const double first = (low - baseMotion.start.y) / deltaY;
            const double last = (high - baseMotion.start.y) / deltaY;
            verticalFirst = std::min(first, last);
            verticalLast = std::max(first, last);
        }

        const double first = std::max({0.0, horizontalFirst, verticalFirst});
        const double last = std::min({1.0, horizontalLast, verticalLast});
        if (first > last)
        {
            return std::nullopt;
        }
        const bool verticalEntry = first > 0.0 && verticalFirst > horizontalFirst;
        return HitAt(baseMotion, target, first, verticalEntry, deltaY);
    }

    bool IntersectsSector(const Cylinder& target, Vector3 origin, Vector2 forward,
        float range, float halfAngleRadians, float height) noexcept
    {
        if (!IsValid(target) || !Finite(origin) || !std::isfinite(forward.x) || !std::isfinite(forward.y)
            || !Nonnegative(range) || !Nonnegative(height) || !Nonnegative(halfAngleRadians)
            || halfAngleRadians > std::numbers::pi_v<float>)
        {
            return false;
        }
        const double forwardLength = std::hypot(static_cast<double>(forward.x), static_cast<double>(forward.y));
        if (forwardLength == 0.0 || !HeightOverlap(origin.y, height, target.base.y, target.height))
        {
            return false;
        }

        const double x = static_cast<double>(target.base.x) - origin.x;
        const double z = static_cast<double>(target.base.z) - origin.z;
        const double distance = std::hypot(x, z);
        if (halfAngleRadians == std::numbers::pi_v<float>)
        {
            return distance <= static_cast<double>(range) + target.radius;
        }

        const double forwardX = forward.x / forwardLength;
        const double forwardZ = forward.y / forwardLength;
        const double angle = std::atan2(std::fma(forwardX, z, -forwardZ * x),
            std::fma(forwardX, x, forwardZ * z));
        if (std::abs(angle) <= halfAngleRadians)
        {
            return distance <= static_cast<double>(range) + target.radius;
        }

        // Outside the angular wedge, the nearest sector point is on one of its two radial edges.
        // Their segment endpoints also cover the circular arc endpoints and the origin.
        const double cosine = std::cos(static_cast<double>(halfAngleRadians));
        const double sine = std::sin(static_cast<double>(halfAngleRadians));
        const double firstX = static_cast<double>(range) * (forwardX * cosine - forwardZ * sine);
        const double firstZ = static_cast<double>(range) * (forwardX * sine + forwardZ * cosine);
        const double secondX = static_cast<double>(range) * (forwardX * cosine + forwardZ * sine);
        const double secondZ = static_cast<double>(range) * (forwardZ * cosine - forwardX * sine);
        return std::min(DistanceToSegment(x, z, firstX, firstZ),
            DistanceToSegment(x, z, secondX, secondZ)) <= target.radius;
    }

    double DistanceSquaredXZ(Vector3 first, Vector3 second) noexcept
    {
        if (!Finite(first) || !Finite(second))
        {
            return std::numeric_limits<double>::infinity();
        }
        const double x = static_cast<double>(first.x) - second.x;
        const double z = static_cast<double>(first.z) - second.z;
        return std::fma(x, x, z * z);
    }
}
