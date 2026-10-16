#pragma once

#include "Types/Math.h"

#include <optional>

namespace Engine
{
    struct Segment
    {
        Vector3 start = {0.0f, 0.0f, 0.0f};
        Vector3 end = {0.0f, 0.0f, 0.0f};
    };

    struct BoundsXZ
    {
        Vector2 min = {0.0f, 0.0f};
        Vector2 max = {0.0f, 0.0f};
    };

    struct Cylinder
    {
        // Upright XZ disc, with the vertical interval [base.y, base.y + height].
        Vector3 base = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        float height = 0.0f;
    };

    struct GeometryHit
    {
        // Moving base at fraction; unlike Terrain SurfaceHit, this is not a surface contact point.
        Vector3 position = {0.0f, 0.0f, 0.0f};
        // Target-to-moving entry normal; coincident/degenerate XZ direction falls back to +X.
        Vector3 normal = {1.0f, 0.0f, 0.0f};
        float fraction = 0.0f;
    };

    // Zero radius/height and closed-boundary contact are valid. Nonfinite/negative inputs fail.
    bool IsValid(const Cylinder& cylinder) noexcept;
    bool Overlaps(const Cylinder& first, const Cylinder& second) noexcept;
    std::optional<GeometryHit> SweepCylinder(const Segment& baseMotion, float movingRadius,
        float movingHeight, const Cylinder& target) noexcept;

    // Closed filled sector, vertical [origin.y, origin.y + height], half angle in [0, pi].
    // forward is an XZ direction (x, z); a zero/nonfinite direction is invalid.
    bool IntersectsSector(const Cylinder& target, Vector3 origin, Vector2 forward,
        float range, float halfAngleRadians, float height) noexcept;

    // Returns +infinity for a nonfinite input; finite Y is otherwise ignored.
    double DistanceSquaredXZ(Vector3 first, Vector3 second) noexcept;
}
