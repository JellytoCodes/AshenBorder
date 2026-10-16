#pragma once

#include "Game/PoolEntities.h"
#include "Graphics/Camera.h"
#include "Types/Math.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Engine
{
class GraphicsDevice;
class Model;
class Terrain;
struct RenderFrame;
} // namespace Engine

namespace Client
{
struct PropBounds
{
    Engine::Vector3 minimum = {}, maximum = {};
};

struct EnvironmentProp
{
    Engine::Matrix world;
    PropBounds bounds;
    float fadeCoverage = 1;
    bool occluded = false;
};

struct EnvironmentDecoration
{
    std::string role;
    std::shared_ptr<const Engine::Model> model;
    EnvironmentProp placement;
    bool occludes = false;
};

// Environmental presentation is owned by the app, independently of the combat pools.
class Environment
{
  public:
    bool Initialize(const std::filesystem::path& resourceRoot, Engine::GraphicsDevice& graphics,
                    const Engine::Terrain& terrain, std::string& error);
    void Update(const Engine::CameraData& camera, const PlayerViewTarget& target, float dt, bool snap = false);
    void Append(Engine::RenderFrame& frame) const;
    void Clear() noexcept;
    const std::vector<EnvironmentProp>& Props() const noexcept;
    const std::vector<EnvironmentDecoration>& Decorations() const noexcept;
    static bool TransformBounds(Engine::Vector3 minimum, Engine::Vector3 maximum, const Engine::Matrix& world,
                                PropBounds& out) noexcept;
    static bool Intersects(Engine::Vector3 start, Engine::Vector3 end, const PropBounds& box,
                           float padding = 0) noexcept;

  private:
    std::shared_ptr<const Engine::Model> _tree;
    std::vector<EnvironmentProp> _props;
    std::vector<EnvironmentDecoration> _decorations;
    std::uint64_t _runEpoch = 0;
};
} // namespace Client
