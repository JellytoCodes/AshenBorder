#include "Scene/Environment.h"

#include "Entity/Transform.h"
#include "Graphics/Model.h"
#include "Graphics/RenderFrame.h"
#include "Resource/ModelCache.h"
#include "Scene/Terrain.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace
{
bool Finite(Engine::Vector3 v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

namespace Client
{
bool Environment::TransformBounds(Engine::Vector3 minimum, Engine::Vector3 maximum, const Engine::Matrix& world,
                                  PropBounds& out) noexcept
{
    if (!Finite(minimum) || !Finite(maximum) || minimum.x > maximum.x || minimum.y > maximum.y ||
        minimum.z > maximum.z || world._14 != 0 || world._24 != 0 || world._34 != 0 || world._44 != 1)
    {
        return false;
    }
    for (const auto& row : world.m)
    {
        for (const float value : row)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
        }
    }
    PropBounds result{
        {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
        {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
         std::numeric_limits<float>::lowest()}};
    for (int corner = 0; corner < 8; ++corner)
    {
        Engine::Vector3 point;
        const auto value = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(corner & 1 ? maximum.x : minimum.x,
                                                                                 corner & 2 ? maximum.y : minimum.y,
                                                                                 corner & 4 ? maximum.z : minimum.z, 1),
                                                            DirectX::XMLoadFloat4x4(&world));
        DirectX::XMStoreFloat3(&point, value);
        if (!Finite(point))
        {
            return false;
        }
        result.minimum.x = std::min(result.minimum.x, point.x);
        result.minimum.y = std::min(result.minimum.y, point.y);
        result.minimum.z = std::min(result.minimum.z, point.z);
        result.maximum.x = std::max(result.maximum.x, point.x);
        result.maximum.y = std::max(result.maximum.y, point.y);
        result.maximum.z = std::max(result.maximum.z, point.z);
    }
    out = result;
    return true;
}

bool Environment::Intersects(Engine::Vector3 start, Engine::Vector3 end, const PropBounds& box, float padding) noexcept
{
    if (!Finite(start) || !Finite(end) || !Finite(box.minimum) || !Finite(box.maximum) || !std::isfinite(padding) ||
        padding < 0)
    {
        return false;
    }
    const std::array<double, 3> origin{start.x, start.y, start.z}, finish{end.x, end.y, end.z};
    const std::array<double, 3> lo{box.minimum.x, box.minimum.y, box.minimum.z},
        hi{box.maximum.x, box.maximum.y, box.maximum.z};
    double enter = 0, leave = 1;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        if (lo[axis] > hi[axis])
        {
            return false;
        }
        const double direction = finish[axis] - origin[axis];
        const double minimum = lo[axis] - padding, maximum = hi[axis] + padding;
        if (direction == 0)
        {
            if (origin[axis] < minimum || origin[axis] > maximum)
            {
                return false;
            }
            continue;
        }
        double first = (minimum - origin[axis]) / direction, last = (maximum - origin[axis]) / direction;
        if (first > last)
        {
            std::swap(first, last);
        }
        enter = std::max(enter, first);
        leave = std::min(leave, last);
        if (enter > leave)
        {
            return false;
        }
    }
    return true;
}

bool Environment::Initialize(const std::filesystem::path& resourceRoot, Engine::GraphicsDevice& graphics,
                             const Engine::Terrain& terrain, std::string& error)
{
    try
    {
        std::ifstream file(resourceRoot / L"Data" / L"presentation-scene.json", std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("Missing presentation-scene.json");
        }
        const auto config = nlohmann::json::parse(file);
        if (!config.at("schemaVersion").is_number_unsigned() || config.at("schemaVersion") != 1 ||
            !config.at("trees").is_array() || !config.at("models").is_object() ||
            (config.contains("props") && !config["props"].is_array()) ||
            (config.contains("colliders") && !config["colliders"].is_object()) ||
            (config.contains("blockingBoxes") &&
                (!config["blockingBoxes"].is_array() || config["blockingBoxes"].size() > 256)) ||
            config["trees"].size() + (config.contains("props") ? config["props"].size() : 0) > 128)
        {
            throw std::runtime_error("Invalid presentation environment");
        }
        std::map<std::string, std::vector<Engine::Cylinder>> localCylinders;
        std::map<std::string, std::vector<Engine::UprightBox>> localBoxes;
        std::size_t localShapeCount = 0;
        const auto number = [](const nlohmann::json& value)
        {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
            {
                throw std::runtime_error("Environment collider requires finite numeric values");
            }
            const float result = value.get<float>();
            if (!std::isfinite(result))
            {
                throw std::runtime_error("Environment collider value exceeds float range");
            }
            return result;
        };
        const auto vector3 = [&](const nlohmann::json& value)
        {
            if (!value.is_array() || value.size() != 3)
            {
                throw std::runtime_error("Environment collider base requires three coordinates");
            }
            return Engine::Vector3{number(value[0]), number(value[1]), number(value[2])};
        };
        const auto readBox = [&](const nlohmann::json& shape)
        {
            if (!shape.is_object())
            {
                throw std::runtime_error("Environment box requires an object");
            }
            const auto& halfExtents = shape.at("halfExtents");
            if (!halfExtents.is_array() || halfExtents.size() != 2)
            {
                throw std::runtime_error("Environment box requires two XZ half extents");
            }
            Engine::UprightBox box{vector3(shape.at("base")),
                {number(halfExtents[0]), number(halfExtents[1])}, number(shape.at("height")),
                shape.contains("yaw") ? number(shape.at("yaw")) : 0};
            if (!Engine::IsValid(box) || box.halfExtents.x <= 0 || box.halfExtents.y <= 0 || box.height <= 0)
            {
                throw std::runtime_error("Environment box requires positive extents and height");
            }
            return box;
        };
        if (config.contains("colliders"))
        {
            for (const auto& [role, shapes] : config["colliders"].items())
            {
                if (!config["models"].contains(role) || !shapes.is_array())
                {
                    throw std::runtime_error("Environment colliders require an existing model role and shape array");
                }
                for (const auto& shape : shapes)
                {
                    if (++localShapeCount > 256 || !shape.is_object() || !shape.at("shape").is_string())
                    {
                        throw std::runtime_error("Invalid environment collider or shape capacity exceeded");
                    }
                    const auto kind = shape.at("shape").get<std::string>();
                    if (kind == "cylinder")
                    {
                        Engine::Cylinder cylinder{vector3(shape.at("base")), number(shape.at("radius")),
                            number(shape.at("height"))};
                        if (!Engine::IsValid(cylinder) || cylinder.radius <= 0 || cylinder.height <= 0)
                        {
                            throw std::runtime_error("Environment cylinder requires positive radius and height");
                        }
                        localCylinders[role].push_back(cylinder);
                    }
                    else if (kind == "box")
                    {
                        localBoxes[role].push_back(readBox(shape));
                    }
                    else
                    {
                        throw std::runtime_error("Unknown environment collider shape");
                    }
                }
            }
        }
        std::map<std::string, std::shared_ptr<const Engine::Model>> models;
        const auto loadModel = [&](const std::string& role) -> std::shared_ptr<const Engine::Model>
        {
            const auto found = models.find(role);
            if (found != models.end())
            {
                return found->second;
            }
            const auto& value = config.at("models").at(role);
            if (!value.is_string() || value.get_ref<const std::string&>().empty())
            {
                throw std::runtime_error("Environment model role requires a relative model file");
            }
            const auto path = value.get<std::string>();
            Engine::ModelData data;
            if (!Engine::ReadModelCache(resourceRoot, std::filesystem::path(std::u8string(path.begin(), path.end())), data, error))
            {
                throw std::runtime_error(error);
            }
            if (std::any_of(data.meshes.begin(), data.meshes.end(), [](const auto& mesh) { return mesh.skin.has_value(); }))
            {
                throw std::runtime_error("Environment decorations require static meshes");
            }
            auto model = std::make_shared<Engine::Model>();
            if (!model->Upload(data, graphics, error))
            {
                throw std::runtime_error(error);
            }
            models.emplace(role, model);
            return model;
        };
        const auto tree = loadModel("tree");
        const auto place = [&](const std::shared_ptr<const Engine::Model>& model, float x, float z, float scale, float yaw)
        {
            const auto ground = terrain.Sample({x, z});
            Engine::Transform transform;
            if (!ground || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(yaw) ||
                !std::isfinite(scale) || scale <= 0 || scale > 4 || !transform.SetScale({scale, scale, scale}) ||
                !transform.SetPosition({x, ground->position.y - model->BoundsMin().y * scale, z}) ||
                !transform.SetRotation({0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}))
            {
                throw std::runtime_error("Invalid environment placement");
            }
            EnvironmentProp prop;
            prop.world = transform.WorldMatrix();
            if (!TransformBounds(model->BoundsMin(), model->BoundsMax(), prop.world, prop.bounds))
            {
                throw std::runtime_error("Invalid transformed environment bounds");
            }
            return prop;
        };
        std::vector<Engine::Cylinder> solidCylinders;
        std::vector<Engine::UprightBox> solidBoxes;
        const auto placeColliders = [&](const std::string& role, const EnvironmentProp& prop, float scale, float yaw)
        {
            const auto transformBase = [&](Engine::Vector3 base)
            {
                Engine::Vector3 result;
                DirectX::XMStoreFloat3(&result, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&base),
                    DirectX::XMLoadFloat4x4(&prop.world)));
                return result;
            };
            const auto cylinders = localCylinders.find(role);
            if (cylinders != localCylinders.end())
            {
                for (const auto& local : cylinders->second)
                {
                    Engine::Cylinder world{transformBase(local.base), local.radius * scale, local.height * scale};
                    if (!Engine::IsValid(world) || world.radius <= 0 || world.height <= 0 ||
                        solidCylinders.size() + solidBoxes.size() >= 256)
                    {
                        throw std::runtime_error("Invalid transformed environment cylinder or solid capacity exceeded");
                    }
                    solidCylinders.push_back(world);
                }
            }
            const auto boxes = localBoxes.find(role);
            if (boxes != localBoxes.end())
            {
                for (const auto& local : boxes->second)
                {
                    Engine::UprightBox world{transformBase(local.base),
                        {local.halfExtents.x * scale, local.halfExtents.y * scale}, local.height * scale, local.yaw + yaw};
                    if (!Engine::IsValid(world) || world.halfExtents.x <= 0 || world.halfExtents.y <= 0 || world.height <= 0 ||
                        solidCylinders.size() + solidBoxes.size() >= 256)
                    {
                        throw std::runtime_error("Invalid transformed environment box or solid capacity exceeded");
                    }
                    solidBoxes.push_back(world);
                }
            }
        };
        std::vector<EnvironmentProp> props;
        props.reserve(config["trees"].size());
        for (const auto& entry : config["trees"])
        {
            if (!entry.is_array() || entry.size() != 4 ||
                !std::all_of(entry.begin(), entry.end(),
                             [](const auto& v)
                             {
                                 return v.is_number() && std::isfinite(v.template get<double>());
                             }))
            {
                throw std::runtime_error("Tree requires x/z/scale/yaw");
            }
            const float x = entry[0].get<float>(), z = entry[1].get<float>(), scale = entry[2].get<float>(),
                        yaw = entry[3].get<float>();
            props.push_back(place(tree, x, z, scale, yaw));
            placeColliders("tree", props.back(), scale, yaw);
        }
        std::vector<EnvironmentDecoration> decorations;
        if (config.contains("props"))
        {
            decorations.reserve(config["props"].size());
            for (const auto& entry : config["props"])
            {
                if (!entry.is_object() || !entry.at("model").is_string() || !entry.at("occludes").is_boolean())
                {
                    throw std::runtime_error("Prop requires model role and boolean occludes");
                }
                for (const char* key : {"x", "z", "scale", "yaw"})
                {
                    if (!entry.at(key).is_number() || !std::isfinite(entry.at(key).get<double>()))
                    {
                        throw std::runtime_error("Prop requires finite x/z/scale/yaw");
                    }
                }
                EnvironmentDecoration decoration;
                decoration.role = entry.at("model").get<std::string>();
                decoration.model = loadModel(decoration.role);
                decoration.occludes = entry.at("occludes").get<bool>();
                decoration.placement = place(decoration.model, entry.at("x").get<float>(), entry.at("z").get<float>(),
                    entry.at("scale").get<float>(), entry.at("yaw").get<float>());
                placeColliders(decoration.role, decoration.placement, entry.at("scale").get<float>(), entry.at("yaw").get<float>());
                decorations.push_back(std::move(decoration));
            }
        }
        // World-space blockers have no model or fade; retain the existing placed-box prefix.
        if (config.contains("blockingBoxes"))
        {
            for (const auto& shape : config["blockingBoxes"])
            {
                if (solidCylinders.size() + solidBoxes.size() >= 256)
                {
                    throw std::runtime_error("Environment solid capacity exceeded");
                }
                solidBoxes.push_back(readBox(shape));
            }
        }
        _tree = tree;
        _props = std::move(props);
        _decorations = std::move(decorations);
        _solidCylinders = std::move(solidCylinders);
        _solidBoxes = std::move(solidBoxes);
        _runEpoch = 0;
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void Environment::Update(const Engine::CameraData& camera, const PlayerViewTarget& target, float dt, bool snap)
{
    if (!std::isfinite(dt) || dt < 0)
    {
        throw std::runtime_error("Environment requires finite nonnegative seconds");
    }
    if (_runEpoch != target.runEpoch)
    {
        _runEpoch = target.runEpoch;
        snap = true;
        for (auto& prop : _props)
        {
            prop.fadeCoverage = 1;
            prop.occluded = false;
        }
        for (auto& decoration : _decorations)
        {
            decoration.placement.fadeCoverage = 1;
            decoration.placement.occluded = false;
        }
    }
    const auto update = [&](EnvironmentProp& prop, bool enabled)
    {
        const float padding = prop.occluded ? 0.3f : 0.15f;
        prop.occluded = enabled && target.valid && (Intersects(camera.position, target.head, prop.bounds, padding) ||
                                         Intersects(camera.position, target.torso, prop.bounds, padding) ||
                                         Intersects(camera.position, target.pelvis, prop.bounds, padding));
        const float desired = prop.occluded ? 0.35f : 1;
        const float step = dt * 0.65f / (prop.occluded ? 0.15f : 0.25f);
        prop.fadeCoverage = snap ? desired
                                 : (prop.fadeCoverage < desired ? std::min(desired, prop.fadeCoverage + step)
                                                                : std::max(desired, prop.fadeCoverage - step));
    };
    for (auto& prop : _props)
    {
        update(prop, true);
    }
    for (auto& decoration : _decorations)
    {
        update(decoration.placement, decoration.occludes);
    }
}

void Environment::Append(Engine::RenderFrame& frame) const
{
    for (const auto& decoration : _decorations)
    {
        Engine::DrawItem item;
        item.model = decoration.model;
        item.world = decoration.placement.world;
        item.fadeCoverage = decoration.placement.fadeCoverage;
        frame.draws.push_back(std::move(item));
    }
    // Keep the tree suffix and Props() order used by fade diagnostics.
    for (const auto& prop : _props)
    {
        Engine::DrawItem item;
        item.model = _tree;
        item.world = prop.world;
        item.fadeCoverage = prop.fadeCoverage;
        frame.draws.push_back(std::move(item));
    }
}

void Environment::Clear() noexcept
{
    _props.clear();
    _decorations.clear();
    _solidCylinders.clear();
    _solidBoxes.clear();
    _tree.reset();
    _runEpoch = 0;
}

const std::vector<EnvironmentProp>& Environment::Props() const noexcept
{
    return _props;
}

const std::vector<EnvironmentDecoration>& Environment::Decorations() const noexcept
{
    return _decorations;
}

std::span<const Engine::Cylinder> Environment::SolidCylinders() const noexcept
{
    return _solidCylinders;
}

std::span<const Engine::UprightBox> Environment::SolidBoxes() const noexcept
{
    return _solidBoxes;
}
} // namespace Client
