#include "Scene/Terrain.h"

#include "Graphics/Model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

namespace
{
    using DVector = std::array<double, 3>;
    constexpr double kRoundoff = 64.0 * std::numeric_limits<double>::epsilon();

    bool Finite(Engine::Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool StoreFloat(double value, float& output)
    {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        {
            return false;
        }
        output = static_cast<float>(value);
        return true;
    }

    DVector Double(Engine::Vector3 value)
    {
        return {value.x, value.y, value.z};
    }

    DVector Add(const DVector& left, const DVector& right)
    {
        return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
    }

    DVector Subtract(const DVector& left, const DVector& right)
    {
        return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
    }

    DVector Scale(const DVector& value, double scale)
    {
        return {value[0] * scale, value[1] * scale, value[2] * scale};
    }

    double Dot(const DVector& left, const DVector& right)
    {
        return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
    }

    DVector Cross(const DVector& left, const DVector& right)
    {
        return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
    }

    DVector Unit(const DVector& value)
    {
        return Scale(value, 1.0 / std::sqrt(Dot(value, value)));
    }

    bool StoreVector(const DVector& value, Engine::Vector3& output)
    {
        return StoreFloat(value[0], output.x) && StoreFloat(value[1], output.y) &&
            StoreFloat(value[2], output.z);
    }

    float Coordinate(float origin, float spacing, uint32_t index)
    {
        return static_cast<float>(static_cast<double>(origin) + static_cast<double>(spacing) * index);
    }

    DVector Position(const Engine::TerrainDesc& desc, std::span<const float> heights,
        uint32_t x, uint32_t z)
    {
        const float y = static_cast<float>(static_cast<double>(desc.origin.y) +
            static_cast<double>(desc.heightScale) * heights[static_cast<size_t>(z) * desc.width + x]);
        return {Coordinate(desc.origin.x, desc.cellSize, x), y,
            Coordinate(desc.origin.z, desc.cellSize, z)};
    }

    uint32_t Cell(float origin, float spacing, uint32_t count, double point)
    {
        uint32_t low = 0;
        uint32_t high = count - 1;
        while (low + 1 < high)
        {
            const uint32_t middle = low + (high - low) / 2;
            if (point < Coordinate(origin, spacing, middle))
            {
                high = middle;
            }
            else
            {
                low = middle;
            }
        }
        return std::min(low, count - 2);
    }

    bool InsideXZ(const DVector& point, const std::array<DVector, 3>& triangle)
    {
        const DVector ab = Subtract(triangle[1], triangle[0]);
        const DVector ac = Subtract(triangle[2], triangle[0]);
        const DVector ap = Subtract(point, triangle[0]);
        const double determinant = ab[0] * ac[2] - ab[2] * ac[0];
        const double u = (ap[0] * ac[2] - ap[2] * ac[0]) / determinant;
        const double v = (ab[0] * ap[2] - ab[2] * ap[0]) / determinant;
        return u >= -kRoundoff && v >= -kRoundoff && u + v <= 1.0 + kRoundoff;
    }

    std::optional<double> TriangleEntryXZ(const DVector& start, const DVector& movement,
        const std::array<DVector, 3>& triangle)
    {
        const DVector ab = Subtract(triangle[1], triangle[0]);
        const DVector ac = Subtract(triangle[2], triangle[0]);
        const DVector relative = Subtract(start, triangle[0]);
        const double determinant = ab[0] * ac[2] - ab[2] * ac[0];
        const double u = (relative[0] * ac[2] - relative[2] * ac[0]) / determinant;
        const double v = (ab[0] * relative[2] - ab[2] * relative[0]) / determinant;
        const double du = (movement[0] * ac[2] - movement[2] * ac[0]) / determinant;
        const double dv = (ab[0] * movement[2] - ab[2] * movement[0]) / determinant;
        double first = 0.0;
        double last = 1.0;
        const auto clip = [&](double initial, double rate)
        {
            if (rate == 0.0)
            {
                return initial >= 0.0;
            }
            const double crossing = -initial / rate;
            if (rate > 0.0)
            {
                first = std::max(first, crossing);
            }
            else
            {
                last = std::min(last, crossing);
            }
            return first <= last;
        };
        if (!clip(u, du) || !clip(v, dv) || !clip(1.0 - u - v, -du - dv))
        {
            return std::nullopt;
        }
        return first;
    }

    DVector ClosestPoint(const DVector& point, const std::array<DVector, 3>& triangle,
        const DVector& normal)
    {
        const double distance = Dot(Subtract(point, triangle[0]), normal);
        const DVector projected = Subtract(point, Scale(normal, distance));
        if (InsideXZ(projected, triangle))
        {
            return projected;
        }

        DVector closest = triangle[0];
        double closestSquared = std::numeric_limits<double>::infinity();
        for (size_t edgeIndex = 0; edgeIndex < triangle.size(); ++edgeIndex)
        {
            const DVector& a = triangle[edgeIndex];
            const DVector edge = Subtract(triangle[(edgeIndex + 1) % triangle.size()], a);
            const double fraction = std::clamp(Dot(Subtract(point, a), edge) / Dot(edge, edge), 0.0, 1.0);
            const DVector candidate = Add(a, Scale(edge, fraction));
            const DVector delta = Subtract(point, candidate);
            const double squared = Dot(delta, delta);
            if (squared < closestSquared)
            {
                closestSquared = squared;
                closest = candidate;
            }
        }
        return closest;
    }

    void RecordHit(double fraction, const DVector& contact, const DVector& start,
        const DVector& movement, const DVector& faceNormal, double& firstFraction,
        std::optional<Engine::SurfaceHit>& firstHit)
    {
        if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 || fraction >= firstFraction)
        {
            return;
        }
        const DVector center = Add(start, Scale(movement, fraction));
        const DVector delta = Subtract(center, contact);
        DVector normal = faceNormal;
        if (Dot(delta, delta) > 0.0)
        {
            normal = Unit(delta);
        }
        else if (Dot(faceNormal, movement) > 0.0)
        {
            normal = Scale(faceNormal, -1.0);
        }

        Engine::SurfaceHit hit;
        if (!StoreVector(contact, hit.position) || !StoreVector(normal, hit.normal))
        {
            return;
        }
        hit.fraction = static_cast<float>(fraction);
        firstFraction = fraction;
        firstHit = hit;
    }

    template <typename Accept>
    void DistanceRoots(const DVector& relative, const DVector& movement, double radiusSquared,
        Accept&& accept)
    {
        const double speedSquared = Dot(movement, movement);
        if (speedSquared == 0.0)
        {
            return;
        }
        const double middle = -Dot(relative, movement) / speedSquared;
        const DVector closest = Add(relative, Scale(movement, middle));
        const double distanceSquared = Dot(closest, closest);
        if (distanceSquared > radiusSquared * (1.0 + kRoundoff))
        {
            return;
        }
        const double half = std::sqrt(std::max(0.0, radiusSquared - distanceSquared) / speedSquared);
        accept(middle - half);
        if (half > 0.0)
        {
            accept(middle + half);
        }
    }

    void SweepTriangle(const std::array<DVector, 3>& triangle, const DVector& start,
        const DVector& movement, double radius, double& firstFraction,
        std::optional<Engine::SurfaceHit>& firstHit)
    {
        const DVector normal = Unit(Cross(Subtract(triangle[1], triangle[0]),
            Subtract(triangle[2], triangle[0])));
        const double radiusSquared = radius * radius;
        const DVector closest = ClosestPoint(start, triangle, normal);
        const DVector initialDelta = Subtract(start, closest);
        if (Dot(initialDelta, initialDelta) <= radiusSquared * (1.0 + kRoundoff))
        {
            RecordHit(0.0, closest, start, movement, normal, firstFraction, firstHit);
            return;
        }

        const double startDistance = Dot(Subtract(start, triangle[0]), normal);
        const double normalMovement = Dot(movement, normal);
        if (normalMovement != 0.0)
        {
            for (const double side : {-1.0, 1.0})
            {
                const double fraction = (side * radius - startDistance) / normalMovement;
                if (fraction < 0.0 || fraction > 1.0 || fraction >= firstFraction)
                {
                    continue;
                }
                const DVector center = Add(start, Scale(movement, fraction));
                const DVector contact = Subtract(center, Scale(normal, side * radius));
                if (InsideXZ(contact, triangle))
                {
                    RecordHit(fraction, contact, start, movement, normal, firstFraction, firstHit);
                }
            }
        }
        else if (std::abs(startDistance) <= radius)
        {
            // A coplanar radius-zero sweep still enters through a triangle boundary.
            const DVector projectedStart = Subtract(start, Scale(normal, startDistance));
            if (const auto entry = TriangleEntryXZ(projectedStart, movement, triangle))
            {
                const DVector contact = Add(projectedStart, Scale(movement, *entry));
                RecordHit(*entry, contact, start, movement, normal, firstFraction, firstHit);
            }
        }

        // Each edge cylinder is limited to its segment. Endpoint spheres complete the capsule.
        for (size_t edgeIndex = 0; edgeIndex < triangle.size(); ++edgeIndex)
        {
            const DVector& a = triangle[edgeIndex];
            const DVector edge = Subtract(triangle[(edgeIndex + 1) % triangle.size()], a);
            const double edgeSquared = Dot(edge, edge);
            const DVector relative = Subtract(start, a);
            const double initialAlong = Dot(relative, edge) / edgeSquared;
            const double movementAlong = Dot(movement, edge) / edgeSquared;
            const DVector perpendicular = Subtract(relative, Scale(edge, initialAlong));
            const DVector perpendicularMovement = Subtract(movement, Scale(edge, movementAlong));
            DistanceRoots(perpendicular, perpendicularMovement, radiusSquared, [&](double fraction)
            {
                const double along = initialAlong + movementAlong * fraction;
                if (along >= 0.0 && along <= 1.0)
                {
                    const DVector contact = Add(a, Scale(edge, along));
                    RecordHit(fraction, contact, start, movement, normal, firstFraction, firstHit);
                }
            });
            DistanceRoots(relative, movement, radiusSquared, [&](double fraction)
            {
                RecordHit(fraction, a, start, movement, normal, firstFraction, firstHit);
            });
        }
    }
}

namespace Engine
{
    bool Terrain::Initialize(const TerrainDesc& desc, std::span<const float> normalizedHeights,
        GraphicsDevice& graphics, std::string& error)
    {
        error.clear();
        const uint64_t vertexCount = static_cast<uint64_t>(desc.width) * desc.height;
        const uint64_t indexCount = desc.width >= 2 && desc.height >= 2
            ? static_cast<uint64_t>(desc.width - 1) * (desc.height - 1) * 6 : 0;
        if (desc.width < 2 || desc.height < 2 || desc.width > 1025 || desc.height > 1025 ||
            vertexCount > 1000000 || indexCount > 3000000 || normalizedHeights.size() != vertexCount ||
            !Finite(desc.origin) || !std::isnormal(desc.cellSize) || desc.cellSize <= 0.0f ||
            !std::isfinite(desc.heightScale) || desc.heightScale <= 0.0f ||
            !std::isfinite(desc.uvMeters) || desc.uvMeters <= 0.0f)
        {
            error = "Terrain requires bounded dimensions, matching samples, positive normal spacing and finite positive scales";
            return false;
        }
        for (const float sample : normalizedHeights)
        {
            if (!std::isfinite(sample) || sample < 0.0f || sample > 1.0f)
            {
                error = "Terrain height samples must be finite and normalized to [0,1]";
                return false;
            }
        }

        try
        {
            TerrainDesc preparedDesc = desc;
            std::vector<float> preparedHeights(normalizedHeights.begin(), normalizedHeights.end());
            std::vector<float> xs(desc.width);
            std::vector<float> zs(desc.height);
            for (uint32_t x = 0; x < desc.width; ++x)
            {
                if (!StoreFloat(static_cast<double>(desc.origin.x) + static_cast<double>(desc.cellSize) * x, xs[x]) ||
                    (xs[x] != 0.0f && !std::isnormal(xs[x])) ||
                    (x > 0 && xs[x] <= xs[x - 1]))
                {
                    error = "Terrain X coordinates overflow, collapse or cannot survive GPU float arithmetic";
                    return false;
                }
            }
            for (uint32_t z = 0; z < desc.height; ++z)
            {
                if (!StoreFloat(static_cast<double>(desc.origin.z) + static_cast<double>(desc.cellSize) * z, zs[z]) ||
                    (zs[z] != 0.0f && !std::isnormal(zs[z])) ||
                    (z > 0 && zs[z] <= zs[z - 1]))
                {
                    error = "Terrain Z coordinates overflow, collapse or cannot survive GPU float arithmetic";
                    return false;
                }
            }

            ModelData data;
            data.nodes.push_back(NodeData{"terrain"});
            data.materials.push_back(desc.material);
            data.meshes.emplace_back();
            MeshData& mesh = data.meshes.back();
            mesh.name = "terrain";
            mesh.vertices.resize(static_cast<size_t>(vertexCount));
            mesh.indices.reserve(static_cast<size_t>(indexCount));
            for (uint32_t z = 0; z < desc.height; ++z)
            {
                for (uint32_t x = 0; x < desc.width; ++x)
                {
                    const size_t index = static_cast<size_t>(z) * desc.width + x;
                    Vertex& vertex = mesh.vertices[index];
                    vertex.position.x = xs[x];
                    vertex.position.z = zs[z];
                    if (!StoreFloat(static_cast<double>(desc.origin.y) +
                        static_cast<double>(desc.heightScale) * preparedHeights[index], vertex.position.y) ||
                        !StoreFloat((static_cast<double>(xs[x]) - desc.origin.x) / desc.uvMeters, vertex.uv.x) ||
                        !StoreFloat((static_cast<double>(zs[z]) - desc.origin.z) / desc.uvMeters, vertex.uv.y))
                    {
                        error = "Terrain world height or repeated UV overflows float precision";
                        return false;
                    }
                }
            }
            for (uint32_t z = 0; z < desc.height; ++z)
            {
                for (uint32_t x = 0; x < desc.width; ++x)
                {
                    const uint32_t left = x == 0 ? x : x - 1;
                    const uint32_t right = x + 1 == desc.width ? x : x + 1;
                    const uint32_t back = z == 0 ? z : z - 1;
                    const uint32_t front = z + 1 == desc.height ? z : z + 1;
                    const double dx = (static_cast<double>(mesh.vertices[static_cast<size_t>(z) * desc.width + right].position.y) -
                        mesh.vertices[static_cast<size_t>(z) * desc.width + left].position.y) /
                        (static_cast<double>(xs[right]) - xs[left]);
                    const double dz = (static_cast<double>(mesh.vertices[static_cast<size_t>(front) * desc.width + x].position.y) -
                        mesh.vertices[static_cast<size_t>(back) * desc.width + x].position.y) /
                        (static_cast<double>(zs[front]) - zs[back]);
                    if (!StoreVector(Unit({-dx, 1.0, -dz}), mesh.vertices[static_cast<size_t>(z) * desc.width + x].normal))
                    {
                        error = "Terrain lighting normal is not finite";
                        return false;
                    }
                    if (x + 1 < desc.width && z + 1 < desc.height)
                    {
                        const uint32_t a = z * desc.width + x;
                        const uint32_t b = a + 1;
                        const uint32_t c = a + desc.width;
                        const uint32_t d = c + 1;
                        mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
                    }
                }
            }

            auto preparedMesh = std::make_shared<Model>();
            if (!preparedMesh->Upload(data, graphics, error))
            {
                return false;
            }
            static_assert(std::is_nothrow_swappable_v<TerrainDesc>);
            std::swap(_desc, preparedDesc);
            _heights.swap(preparedHeights);
            _mesh = std::move(preparedMesh);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Terrain initialization failed: ") + exception.what();
            return false;
        }
    }

    std::optional<SurfaceHit> Terrain::Sample(Vector2 xz) const
    {
        if (!_mesh || !std::isfinite(xz.x) || !std::isfinite(xz.y) ||
            xz.x < _desc.origin.x || xz.y < _desc.origin.z ||
            xz.x > Coordinate(_desc.origin.x, _desc.cellSize, _desc.width - 1) ||
            xz.y > Coordinate(_desc.origin.z, _desc.cellSize, _desc.height - 1))
        {
            return std::nullopt;
        }
        const uint32_t x = Cell(_desc.origin.x, _desc.cellSize, _desc.width, xz.x);
        const uint32_t z = Cell(_desc.origin.z, _desc.cellSize, _desc.height, xz.y);
        const DVector a = Position(_desc, _heights, x, z);
        const DVector b = Position(_desc, _heights, x + 1, z);
        const DVector c = Position(_desc, _heights, x, z + 1);
        const DVector d = Position(_desc, _heights, x + 1, z + 1);
        const double u = (static_cast<double>(xz.x) - a[0]) / (b[0] - a[0]);
        const double v = (static_cast<double>(xz.y) - a[2]) / (c[2] - a[2]);
        const bool firstTriangle = u + v <= 1.0;
        const double height = firstTriangle ? a[1] + (b[1] - a[1]) * u + (c[1] - a[1]) * v
            : d[1] + (c[1] - d[1]) * (1.0 - u) + (b[1] - d[1]) * (1.0 - v);
        const DVector normal = firstTriangle ? Unit(Cross(Subtract(c, a), Subtract(b, a)))
            : Unit(Cross(Subtract(c, b), Subtract(d, b)));
        SurfaceHit hit;
        if (!StoreVector({xz.x, height, xz.y}, hit.position) || !StoreVector(normal, hit.normal))
        {
            return std::nullopt;
        }
        return hit;
    }

    std::optional<SurfaceHit> Terrain::SweepSphere(const Segment& segment, float radius) const
    {
        if (!_mesh || !Finite(segment.start) || !Finite(segment.end) ||
            !std::isfinite(radius) || radius < 0.0f)
        {
            return std::nullopt;
        }
        const double maxX = Coordinate(_desc.origin.x, _desc.cellSize, _desc.width - 1);
        const double maxZ = Coordinate(_desc.origin.z, _desc.cellSize, _desc.height - 1);
        const double lowX = std::min(static_cast<double>(segment.start.x), static_cast<double>(segment.end.x)) - radius;
        const double highX = std::max(static_cast<double>(segment.start.x), static_cast<double>(segment.end.x)) + radius;
        const double lowZ = std::min(static_cast<double>(segment.start.z), static_cast<double>(segment.end.z)) - radius;
        const double highZ = std::max(static_cast<double>(segment.start.z), static_cast<double>(segment.end.z)) + radius;
        if (highX < _desc.origin.x || lowX > maxX || highZ < _desc.origin.z || lowZ > maxZ)
        {
            return std::nullopt;
        }
        uint32_t firstX = Cell(_desc.origin.x, _desc.cellSize, _desc.width,
            std::clamp(lowX, static_cast<double>(_desc.origin.x), maxX));
        uint32_t firstZ = Cell(_desc.origin.z, _desc.cellSize, _desc.height,
            std::clamp(lowZ, static_cast<double>(_desc.origin.z), maxZ));
        const uint32_t lastX = Cell(_desc.origin.x, _desc.cellSize, _desc.width,
            std::clamp(highX, static_cast<double>(_desc.origin.x), maxX));
        const uint32_t lastZ = Cell(_desc.origin.z, _desc.cellSize, _desc.height,
            std::clamp(highZ, static_cast<double>(_desc.origin.z), maxZ));
        // Include the cell before a lower boundary so contacts exactly on shared edges are retained.
        if (firstX > 0)
        {
            --firstX;
        }
        if (firstZ > 0)
        {
            --firstZ;
        }

        const DVector start = Double(segment.start);
        const DVector movement = Subtract(Double(segment.end), start);
        double firstFraction = std::numeric_limits<double>::infinity();
        std::optional<SurfaceHit> firstHit;
        for (uint32_t z = firstZ; z <= lastZ; ++z)
        {
            for (uint32_t x = firstX; x <= lastX; ++x)
            {
                const DVector a = Position(_desc, _heights, x, z);
                const DVector b = Position(_desc, _heights, x + 1, z);
                const DVector c = Position(_desc, _heights, x, z + 1);
                const DVector d = Position(_desc, _heights, x + 1, z + 1);
                SweepTriangle({a, c, b}, start, movement, radius, firstFraction, firstHit);
                SweepTriangle({b, c, d}, start, movement, radius, firstFraction, firstHit);
                if (firstFraction == 0.0)
                {
                    return firstHit;
                }
            }
        }
        return firstHit;
    }

    DrawItem Terrain::BuildDrawItem() const
    {
        DrawItem item;
        item.model = _mesh;
        return item;
    }

    TerrainDesc Terrain::Description() const
    {
        return _desc;
    }
}
