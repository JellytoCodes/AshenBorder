#pragma once

#include "Graphics/ModelData.h"
#include "Graphics/RenderFrame.h"
#include "Scene/Geometry.h"
#include "Types/Math.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine
{
    class GraphicsDevice;

    struct TerrainDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        Vector3 origin = {0.0f, 0.0f, 0.0f};
        float cellSize = 1.0f;
        float heightScale = 1.0f;
        float uvMeters = 2.0f;
        MaterialData material{"terrain"};
    };

    struct SurfaceHit
    {
        // Sample: surface point. SweepSphere: triangle contact point, not sphere center.
        Vector3 position;
        Vector3 normal;
        float fraction = 0.0f;
    };

    class Terrain
    {
    public:
        bool Initialize(const TerrainDesc& desc, std::span<const float> normalizedHeights,
            GraphicsDevice& graphics, std::string& error);
        std::optional<SurfaceHit> Sample(Vector2 xz) const;
        std::optional<SurfaceHit> SweepSphere(const Segment& segment, float radius) const;
        DrawItem BuildDrawItem() const;
        // Borrowed until this Terrain is reinitialized or destroyed.
        const TerrainDesc& Description() const noexcept;

    private:
        TerrainDesc _desc;
        std::vector<float> _heights;
        std::shared_ptr<const Model> _mesh;
    };
}
