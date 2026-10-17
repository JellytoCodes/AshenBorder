#include "Scene/Geometry.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
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

    bool ClipRange(double start, double delta, double low, double high,
        double& first, double& last) noexcept
    {
        if (delta == 0.0)
        {
            return start >= low && start <= high;
        }
        const double a = (low - start) / delta;
        const double b = (high - start) / delta;
        first = std::max(first, std::min(a, b));
        last = std::min(last, std::max(a, b));
        return first <= last;
    }

    Engine::Vector3 BoxNormal(double x, double z, double halfX, double halfZ,
        double cosine, double sine) noexcept
    {
        double normalX = x - std::clamp(x, -halfX, halfX);
        double normalZ = z - std::clamp(z, -halfZ, halfZ);
        const double length = std::hypot(normalX, normalZ);
        if (length > 0.0)
        {
            normalX /= length;
            normalZ /= length;
        }
        else
        {
            // An initial overlap needs an outward escape normal, including points inside the box.
            // Equal face distances choose +X, matching the concentric cylinder fallback.
            double nearest = halfX - x;
            normalX = 1.0;
            normalZ = 0.0;
            if (halfX + x < nearest)
            {
                nearest = halfX + x;
                normalX = -1.0;
            }
            if (halfZ - z < nearest)
            {
                nearest = halfZ - z;
                normalX = 0.0;
                normalZ = 1.0;
            }
            if (halfZ + z < nearest)
            {
                normalX = 0.0;
                normalZ = -1.0;
            }
        }
        return {static_cast<float>(normalX * cosine + normalZ * sine), 0.0f,
            static_cast<float>(normalZ * cosine - normalX * sine)};
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

    bool IsValid(const UprightBox& box) noexcept
    {
        return Finite(box.base) && Nonnegative(box.halfExtents.x) && Nonnegative(box.halfExtents.y)
            && Nonnegative(box.height) && std::isfinite(box.yaw);
    }

    bool Overlaps(const Cylinder& cylinder, const UprightBox& box) noexcept
    {
        if (!IsValid(cylinder) || !IsValid(box)
            || !HeightOverlap(cylinder.base.y, cylinder.height, box.base.y, box.height))
        {
            return false;
        }
        const double cosine = std::cos(static_cast<double>(box.yaw));
        const double sine = std::sin(static_cast<double>(box.yaw));
        const double offsetX = static_cast<double>(cylinder.base.x) - box.base.x;
        const double offsetZ = static_cast<double>(cylinder.base.z) - box.base.z;
        const double x = offsetX * cosine - offsetZ * sine;
        const double z = offsetX * sine + offsetZ * cosine;
        const double nearestX = std::max(0.0, std::abs(x) - box.halfExtents.x);
        const double nearestZ = std::max(0.0, std::abs(z) - box.halfExtents.y);
        return std::hypot(nearestX, nearestZ) <= cylinder.radius;
    }

    std::optional<GeometryHit> SweepCylinder(const Segment& baseMotion, float movingRadius,
        float movingHeight, const UprightBox& target) noexcept
    {
        if (!Finite(baseMotion.start) || !Finite(baseMotion.end) || !Nonnegative(movingRadius)
            || !Nonnegative(movingHeight) || !IsValid(target))
        {
            return std::nullopt;
        }
        const double cosine = std::cos(static_cast<double>(target.yaw));
        const double sine = std::sin(static_cast<double>(target.yaw));
        const double offsetX = static_cast<double>(baseMotion.start.x) - target.base.x;
        const double offsetZ = static_cast<double>(baseMotion.start.z) - target.base.z;
        const double worldDeltaX = static_cast<double>(baseMotion.end.x) - baseMotion.start.x;
        const double deltaY = static_cast<double>(baseMotion.end.y) - baseMotion.start.y;
        const double worldDeltaZ = static_cast<double>(baseMotion.end.z) - baseMotion.start.z;
        const double x = offsetX * cosine - offsetZ * sine;
        const double z = offsetX * sine + offsetZ * cosine;
        const double deltaX = worldDeltaX * cosine - worldDeltaZ * sine;
        const double deltaZ = worldDeltaX * sine + worldDeltaZ * cosine;
        const double halfX = target.halfExtents.x;
        const double halfZ = target.halfExtents.y;
        const double radius = movingRadius;
        if (HeightOverlap(baseMotion.start.y, movingHeight, target.base.y, target.height)
            && std::hypot(std::max(0.0, std::abs(x) - halfX), std::max(0.0, std::abs(z) - halfZ)) <= radius)
        {
            return GeometryHit{baseMotion.start, BoxNormal(x, z, halfX, halfZ, cosine, sine), 0.0f};
        }
        const double infinity = std::numeric_limits<double>::infinity();
        double verticalFirst = -infinity;
        double verticalLast = infinity;
        if (!ClipRange(baseMotion.start.y, deltaY, static_cast<double>(target.base.y) - movingHeight,
            static_cast<double>(target.base.y) + target.height, verticalFirst, verticalLast))
        {
            return std::nullopt;
        }

        double fraction = infinity;
        bool verticalEntry = false;
        const auto consider = [&](double horizontalFirst, double horizontalLast)
        {
            const double first = std::max({0.0, horizontalFirst, verticalFirst});
            const double last = std::min({1.0, horizontalLast, verticalLast});
            if (first <= last && first < fraction)
            {
                fraction = first;
                verticalEntry = first > 0.0 && verticalFirst > horizontalFirst;
            }
        };
        const auto rectangle = [&](double lowX, double highX, double lowZ, double highZ)
        {
            double first = -infinity;
            double last = infinity;
            if (ClipRange(x, deltaX, lowX, highX, first, last)
                && ClipRange(z, deltaZ, lowZ, highZ, first, last))
            {
                consider(first, last);
            }
        };
        // The exact disc-expanded rectangle is the union of two strips and four corner discs.
        // Expanding both slab axes by radius alone would block empty space near each corner.
        rectangle(-halfX - radius, halfX + radius, -halfZ, halfZ);
        rectangle(-halfX, halfX, -halfZ - radius, halfZ + radius);
        const double speedXZ = std::hypot(deltaX, deltaZ);
        for (int sideX : {-1, 1})
        {
            for (int sideZ : {-1, 1})
            {
                const double cornerX = x - sideX * halfX;
                const double cornerZ = z - sideZ * halfZ;
                if (speedXZ == 0.0)
                {
                    if (std::hypot(cornerX, cornerZ) <= radius)
                    {
                        consider(-infinity, infinity);
                    }
                    continue;
                }
                const double perpendicular = std::abs(std::fma(cornerX, deltaZ, -cornerZ * deltaX) / speedXZ);
                if (perpendicular <= radius)
                {
                    const double along = std::fma(cornerX, deltaX, cornerZ * deltaZ) / speedXZ;
                    const double extent = std::sqrt(std::max(0.0, (radius - perpendicular) * (radius + perpendicular)));
                    consider((-along - extent) / speedXZ, (-along + extent) / speedXZ);
                }
            }
        }
        if (!std::isfinite(fraction))
        {
            return std::nullopt;
        }
        GeometryHit hit;
        hit.position = {
            static_cast<float>(std::lerp(static_cast<double>(baseMotion.start.x), static_cast<double>(baseMotion.end.x), fraction)),
            static_cast<float>(std::lerp(static_cast<double>(baseMotion.start.y), static_cast<double>(baseMotion.end.y), fraction)),
            static_cast<float>(std::lerp(static_cast<double>(baseMotion.start.z), static_cast<double>(baseMotion.end.z), fraction))};
        hit.fraction = static_cast<float>(fraction);
        hit.normal = verticalEntry ? Vector3{0.0f, deltaY > 0.0 ? -1.0f : 1.0f, 0.0f}
            : BoxNormal(std::fma(deltaX, fraction, x), std::fma(deltaZ, fraction, z), halfX, halfZ, cosine, sine);
        return hit;
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
