#include "Core/LastFieldApp.h"

#include "Audio/AudioClip.h"

#include "App/Application.h"
#include "Animation/AnimationClip.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "Graphics/Model.h"
#include "Graphics/RenderFrame.h"
#include "Resource/ModelCache.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t kSmokeFrameLimit = 90;
    constexpr float kCharacterHeight = 1.8f;
    constexpr float kCameraPitch = DirectX::XM_PI / 3.0f;
    constexpr float kCameraDistance = 18.0f;
    constexpr float kCameraFov = DirectX::XM_PI / 4.0f;
    constexpr float kFollowRate = 12.0f;

    const char* WaveLabel(const Client::WaveSnapshot& wave) noexcept
    {
        if (wave.outcome == Client::RunOutcome::Victory)
        {
            return "Victory";
        }
        if (wave.outcome == Client::RunOutcome::Defeat)
        {
            return "Defeat";
        }
        switch (wave.phase)
        {
        case Client::WavePhase::Ready: return "Ready";
        case Client::WavePhase::Combat: return "Combat";
        case Client::WavePhase::Settlement: return "Settlement";
        case Client::WavePhase::Rest: return "Rest";
        case Client::WavePhase::Complete: return "Complete";
        case Client::WavePhase::Selection: return "Selection";
        case Client::WavePhase::Shop: return "Shop";
        }
        return "Invalid";
    }

    std::filesystem::path Utf8Path(const std::string& value)
    {
        return std::filesystem::path(std::u8string(value.begin(), value.end()));
    }

    bool LoadTerrain(const nlohmann::json& config, const std::filesystem::path& resourceRoot,
        Engine::GraphicsDevice& graphics, std::unique_ptr<Engine::Terrain>& out,
        float& minimumNormalY, std::string& error)
    {
        try
        {
            const auto scalar = [&](const char* key)
            {
                const auto& value = config.at(key);
                if (!value.is_number())
                {
                    throw std::runtime_error(std::string("Terrain requires numeric ") + key);
                }
                const float result = value.get<float>();
                if (!std::isfinite(result))
                {
                    throw std::runtime_error(std::string("Terrain requires finite ") + key);
                }
                return result;
            };
            const auto dimension = [&](const char* key)
            {
                const auto& value = config.at(key);
                if (!value.is_number_unsigned() || value.get<uint64_t>() < 2 || value.get<uint64_t>() > 1025)
                {
                    throw std::runtime_error(std::string("Terrain dimension out of range: ") + key);
                }
                return value.get<uint32_t>();
            };
            Engine::TerrainDesc desc;
            desc.width = dimension("width");
            desc.height = dimension("height");
            desc.cellSize = scalar("cellSize");
            desc.heightScale = scalar("heightScale");
            desc.uvMeters = scalar("uvMeters");
            const auto& origin = config.at("origin");
            if (!origin.is_array() || origin.size() != 3 || desc.cellSize <= 0 ||
                desc.heightScale <= 0 || desc.uvMeters <= 0)
            {
                error = "Invalid terrain origin or spacing/scale";
                return false;
            }
            for (const auto& component : origin)
            {
                if (!component.is_number() || !std::isfinite(component.get<float>()))
                {
                    error = "Terrain origin requires three finite values";
                    return false;
                }
            }
            desc.origin = {origin[0].get<float>(), origin[1].get<float>(), origin[2].get<float>()};
            const float slopeDegrees = scalar("maxSlopeDegrees");
            if (slopeDegrees < 0 || slopeDegrees >= 90)
            {
                error = "Client maximum walking slope must be in [0,90) degrees";
                return false;
            }
            Engine::ModelData data;
            if (!Engine::ReadModelCache(resourceRoot, Utf8Path(config.at("modelPath").get<std::string>()), data, error))
            {
                return false;
            }
            const uint64_t vertexCount = static_cast<uint64_t>(desc.width) * desc.height;
            const uint64_t indexCount = static_cast<uint64_t>(desc.width - 1) * (desc.height - 1) * 6;
            const Engine::Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            if (data.nodes.size() != 1 || data.nodes[0].parent != -1 ||
                data.meshes.size() != 1 || data.materials.size() != 2 || vertexCount > 1000000 ||
                indexCount > 3000000 || data.meshes[0].vertices.size() != vertexCount ||
                data.meshes[0].indices.size() != indexCount || data.meshes[0].nodeIndex != 0 ||
                data.meshes[0].materialIndex != 0)
            {
                error = "Trial terrain cache does not match the selected grid descriptor";
                return false;
            }
            for (size_t row = 0; row < 4; ++row)
            {
                for (size_t column = 0; column < 4; ++column)
                {
                    if (data.nodes[0].bindLocal.m[row][column] != identity.m[row][column])
                    {
                        error = "Trial terrain requires an identity node transform";
                        return false;
                    }
                }
            }
            std::vector<float> heights;
            heights.reserve(static_cast<size_t>(vertexCount));
            const auto& mesh = data.meshes[0];
            for (uint32_t z = 0; z < desc.height; ++z)
            {
                for (uint32_t x = 0; x < desc.width; ++x)
                {
                    const auto& vertex = mesh.vertices[static_cast<size_t>(z) * desc.width + x];
                    const float expectedX = static_cast<float>(static_cast<double>(desc.origin.x) + static_cast<double>(x) * desc.cellSize);
                    const float expectedZ = static_cast<float>(static_cast<double>(desc.origin.z) + static_cast<double>(z) * desc.cellSize);
                    const float height = (vertex.position.y - desc.origin.y) / desc.heightScale;
                    if (std::abs(vertex.position.x - expectedX) > 0.00001f ||
                        std::abs(vertex.position.z - expectedZ) > 0.00001f ||
                        std::abs(vertex.uv.x - static_cast<float>((static_cast<double>(expectedX) - desc.origin.x) / desc.uvMeters)) > 0.0001f ||
                        std::abs(vertex.uv.y - static_cast<float>((static_cast<double>(expectedZ) - desc.origin.z) / desc.uvMeters)) > 0.0001f ||
                        !std::isfinite(height) || height < 0 || height > 1)
                    {
                        error = "Trial terrain cache vertex order, UV or height disagrees with its descriptor";
                        return false;
                    }
                    heights.push_back(height);
                }
            }
            size_t index = 0;
            for (uint32_t z = 0; z + 1 < desc.height; ++z)
            {
                for (uint32_t x = 0; x + 1 < desc.width; ++x)
                {
                    const uint32_t a = z * desc.width + x;
                    const uint32_t b = a + 1;
                    const uint32_t c = a + desc.width;
                    const uint32_t d = c + 1;
                    for (const uint32_t expected : {a, c, b, b, c, d})
                    {
                        if (mesh.indices[index++] != expected)
                        {
                            error = "Trial terrain cache does not use A-C-B/B-C-D triangles";
                            return false;
                        }
                    }
                }
            }
            desc.material = data.materials[0];
            desc.material.blend = Engine::MaterialBlendData{data.materials[1].baseColor,
                data.materials[1].textureFile, scalar("blendStartHeight"), scalar("blendEndHeight")};
            auto terrain = std::make_unique<Engine::Terrain>();
            if (!terrain->Initialize(desc, heights, graphics, error))
            {
                return false;
            }
            minimumNormalY = std::cos(slopeDegrees * DirectX::XM_PI / 180.0f);
            out = std::move(terrain);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Trial terrain configuration: ") + exception.what();
            return false;
        }
    }
}

namespace Client
{
    LastFieldApp::LastFieldApp(std::filesystem::path resourceRoot, bool smoke)
        : _resourceRoot(std::move(resourceRoot)), _smoke(smoke)
    {
    }

    bool LastFieldApp::Initialize(Engine::Application& app)
    {
        _application = &app;
        _wasInactive = GetForegroundWindow() != app.NativeWindow() || IsIconic(app.NativeWindow()) != FALSE;
        _paused = false;
        _wireframe = false;
        _showSurface = false;
        _moving = false;
        _showPools = false;
        _collisionTrial = false;
        _gameMode = false;
        _automaticAttack = true;
        ResetPresentation();
        _frameCount = 0;
        _diagnosticSeconds = 0;
        _diagnosticRunEpoch = 0;
        _diagnosticPhase = WavePhase::Ready;
        _diagnosticOutcome = RunOutcome::Playing;
        auto definitions = std::make_shared<GameDefinitions>();
        std::string definitionsError;
        if (!LoadGameDefinitions(_resourceRoot / L"Data" / L"trial-game.json", *definitions, definitionsError))
        {
            Engine::Log::Write("LastFieldApp game definitions: " + definitionsError);
            return false;
        }
        _definitions = std::move(definitions);
        std::ifstream configFile(_resourceRoot / L"Data" / L"trial-scene.json", std::ios::binary);
        if (!configFile)
        {
            Engine::Log::Write("LastFieldApp: cannot open Data/trial-scene.json");
            return false;
        }
        const auto config = nlohmann::json::parse(configFile, nullptr, false);
        if (!config.is_object() || !config.contains("modelPath") || !config["modelPath"].is_string() ||
            !config.contains("terrain") || !config["terrain"].is_object())
        {
            Engine::Log::Write("LastFieldApp: trial scene requires modelPath and terrain");
            return false;
        }
        Engine::ModelData data;
        std::string error;
        if (!Engine::ReadModelCache(_resourceRoot, Utf8Path(config["modelPath"].get<std::string>()), data, error))
        {
            Engine::Log::Write("LastFieldApp model read: " + error);
            return false;
        }
        auto character = std::make_shared<Engine::Model>();
        if (!character->Upload(data, app.Graphics(), error))
        {
            Engine::Log::Write("LastFieldApp model upload: " + error);
            return false;
        }
        if (!LoadTerrain(config["terrain"], _resourceRoot, app.Graphics(), _terrain, _minimumNormalY, error))
        {
            Engine::Log::Write("LastFieldApp terrain: " + error);
            return false;
        }
        const auto terrainDesc = _terrain->Description();
        const float terrainWidth = static_cast<float>(terrainDesc.width - 1) * terrainDesc.cellSize;
        const float terrainDepth = static_cast<float>(terrainDesc.height - 1) * terrainDesc.cellSize;
        // Legacy scenes keep their outer sixth; an explicit safe range can leave movement limits to solid scenery.
        Engine::BoundsXZ playBounds{{terrainDesc.origin.x + terrainWidth / 6, terrainDesc.origin.z + terrainDepth / 6},
                                    {terrainDesc.origin.x + terrainWidth * 5 / 6, terrainDesc.origin.z + terrainDepth * 5 / 6}};
        if (config.contains("playBounds"))
        {
            const auto& selected = config["playBounds"];
            const auto validPoint = [](const nlohmann::json& point)
            {
                return point.is_array() && point.size() == 2 &&
                    std::all_of(point.begin(), point.end(), [](const auto& value)
                    {
                        return value.is_number() && std::isfinite(value.template get<float>());
                    });
            };
            if (!selected.is_object() || !selected.contains("min") || !selected.contains("max") ||
                !validPoint(selected["min"]) || !validPoint(selected["max"]))
            {
                Engine::Log::Write("LastFieldApp: playBounds requires finite min/max XZ pairs");
                return false;
            }
            playBounds = {{selected["min"][0].get<float>(), selected["min"][1].get<float>()},
                          {selected["max"][0].get<float>(), selected["max"][1].get<float>()}};
            if (playBounds.min.x >= playBounds.max.x || playBounds.min.y >= playBounds.max.y ||
                playBounds.min.x < terrainDesc.origin.x || playBounds.min.y < terrainDesc.origin.z ||
                playBounds.max.x > terrainDesc.origin.x + terrainWidth ||
                playBounds.max.y > terrainDesc.origin.z + terrainDepth ||
                playBounds.min.x > 0 || playBounds.min.y > 0 || playBounds.max.x < 0 || playBounds.max.y < 0)
            {
                Engine::Log::Write("LastFieldApp: playBounds must be ordered, inside Terrain, and contain the starting origin");
                return false;
            }
        }
        _playBounds = playBounds;
        const float sourceHeight = character->BoundsMax().y - character->BoundsMin().y;
        const auto spawnSurface = _terrain->Sample({0, 0});
        if (!std::isfinite(sourceHeight) || sourceHeight <= 0.0f || !spawnSurface ||
            spawnSurface->normal.y < _minimumNormalY)
        {
            Engine::Log::Write("LastFieldApp: invalid character bounds or spawn surface");
            return false;
        }
        _visualScale = kCharacterHeight / sourceHeight;
        _footOffset = -character->BoundsMin().y * _visualScale;
        if (!_characterTransform.SetScale({_visualScale, _visualScale, _visualScale}) ||
            !_characterTransform.SetPosition({0, spawnSurface->position.y + _footOffset, 0}) ||
            !_characterTransform.SetRotation({0, 1, 0, 0}))
        {
            Engine::Log::Write("LastFieldApp: invalid trial transform");
            return false;
        }
        _character = std::move(character);
        if (config.contains("animationSlots"))
        {
            const auto& slots = config["animationSlots"];
            if (!slots.is_object() || !slots.contains("idle") || !slots["idle"].is_string() ||
                !slots.contains("move") || !slots["move"].is_string() ||
                !slots.contains("attack") || !slots["attack"].is_string() ||
                !slots.contains("death") || !slots["death"].is_string())
            {
                Engine::Log::Write("LastFieldApp: required idle/move/attack/death animation slots are invalid");
                return false;
            }
            const auto loadClip = [&](const char* role, std::shared_ptr<const Engine::AnimationClip>& out)
            {
                Engine::ClipData clipData;
                if (!Engine::ReadClipCache(_resourceRoot, Utf8Path(slots[role].get<std::string>()), clipData, error))
                {
                    Engine::Log::Write("LastFieldApp clip read: " + error);
                    return false;
                }
                auto clip = std::make_shared<Engine::AnimationClip>();
                if (!clip->Initialize(clipData, error) || clip->Data().skeletonSignature != _character->SkeletonSignature())
                {
                    Engine::Log::Write("LastFieldApp: incompatible required animation clip " + std::string(role) + ": " + error);
                    return false;
                }
                out = std::move(clip);
                return true;
            };
            if (!loadClip("idle", _idleClip) || !loadClip("move", _moveClip) ||
                !loadClip("attack", _attackClip) || !loadClip("death", _deathClip) ||
                !_animator.SetModel(_character, error) ||
                !_animator.SetClip(_attackClip, Engine::PlaybackMode::Clamp, error) ||
                !_animator.SetClip(_deathClip, Engine::PlaybackMode::Clamp, error) ||
                !_animator.SetClip(_moveClip, Engine::PlaybackMode::Loop, error) ||
                !_animator.SetClip(_idleClip, Engine::PlaybackMode::Loop, error))
            {
                Engine::Log::Write("LastFieldApp animation setup: " + error);
                return false;
            }
            Engine::Log::Write("LastFieldApp: required idle/move clips loaded; skeletal pose advances on fixed simulation ticks");
            Engine::Log::Write("LastFieldApp: attack/death roles validated; approved events drive clamped action poses");
        }
        else if (std::any_of(_character->Meshes().begin(), _character->Meshes().end(), [](const auto& mesh)
            {
                return mesh.skin.has_value();
            }))
        {
            Engine::Log::Write("LastFieldApp: a skinned trial character requires animationSlots");
            return false;
        }
        if (config.contains("weaponAttackSlots") || config.contains("weaponAttachments"))
        {
            constexpr std::array<const char*, 3> roles{"blade", "bow", "throwingKnife"};
            if (!config.contains("weaponAttackSlots") || !config.contains("weaponAttachments") ||
                !config["weaponAttackSlots"].is_object() || config["weaponAttackSlots"].size() != roles.size() ||
                !config["weaponAttachments"].is_object() || config["weaponAttachments"].size() != roles.size() ||
                !_idleClip)
            {
                Engine::Log::Write("LastFieldApp: weaponAttackSlots and weaponAttachments require all three roles together");
                return false;
            }
            Engine::Animator preview;
            if (!preview.SetModel(_character, error))
            {
                Engine::Log::Write("LastFieldApp weapon preview: " + error);
                return false;
            }
            const auto readAttachment = [&](const auto& record, WeaponAttachment& out)
            {
                if (!record.contains("nodeName") || !record["nodeName"].is_string())
                {
                    Engine::Log::Write("LastFieldApp: weapon attachment requires a nodeName");
                    return false;
                }
                const auto name = record["nodeName"].get<std::string>();
                const auto nodes = _character->Nodes();
                const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& value) { return value.name == name; });
                if (name.empty() || node == nodes.end() ||
                    std::count_if(nodes.begin(), nodes.end(), [&](const auto& value) { return value.name == name; }) != 1)
                {
                    Engine::Log::Write("LastFieldApp: weapon attachment node is missing or ambiguous");
                    return false;
                }
                const auto vector = [&](const char* key, bool positive)
                {
                    const auto& value = record.at(key);
                    if (!value.is_array() || value.size() != 3)
                    {
                        throw std::runtime_error("Weapon attachment requires three components: " + std::string(key));
                    }
                    std::array<float, 3> result{};
                    for (std::size_t component = 0; component < result.size(); ++component)
                    {
                        if (!value[component].is_number())
                        {
                            throw std::runtime_error("Weapon attachment component must be numeric");
                        }
                        result[component] = value[component].get<float>();
                        if (!std::isfinite(result[component]) || std::abs(result[component]) > 10 ||
                            (positive && result[component] <= 0))
                        {
                            throw std::runtime_error("Weapon attachment requires finite local values in [-10,10] and positive scale");
                        }
                    }
                    return Engine::Vector3{result[0], result[1], result[2]};
                };
                const auto position = vector("position", false);
                const auto rotation = vector("rotation", false);
                const auto scale = vector("scale", true);
                Engine::Quaternion quaternion;
                DirectX::XMStoreFloat4(&quaternion,
                    DirectX::XMQuaternionRotationRollPitchYaw(rotation.x, rotation.y, rotation.z));
                Engine::Transform local;
                if (!local.SetPosition(position) || !local.SetRotation(quaternion) || !local.SetScale(scale))
                {
                    Engine::Log::Write("LastFieldApp: invalid weapon attachment transform");
                    return false;
                }
                out = {static_cast<std::size_t>(node - nodes.begin()), local.WorldMatrix()};
                return true;
            };
            for (std::size_t index = 0; index < roles.size(); ++index)
            {
                const char* role = roles[index];
                const auto& clips = config.at("weaponAttackSlots");
                const auto& attachments = config.at("weaponAttachments");
                if (!clips.contains(role) || !clips[role].is_string() || !attachments.contains(role) ||
                    !attachments[role].is_object())
                {
                    Engine::Log::Write("LastFieldApp: invalid weapon action role " + std::string(role));
                    return false;
                }
                Engine::ClipData data;
                if (!Engine::ReadClipCache(_resourceRoot, Utf8Path(clips[role].get<std::string>()), data, error))
                {
                    Engine::Log::Write("LastFieldApp weapon clip read: " + error);
                    return false;
                }
                auto clip = std::make_shared<Engine::AnimationClip>();
                if (!clip->Initialize(data, error) || clip->Data().skeletonSignature != _character->SkeletonSignature() ||
                    !preview.SetClip(clip, Engine::PlaybackMode::Clamp, error))
                {
                    Engine::Log::Write("LastFieldApp incompatible weapon action: " + std::string(role) + ": " + error);
                    return false;
                }
                if (!readAttachment(attachments[role], _weaponAttachments[index]))
                {
                    return false;
                }
                if (index == static_cast<std::size_t>(WeaponKind::SingleProjectile))
                {
                    const auto& bow = attachments[role];
                    if (!bow.contains("stowed") || !bow["stowed"].is_object())
                    {
                        Engine::Log::Write("LastFieldApp: bow attachment requires a stowed transform");
                        return false;
                    }
                    if (!readAttachment(bow["stowed"], _stowedBowAttachment))
                    {
                        return false;
                    }
                }
                _weaponAttackClips[index] = std::move(clip);
            }
            _weaponAttachmentsEnabled = true;
            Engine::Log::Write("LastFieldApp: weapon mounts ready; blade=hand, bow=back/hand, throwingKnife=active pooled projectiles only");
        }
        _cameraFocus = {0, spawnSurface->position.y + kCharacterHeight * 0.55f, 0};
        if (!UpdateCamera(0))
        {
            return false;
        }
        _poolPose = _animator.Pose();
        _enemyAssets.clear();
        for (const auto& definition : _definitions->enemies)
        {
            _enemyAssets.emplace(definition.id,
                CharacterAsset{_character, _idleClip, _moveClip, sourceHeight, _attackClip, _deathClip});
        }
        if (config.contains("enemyVisuals"))
        {
            try
            {
                const auto& records = config.at("enemyVisuals");
                if (!records.is_array() || records.size() != _definitions->enemies.size())
                {
                    throw std::runtime_error("enemyVisuals must map every enemy definition exactly once");
                }
                std::map<std::uint32_t, CharacterAsset> candidate;
                for (const auto& record : records)
                {
                    if (!record.is_object() || !record.at("definitionId").is_number_unsigned())
                    {
                        throw std::runtime_error("enemyVisuals requires an unsigned definitionId");
                    }
                    const auto value = record.at("definitionId").get<std::uint64_t>();
                    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max() ||
                        !_definitions->FindEnemy(static_cast<std::uint32_t>(value)))
                    {
                        throw std::runtime_error("enemyVisuals references an unknown definition");
                    }
                    const auto id = static_cast<std::uint32_t>(value);
                    if (candidate.contains(id))
                    {
                        throw std::runtime_error("enemyVisuals duplicates a definition");
                    }
                    Engine::ModelData enemyData;
                    if (!Engine::ReadModelCache(_resourceRoot, Utf8Path(record.at("modelPath").get<std::string>()),
                        enemyData, error))
                    {
                        throw std::runtime_error("enemy model read: " + error);
                    }
                    auto model = std::make_shared<Engine::Model>();
                    if (!model->Upload(enemyData, app.Graphics(), error))
                    {
                        throw std::runtime_error("enemy model upload: " + error);
                    }
                    CharacterAsset asset;
                    asset.model = std::move(model);
                    asset.sourceHeight = asset.model->BoundsMax().y - asset.model->BoundsMin().y;
                    if (!std::isfinite(asset.sourceHeight) || asset.sourceHeight <= 0)
                    {
                        throw std::runtime_error("enemy model has invalid bounds");
                    }
                    const auto& slots = record.at("animationSlots");
                    const auto clip = [&](const char* role)
                    {
                        Engine::ClipData clipData;
                        if (!Engine::ReadClipCache(_resourceRoot, Utf8Path(slots.at(role).get<std::string>()),
                            clipData, error))
                        {
                            throw std::runtime_error("enemy clip read: " + error);
                        }
                        auto result = std::make_shared<Engine::AnimationClip>();
                        if (!result->Initialize(clipData, error) ||
                            result->Data().skeletonSignature != asset.model->SkeletonSignature())
                        {
                            throw std::runtime_error("enemy clip is incompatible: " + error);
                        }
                        return result;
                    };
                    asset.idle = clip("idle");
                    asset.move = clip("move");
                    asset.attack = clip("attack");
                    asset.death = clip("death");
                    Engine::Animator preview;
                    if (!preview.SetModel(asset.model, error) ||
                        !preview.SetClip(asset.attack, Engine::PlaybackMode::Clamp, error) ||
                        !preview.SetClip(asset.death, Engine::PlaybackMode::Clamp, error) ||
                        !preview.SetClip(asset.move, Engine::PlaybackMode::Loop, error) ||
                        !preview.SetClip(asset.idle, Engine::PlaybackMode::Loop, error))
                    {
                        throw std::runtime_error("enemy playback setup: " + error);
                    }
                    candidate.emplace(id, std::move(asset));
                }
                _enemyAssets.swap(candidate);
            }
            catch (const std::exception& exception)
            {
                Engine::Log::Write(std::string("LastFieldApp enemy visuals: ") + exception.what());
                return false;
            }
        }
        if (!LoadPresentation(error))
        {
            Engine::Log::Write("LastFieldApp presentation: " + error);
            return false;
        }
        if (!_combatEffects.Initialize(_definitions->projectileCapacity))
        {
            Engine::Log::Write("LastFieldApp: combat particle storage initialization failed");
            return false;
        }
        _enemyVisuals.clear();
        _deathVisuals.clear();
        _deathVisuals.resize(_definitions->enemyCapacity);
        _enemyVisuals.resize(_definitions->enemyCapacity);
        if (_idleClip)
        {
            for (auto& visual : _enemyVisuals)
            {
                if (!visual.animator.SetModel(_character, error) ||
                    !visual.animator.SetClip(_idleClip, Engine::PlaybackMode::Loop, error))
                {
                    Engine::Log::Write("LastFieldApp Enemy animation setup: " + error);
                    return false;
                }
            }
        }
        if (!RestartTrialRun())
        {
            return false;
        }
        if (_smoke)
        {
            _gameMode = true;
            _showPools = true;
            if (!RestartGameRun() || !_run->BeginWave())
            {
                return false;
            }
        }
        Engine::Log::Write("LastFieldApp: character/texture loaded; Terrain " + std::to_string(terrainDesc.width) + "x" +
                           std::to_string(terrainDesc.height) + " and two-material blend; camera pitch=60 distance=18 fov=45; WASD/P/F2/F3/F4/F5/F6/Escape");
        Engine::Log::Write("LastFieldApp: terrain span=" + std::to_string(terrainWidth) + "x" + std::to_string(terrainDepth) +
                           "m; run safety span=" + std::to_string(_playBounds.max.x - _playBounds.min.x) + "x" +
                           std::to_string(_playBounds.max.y - _playBounds.min.y) + "m");
        Engine::Log::Write("LastFieldApp: environment collision ready; trunk cylinders=" +
                           std::to_string(_environment.SolidCylinders().size()) + ", solid boxes=" +
                           std::to_string(_environment.SolidBoxes().size()) + "; canopy remains walkable");
        Engine::Log::Write("LastFieldApp: LF07 enemy/projectile/drop pools ready; storage/generation/RunEpoch validation; F4 trial display, F5 recycle, F6 new run");
        Engine::Log::Write("LastFieldApp: LF08 grounded movement/grid/nearest/melee/swept projectile ready; F7 collision trial, Space fire, Q melee");
        Engine::Log::Write("LastFieldApp: game definitions loaded; F8 game mode, Enter start/next wave, F9 automatic attacks, F6 new run");
        Engine::Log::Write("LastFieldApp: LF11 progression/shop ready; LF12 shadows/instances/tree fade ready; LF13 HUD/audio ready; LF14 complete-flow/restart path ready");
        return true;
    }

    bool LastFieldApp::RestartTrialRun()
    {
        if (!_terrain || _nextRunEpoch == 0 || _nextRunEpoch == std::numeric_limits<std::uint64_t>::max())
        {
            Engine::Log::Write("LastFieldApp: no Terrain or run epoch exhausted");
            return false;
        }
        auto next = std::make_unique<LastFieldRun>(_nextRunEpoch, *_terrain);
        RunConfig config;
        config.playBounds = _playBounds;
        config.maxSlopeDegrees = std::acos(std::clamp(_minimumNormalY, -1.0f, 1.0f)) * 180 / DirectX::XM_PI;
        _trialMelee.hitTargets.reserve(config.enemyCapacity);
        if (!next->Initialize(config) || !next->SetPlayerPosition(_characterTransform.Position()) ||
            (_showPools && !(_collisionTrial ? next->BuildCollisionTrial() : next->BuildTrialWave())))
        {
            Engine::Log::Write("LastFieldApp: trial run initialization failed");
            return false;
        }
        const auto facing = next->PlayerFacing();
        const float yaw = std::atan2(facing.x, facing.y);
        if (!_characterTransform.SetRotation({0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}))
        {
            return false;
        }
        std::string error;
        if (_idleClip && !_animator.SetClip(_idleClip, Engine::PlaybackMode::Loop, error))
        {
            Engine::Log::Write("LastFieldApp trial idle reset: " + error);
            return false;
        }
        _run = std::move(next);
        ResetPresentation();
        SnapCamera();
        ++_nextRunEpoch;
        return true;
    }

    bool LastFieldApp::RestartGameRun()
    {
        if (!_terrain || !_definitions || _nextRunEpoch == 0 ||
            _nextRunEpoch == std::numeric_limits<std::uint64_t>::max())
        {
            Engine::Log::Write("LastFieldApp: game run has no Terrain/definitions or the run epoch is exhausted");
            return false;
        }
        auto next = std::make_unique<LastFieldRun>(_nextRunEpoch, *_terrain);
        RunConfig config;
        config.playBounds = _playBounds;
        config.enemyCapacity = _definitions->enemyCapacity;
        config.staticCylinders = _environment.SolidCylinders();
        config.staticBoxes = _environment.SolidBoxes();
        config.projectileCapacity = _definitions->projectileCapacity;
        config.dropCapacity = _definitions->dropCapacity;
        config.maxSlopeDegrees = std::acos(std::clamp(_minimumNormalY, -1.0f, 1.0f)) * 180 / DirectX::XM_PI;
        if (!next->Initialize(config) || !next->ConfigureGame(_definitions, {}, 42))
        {
            Engine::Log::Write("LastFieldApp: game run initialization failed");
            return false;
        }
        std::string error;
        if (_idleClip && !_animator.SetClip(_idleClip, Engine::PlaybackMode::Loop, error))
        {
            Engine::Log::Write("LastFieldApp game idle reset: " + error);
            return false;
        }
        _run = std::move(next);
        _moving = false;
        _automaticAttack = true;
        _wireframe = false;
        _shadows = true;
        _instancing = true;
        _culling = true;
        _run->SetAutoAttackEnabled(_automaticAttack);
        ResetPresentation();
        SyncPlayerTransform();
        SnapCamera();
        ++_nextRunEpoch;
        Engine::Log::Write("LastFieldApp: LF09 combat weapons/damage/rewards ready");
        Engine::Log::Write("LastFieldApp: LF10 timed waves/spawning/settlement ready");
        return true;
    }

    void LastFieldApp::ResetPresentation() noexcept
    {
        _combatEffects.Reset(_run ? _run->RunEpoch() : 0);
        _pendingHudCommand.reset();
        _hudMessage.clear();
        _audio.StopAll();
        _nearest.reset();
        _lastHit.reset();
        _lastGameEvent.reset();
        _hitFlash = 0;
        _playerHitFlash = 0;
        _nextMarker = 0;
        _lastEventSequence = 0;
        _playerAction = PlayerAction::Locomotion;
        _attackYaw = 0;
        _activeAttackClip.reset();
        _activeWeaponSlot.reset();
        _nextDeathVisual = 0;
        for (auto& visual : _deathVisuals)
        {
            visual = DeathVisual{};
        }
        for (auto& marker : _eventMarkers)
        {
            marker = EventMarker{};
        }
        for (auto& visual : _enemyVisuals)
        {
            visual.ref.reset();
            visual.hitFlash = 0;
            visual.moving = false;
            visual.seen = false;
            visual.attacking = false;
        }
    }

    bool LastFieldApp::LoadPresentation(std::string& error)
    {
        try
        {
            std::ifstream file(_resourceRoot / L"Data" / L"presentation-scene.json", std::ios::binary);
            if (!file)
            {
                error = "Missing presentation-scene.json";
                return false;
            }
            const auto config = nlohmann::json::parse(file);
            const auto load = [&](const char* name, std::shared_ptr<const Engine::Model>& result)
            {
                Engine::ModelData data;
                if (!Engine::ReadModelCache(_resourceRoot, Utf8Path(config.at("models").at(name).get<std::string>()),
                                            data, error))
                {
                    return false;
                }
                auto model = std::make_shared<Engine::Model>();
                if (!model->Upload(data, _application->Graphics(), error))
                {
                    return false;
                }
                result = std::move(model);
                return true;
            };
            if (!load("blade", _weaponModels[0]) || !load("bow", _weaponModels[1]) ||
                !load("throwingKnife", _weaponModels[2]) || !load("arrow", _projectileModels[0]) ||
                !load("knifeProjectile", _projectileModels[1]) ||
                !load("experience", _experienceModel) || !load("currency", _currencyModel) ||
                !_environment.Initialize(_resourceRoot, _application->Graphics(), *_terrain, error) ||
                !_audio.Initialize(16))
            {
                return false;
            }
            Engine::ModelData warningData;
            warningData.nodes.push_back({"AttackWarning"});
            warningData.materials.push_back({"AttackWarning"});
            Engine::MeshData warningMesh;
            warningMesh.name = "GroundRibbon";
            warningMesh.vertices = {{{-.5f, 0, 0}, {0, 1, 0}, {0, 0}},
                {{-.5f, 0, 1}, {0, 1, 0}, {0, 1}}, {{.5f, 0, 0}, {0, 1, 0}, {1, 0}},
                {{.5f, 0, 1}, {0, 1, 0}, {1, 1}}};
            warningMesh.indices = {0, 1, 2, 2, 1, 3};
            warningData.meshes.push_back(std::move(warningMesh));
            auto warningModel = std::make_shared<Engine::Model>();
            if (!warningModel->Upload(warningData, _application->Graphics(), error))
            {
                return false;
            }
            _attackWarningModel = std::move(warningModel);
            const auto& audio = config.at("audio");
            if (!audio.is_object())
            {
                error = "Presentation audio requires a role-to-WAV object";
                return false;
            }
            const bool weaponAudio = audio.contains("blade") || audio.contains("bow") || audio.contains("throwingKnife");
            if (weaponAudio)
            {
                for (const char* role : {"blade", "bow", "throwingKnife"})
                {
                    if (!audio.contains(role) || !audio[role].is_string())
                    {
                        error = "Weapon audio requires blade, bow and throwingKnife together";
                        return false;
                    }
                }
            }
            for (const auto& [name, path] : audio.items())
            {
                auto clip = Engine::AudioClip::LoadWav(_resourceRoot / Utf8Path(path.get<std::string>()), error);
                if (!clip)
                {
                    return false;
                }
                _sounds.emplace(name, std::move(clip));
            }
            if (weaponAudio)
            {
                Engine::Log::Write("LastFieldApp: blade/bow/throwingKnife audio loaded; approved Fired selects each weapon cue");
            }
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    void LastFieldApp::SnapCamera()
    {
        const auto target = _run->ViewTarget();
        _cameraFocus = target.torso;
        if (!UpdateCamera(0))
        {
            throw std::runtime_error("Client restart camera reset failed");
        }
        _environment.Update(_camera.Snapshot(), target, 0, true);
    }

    HudSnapshot LastFieldApp::Hud() const
    {
        HudSnapshot snapshot;
        snapshot.gameMode = _gameMode;
        snapshot.showStart = !_gameMode && !_showPools && !_collisionTrial;
        snapshot.paused = _paused;
        snapshot.muted = _audio.IsMuted();
        snapshot.message = _hudMessage;
        snapshot.definitions = _definitions;
        if (!_run)
        {
            return snapshot;
        }
        snapshot.runEpoch = _run->RunEpoch();
        snapshot.player = _run->Player();
        snapshot.playerStats = _run->PlayerStats();
        snapshot.wave = _run->Wave();
        snapshot.progression = _run->Progression();
        for (const auto& slot : _run->WeaponSlots())
        {
            const auto* definition = _definitions->FindWeapon(slot.definitionId);
            if (definition)
            {
                snapshot.weapons.push_back(
                    {definition->kind, slot.cooldown,
                     std::max(_definitions->minimumCooldown, definition->cooldown / snapshot.playerStats.attackSpeed)});
            }
        }
        return snapshot;
    }

    void LastFieldApp::ExecuteHudCommand(const HudCommand& command)
    {
        if (!_run || command.runEpoch != _run->RunEpoch())
        {
            return;
        }
        ProgressionResult result;
        switch (command.kind)
        {
        case HudCommandKind::Start:
        case HudCommandKind::Restart:
            if (!_gameMode || command.kind == HudCommandKind::Restart)
            {
                _gameMode = true;
                _showPools = true;
                _collisionTrial = false;
                if (!RestartGameRun())
                {
                    throw std::runtime_error("Client UI start/restart failed");
                }
            }
            if (_run->Wave().phase == WavePhase::Rest && !_run->PrepareNextWave())
            {
                throw std::runtime_error("Client UI wave preparation failed");
            }
            if (_run->Wave().phase == WavePhase::Ready && !_run->BeginWave())
            {
                throw std::runtime_error("Client UI wave start failed");
            }
            CaptureGameEvents();
            return;
        case HudCommandKind::Choose:
            result = _run->ChooseUpgrade(command.runEpoch, command.choiceId, command.index);
            break;
        case HudCommandKind::Skip:
            result = _run->SkipUpgrade(command.runEpoch, command.choiceId);
            break;
        case HudCommandKind::Buy:
            result = _run->BuyOffer(command.runEpoch, command.revision, command.offerId);
            break;
        case HudCommandKind::Reroll:
            result = _run->RerollShop(command.runEpoch, command.revision);
            break;
        case HudCommandKind::NextWave:
            if (_run->Wave().phase == WavePhase::Shop)
            {
                result = _run->LeaveShop(command.runEpoch, command.revision);
                if (!result.accepted)
                {
                    break;
                }
            }
            if (_run->Wave().phase == WavePhase::Rest && !_run->PrepareNextWave())
            {
                throw std::runtime_error("Client UI next-wave preparation failed");
            }
            if (_run->Wave().phase == WavePhase::Ready && !_run->BeginWave())
            {
                throw std::runtime_error("Client UI next-wave start failed");
            }
            _hudMessage.clear();
            CaptureGameEvents();
            return;
        default:
            return;
        }
        if (result.accepted)
        {
            _hudMessage = command.kind == HudCommandKind::Buy ? "장비를 갖췄습니다" :
                command.kind == HudCommandKind::Reroll ? "물품 목록을 바꿨습니다" : "보급품을 챙겼습니다";
        }
        else
        {
            switch (result.reason)
            {
            case ProgressionFailure::InsufficientCurrency:
                _hudMessage = "보급품이 부족합니다";
                break;
            case ProgressionFailure::FullSlots:
                _hudMessage = "무기는 최대 6개까지 장비할 수 있습니다";
                break;
            case ProgressionFailure::MaxStacks:
                _hudMessage = "이미 최대로 보강한 장비입니다";
                break;
            case ProgressionFailure::AlreadyPurchased:
                _hudMessage = "이미 확보한 물품입니다";
                break;
            default:
                _hudMessage = "목록이 바뀌었습니다. 다시 선택하세요";
                break;
            }
        }
        CaptureGameEvents();
    }

    void LastFieldApp::SyncPlayerTransform()
    {
        auto position = _run->PlayerCollider().base;
        position.y += _footOffset;
        const auto facing = _run->PlayerFacing();
        const float yaw = _playerAction == PlayerAction::Attack ? _attackYaw : std::atan2(facing.x, facing.y);
        if (!_characterTransform.SetPosition(position) ||
            (_playerAction != PlayerAction::Death &&
             !_characterTransform.SetRotation({0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)})))
        {
            throw std::runtime_error("Client Run returned an invalid grounded player transform");
        }
    }

    void LastFieldApp::UpdatePlayerVisual(double simulationSeconds, double presentationSeconds, bool moved)
    {
        if (!_idleClip)
        {
            return;
        }
        std::string error;
        if (_playerAction == PlayerAction::Death)
        {
            // The Run is already terminal. Only the submitted death pose advances.
            if (!_animator.Advance(presentationSeconds, error))
            {
                throw std::runtime_error("Player death animation: " + error);
            }
            return;
        }
        if (_gameMode && simulationSeconds <= 0)
        {
            return;
        }
        if (_playerAction == PlayerAction::Attack && _activeAttackClip &&
            _animator.TimeSeconds() >= _activeAttackClip->Data().durationSeconds)
        {
            _playerAction = PlayerAction::Locomotion;
            _activeAttackClip.reset();
            _activeWeaponSlot.reset();
            if (!_animator.SetClip(moved ? _moveClip : _idleClip, Engine::PlaybackMode::Loop, error))
            {
                throw std::runtime_error("Player attack completion: " + error);
            }
            _moving = moved;
        }
        if (_playerAction == PlayerAction::Locomotion && moved != _moving)
        {
            if (!_animator.SetClip(moved ? _moveClip : _idleClip, Engine::PlaybackMode::Loop, error))
            {
                throw std::runtime_error("Player locomotion transition: " + error);
            }
            _moving = moved;
        }
        if (!_animator.Advance(simulationSeconds, error))
        {
            throw std::runtime_error("Player animation advance: " + error);
        }
    }

    void LastFieldApp::UpdateDeathVisuals(double seconds)
    {
        for (auto& visual : _deathVisuals)
        {
            if (!visual.active)
            {
                continue;
            }
            visual.elapsed += seconds;
            if (visual.elapsed >= visual.duration + .2)
            {
                visual = DeathVisual{};
                continue;
            }
            std::string error;
            if (!visual.animator.Advance(seconds, error))
            {
                throw std::runtime_error("Enemy death animation: " + error);
            }
        }
    }

    void LastFieldApp::BeginEnemyDeath(const GameEvent& event)
    {
        const auto asset = _enemyAssets.find(event.definitionId);
        const auto* definition = _definitions->FindEnemy(event.definitionId);
        if (asset == _enemyAssets.end() || !definition || !asset->second.death || _deathVisuals.empty())
        {
            return;
        }
        const auto playerPosition = _run->PlayerCollider().base;
        float yaw = std::atan2(playerPosition.x - event.position.x, playerPosition.z - event.position.z);
        if (event.target && event.target->handle && event.target->handle->index < _enemyVisuals.size())
        {
            const auto& previous = _enemyVisuals[event.target->handle->index];
            if (previous.ref == event.target)
            {
                yaw = previous.yaw;
            }
        }
        const float scale = definition->height / asset->second.sourceHeight;
        auto position = event.position;
        position.y -= asset->second.model->BoundsMin().y * scale;
        Engine::Transform transform;
        DeathVisual candidate;
        std::string error;
        if (!transform.SetScale({scale, scale, scale}) || !transform.SetPosition(position) ||
            !transform.SetRotation({0, std::sin(yaw * .5f), 0, std::cos(yaw * .5f)}) ||
            !candidate.animator.SetModel(asset->second.model, error) ||
            !candidate.animator.SetClip(asset->second.death, Engine::PlaybackMode::Clamp, error))
        {
            throw std::runtime_error("Enemy death snapshot: " + error);
        }
        candidate.model = asset->second.model;
        candidate.world = transform.WorldMatrix();
        candidate.duration = asset->second.death->Data().durationSeconds;
        candidate.active = true;
        auto slot = _nextDeathVisual;
        for (std::size_t i = 0; i < _deathVisuals.size(); ++i)
        {
            const auto index = (_nextDeathVisual + i) % _deathVisuals.size();
            if (!_deathVisuals[index].active)
            {
                slot = index;
                break;
            }
        }
        // Full visual storage replaces one old presentation, never a live pool slot.
        _deathVisuals[slot] = std::move(candidate);
        _nextDeathVisual = (slot + 1) % _deathVisuals.size();
    }

    void LastFieldApp::UpdateEnemyVisuals(double simulationSeconds)
    {
        for (auto& visual : _enemyVisuals)
        {
            visual.seen = false;
        }
        if (!_run->Enemies().ForEachActive([&](Engine::PoolHandle handle, const Enemy& enemy)
            {
                if (handle.index >= _enemyVisuals.size())
                {
                    throw std::runtime_error("Enemy animation slot is outside the fixed visual storage");
                }
                auto& visual = _enemyVisuals[handle.index];
                const EntityRef ref{EntityKind::Enemy, handle, _run->RunEpoch()};
                const bool fresh = !visual.ref || *visual.ref != ref || visual.definitionId != enemy.definitionId;
                const auto found = _enemyAssets.find(enemy.definitionId);
                if (found == _enemyAssets.end())
                {
                    throw std::runtime_error("Enemy has no configured visual asset");
                }
                const auto& asset = found->second;
                std::string error;
                if (fresh)
                {
                    visual.ref = ref;
                    visual.position = enemy.position;
                    visual.yaw = 0;
                    visual.hitFlash = 0;
                    visual.moving = false;
                    visual.definitionId = enemy.definitionId;
                    visual.attacking = false;
                    visual.charging = false;
                    if (asset.idle && (!visual.animator.SetModel(asset.model, error) ||
                        !visual.animator.SetClip(asset.idle, Engine::PlaybackMode::Loop, error)))
                    {
                        throw std::runtime_error("Enemy idle reset: " + error);
                    }
                    if (!asset.idle)
                    {
                        visual.animator.Reset();
                    }
                }
                const double x = static_cast<double>(enemy.position.x) - visual.position.x;
                const double z = static_cast<double>(enemy.position.z) - visual.position.z;
                const bool moved = x != 0 || z != 0;
                if (simulationSeconds > 0 && enemy.eligibleTick <= _run->TickIndex())
                {
                    if (enemy.chargePhase == EnemyChargePhase::Windup || enemy.chargePhase == EnemyChargePhase::Charging)
                    {
                        visual.yaw = static_cast<float>(std::atan2(enemy.chargeDirection.x, enemy.chargeDirection.y));
                    }
                    else if (enemy.rangedCharging)
                    {
                        const double aimX = static_cast<double>(enemy.rangedAim.x) - enemy.position.x;
                        const double aimZ = static_cast<double>(enemy.rangedAim.z) - enemy.position.z;
                        visual.yaw = static_cast<float>(std::atan2(aimX, aimZ));
                    }
                    else if (moved)
                    {
                        visual.yaw = static_cast<float>(std::atan2(x, z));
                    }
                    const bool windingUp = enemy.rangedCharging || enemy.chargePhase == EnemyChargePhase::Windup;
                    if (windingUp && !visual.charging && asset.attack)
                    {
                        if (!visual.animator.SetClip(asset.attack, Engine::PlaybackMode::Clamp, error))
                        {
                            throw std::runtime_error("Enemy windup transition: " + error);
                        }
                        visual.attacking = true;
                    }
                    visual.charging = windingUp;
                    if (asset.idle)
                    {
                        if (visual.attacking && !windingUp &&
                            visual.animator.TimeSeconds() >= asset.attack->Data().durationSeconds)
                        {
                            visual.attacking = false;
                            if (!visual.animator.SetClip(moved ? asset.move : asset.idle, Engine::PlaybackMode::Loop, error))
                            {
                                throw std::runtime_error("Enemy attack completion: " + error);
                            }
                            visual.moving = moved;
                        }
                        if (!visual.attacking && moved != visual.moving)
                        {
                            if (!visual.animator.SetClip(moved ? asset.move : asset.idle, Engine::PlaybackMode::Loop, error))
                            {
                                throw std::runtime_error("Enemy clip transition: " + error);
                            }
                            visual.moving = moved;
                        }
                        if (!visual.animator.Advance(simulationSeconds, error))
                        {
                            throw std::runtime_error("Enemy animation advance: " + error);
                        }
                    }
                }
                visual.position = enemy.position;
                visual.seen = true;
            }))
        {
            throw std::runtime_error("Enemy animation iteration failed");
        }
        for (auto& visual : _enemyVisuals)
        {
            if (!visual.seen)
            {
                // Retain only the handle value and last facing until a death event
                // is copied below. No returned pool object is dereferenced here.
                visual.hitFlash = 0;
            }
        }
    }

    void LastFieldApp::CaptureGameEvents()
    {
        for (const auto& event : _run->Events())
        {
            if (event.runEpoch != _run->RunEpoch() || event.sequence <= _lastEventSequence)
            {
                continue;
            }
            _lastEventSequence = event.sequence;
            _lastGameEvent = event;
            _combatEffects.OnEvent(event, *_run, *_definitions);
            std::string animationError;
            if (event.kind == GameEventKind::PlayerDied && _deathClip && _playerAction != PlayerAction::Death)
            {
                if (!_animator.SetClip(_deathClip, Engine::PlaybackMode::Clamp, animationError))
                {
                    throw std::runtime_error("Player death transition: " + animationError);
                }
                _playerAction = PlayerAction::Death;
                _activeAttackClip.reset();
                _activeWeaponSlot.reset();
            }
            else if (event.kind == GameEventKind::Fired && _attackClip &&
                !_run->Player().dead && _playerAction == PlayerAction::Locomotion)
            {
                const auto* definition = _definitions->FindWeapon(event.definitionId);
                if (!definition || event.slotIndex >= _run->WeaponSlots().size() ||
                    _run->WeaponSlots()[event.slotIndex].definitionId != event.definitionId)
                {
                    throw std::runtime_error("Approved attack visual references an invalid weapon definition/slot");
                }
                const auto kind = static_cast<std::size_t>(definition->kind);
                _activeAttackClip = _weaponAttackClips[kind] ? _weaponAttackClips[kind] : _attackClip;
                if (!_animator.SetClip(_activeAttackClip, Engine::PlaybackMode::Clamp, animationError))
                {
                    throw std::runtime_error("Player approved attack transition: " + animationError);
                }
                _playerAction = PlayerAction::Attack;
                _activeWeaponSlot = event.slotIndex;
                if (event.slotIndex < _run->WeaponSlots().size())
                {
                    const auto direction = _run->WeaponSlots()[event.slotIndex].direction;
                    _attackYaw = std::atan2(direction.x, direction.y);
                }
            }
            else if (event.kind == GameEventKind::EnemyAttacked && event.source.handle &&
                event.source.handle->index < _enemyVisuals.size())
            {
                auto& visual = _enemyVisuals[event.source.handle->index];
                const auto asset = _enemyAssets.find(event.definitionId);
                if (visual.ref == event.source && visual.seen && !visual.attacking &&
                    asset != _enemyAssets.end() && asset->second.attack)
                {
                    if (!visual.animator.SetClip(asset->second.attack, Engine::PlaybackMode::Clamp, animationError))
                    {
                        throw std::runtime_error("Enemy approved attack transition: " + animationError);
                    }
                    visual.attacking = true;
                }
            }
            else if (event.kind == GameEventKind::EnemyDied)
            {
                BeginEnemyDeath(event);
            }
            if ((event.kind == GameEventKind::WaveSettled || event.kind == GameEventKind::Victory) &&
                _playerAction == PlayerAction::Attack)
            {
                if (!_animator.SetClip(_idleClip, Engine::PlaybackMode::Loop, animationError))
                {
                    throw std::runtime_error("Player wave-end idle transition: " + animationError);
                }
                _playerAction = PlayerAction::Locomotion;
                _activeAttackClip.reset();
                _activeWeaponSlot.reset();
                _moving = false;
            }
            const char* sound = nullptr;
            switch (event.kind)
            {
            case GameEventKind::Fired:
            {
                sound = "fire";
                if (const auto* weapon = _definitions->FindWeapon(event.definitionId))
                {
                    switch (weapon->kind)
                    {
                    case WeaponKind::Melee: sound = "blade"; break;
                    case WeaponKind::SingleProjectile: sound = "bow"; break;
                    case WeaponKind::Shotgun: sound = "throwingKnife"; break;
                    }
                    if (!_sounds.contains(sound))
                    {
                        sound = "fire";
                    }
                }
                break;
            }
            case GameEventKind::EnemyAttacked:
                if (const auto* enemy = _definitions->FindEnemy(event.definitionId);
                    enemy && enemy->ranged && event.damage > 0)
                {
                    sound = "bow";
                }
                break;
            case GameEventKind::Damaged: sound = "hit"; break;
            case GameEventKind::EnemyDied: sound = "death"; break;
            case GameEventKind::PickedUp: sound = "pickup"; break;
            case GameEventKind::UpgradeChosen:
            case GameEventKind::Purchased:
            case GameEventKind::Rerolled:
            case GameEventKind::LevelQueued: sound = "select"; break;
            case GameEventKind::Victory: sound = "victory"; _audio.StopAll(); break;
            case GameEventKind::Defeat: sound = "defeat"; _audio.StopAll(); break;
            case GameEventKind::WaveStarted:
                if (const auto music = _sounds.find("music"); music != _sounds.end())
                {
                    _audio.PlayMusic(music->second, true, 0.6f);
                }
                break;
            default: break;
            }
            if (sound)
            {
                const auto clip = _sounds.find(sound);
                if (clip != _sounds.end())
                {
                    _audio.PlayEffect(clip->second, 0.65f);
                }
            }
            if (event.kind == GameEventKind::Damaged && event.target)
            {
                if (event.target->kind == EntityKind::Player)
                {
                    _playerHitFlash = 0.25f;
                }
                else if (event.target->kind == EntityKind::Enemy && event.target->handle &&
                    event.target->handle->index < _enemyVisuals.size())
                {
                    auto& visual = _enemyVisuals[event.target->handle->index];
                    if (visual.ref == event.target)
                    {
                        visual.hitFlash = 0.25f;
                    }
                }
            }
            if (event.kind == GameEventKind::PickedUp)
            {
                auto& marker = _eventMarkers[_nextMarker];
                marker.event = event;
                marker.remaining = 0.8f;
                _nextMarker = (_nextMarker + 1) % _eventMarkers.size();
            }
        }
    }

    void LastFieldApp::FixedUpdate(const Engine::InputState& input, float dt)
    {
        if (!std::isfinite(dt) || dt < 0)
        {
            throw std::runtime_error("LastFieldApp fixed update requires finite nonnegative seconds");
        }
        if (_paused)
        {
            return;
        }
        if (_pendingHudCommand)
        {
            const auto command = *_pendingHudCommand;
            _pendingHudCommand.reset();
            ExecuteHudCommand(command);
        }
        _hitFlash = std::max(0.0f, _hitFlash - dt);
        _playerHitFlash = std::max(0.0f, _playerHitFlash - dt);
        for (auto& marker : _eventMarkers)
        {
            marker.remaining = std::max(0.0f, marker.remaining - dt);
        }
        for (auto& visual : _enemyVisuals)
        {
            visual.hitFlash = std::max(0.0f, visual.hitFlash - dt);
        }
        if (!_gameMode && !_showPools && !_collisionTrial && input.IsPressed(Engine::Key::Enter))
        {
            ExecuteHudCommand({HudCommandKind::Start, _run->RunEpoch()});
        }
        else if (_gameMode && input.IsPressed(Engine::Key::Enter))
        {
            const auto phase = _run->Wave().phase;
            if (phase == WavePhase::Ready || phase == WavePhase::Rest)
            {
                if ((phase == WavePhase::Rest && !_run->PrepareNextWave()) || !_run->BeginWave())
                {
                    throw std::runtime_error("Client game wave start failed");
                }
                CaptureGameEvents();
            }
        }
        const float x = static_cast<float>(input.IsHeld(Engine::Key::D)) - static_cast<float>(input.IsHeld(Engine::Key::A));
        const float z = static_cast<float>(input.IsHeld(Engine::Key::W)) - static_cast<float>(input.IsHeld(Engine::Key::S));
        const auto previous = _characterTransform.Position();
        const double beforeElapsed = _gameMode ? _run->Wave().elapsed : 0;
        _run->Tick(dt, {x, z});
        if (_gameMode && !_combatEffects.Advance(dt, *_run))
        {
            throw std::runtime_error("Client particle fixed update failed");
        }
        const double animationSeconds = _gameMode
            ? std::clamp(_run->Wave().elapsed - beforeElapsed, 0.0, static_cast<double>(dt)) : dt;
        SyncPlayerTransform();
        const auto position = _characterTransform.Position();
        const bool moved = position.x != previous.x || position.z != previous.z;
        UpdatePlayerVisual(animationSeconds, dt, moved);
        if (_gameMode)
        {
            UpdateEnemyVisuals(animationSeconds);
            UpdateDeathVisuals(dt);
            _nearest.reset();
            for (const auto& slot : _run->WeaponSlots())
            {
                if (slot.target && _run->IsValid(*slot.target))
                {
                    _nearest = slot.target;
                    break;
                }
            }
            CaptureGameEvents();
            SyncPlayerTransform();
        }
        else if (_collisionTrial)
        {
            if (input.IsPressed(Engine::Key::Space))
            {
                _run->FireTrialProjectile();
            }
            if (input.IsPressed(Engine::Key::Q) && _run->BeginMelee(_trialMelee, {EntityKind::Player, {}, _run->RunEpoch()}))
            {
                const auto body = _run->PlayerCollider();
                if (!_run->TestMelee(_trialMelee, body.base, _run->PlayerFacing(), 3, DirectX::XM_PI / 3, body.height))
                {
                    throw std::runtime_error("Client melee query failed");
                }
            }
            _nearest = _run->FindNearest(_run->PlayerCollider().base, 12, 1.8f);
            for (const auto& hit : _run->Hits())
            {
                _lastHit = hit;
                _hitFlash = 0.35f;
            }
        }
        _run->ClearHits();
        if (_gameMode || _showSurface || _showPools || _collisionTrial)
        {
            // Count actual fixed simulation progress, not idle render callbacks or paused wall time.
            _diagnosticSeconds += animationSeconds;
        }
    }

    bool LastFieldApp::UpdateCamera(float dt)
    {
        const auto position = _characterTransform.Position();
        const Engine::Vector3 target{position.x, position.y - _footOffset + kCharacterHeight * 0.55f, position.z};
        const float blend = 1.0f - std::exp(-kFollowRate * dt);
        _cameraFocus.x += (target.x - _cameraFocus.x) * blend;
        _cameraFocus.y += (target.y - _cameraFocus.y) * blend;
        _cameraFocus.z += (target.z - _cameraFocus.z) * blend;
        const Engine::Vector3 eye{
            _cameraFocus.x, _cameraFocus.y + std::sin(kCameraPitch) * kCameraDistance,
            _cameraFocus.z - std::cos(kCameraPitch) * kCameraDistance};
        const float aspect = _application->Graphics().ViewportAspect();
        if (!_camera.SetView(eye, _cameraFocus) ||
            (aspect > 0 && !_camera.SetPerspective(kCameraFov, aspect, 0.1f, 100.0f)))
        {
            Engine::Log::Write("LastFieldApp: invalid trial camera parameters");
            return false;
        }
        return true;
    }

    std::string LastFieldApp::DiagnosticText() const
    {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << "SAVE MYSELF: ASHEN BORDER";
        if (_gameMode && _run)
        {
            const auto& wave = _run->Wave();
            const auto& rewards = _run->Rewards();
            text << " | Run=" << _run->RunEpoch()
                << " | Wave=" << wave.waveIndex + 1 << "/" << wave.waveCount << " " << WaveLabel(wave)
                << std::fixed << std::setprecision(1) << " | HP=" << _run->Player().hp << "/" << _run->PlayerStats().maxHp
                << " | Time=" << wave.remaining << " | XP=" << rewards.acquiredXp
                << " Currency=" << rewards.acquiredCurrency << " Kills=" << rewards.kills
                << " | Auto=" << (_automaticAttack ? "ON" : "OFF");
            const EventMarker* pickup = nullptr;
            for (const auto& marker : _eventMarkers)
            {
                if (marker.remaining > 0 && marker.event.kind == GameEventKind::PickedUp &&
                    (!pickup || marker.event.sequence > pickup->event.sequence))
                {
                    pickup = &marker;
                }
            }
            if (pickup)
            {
                text << " | +XP=" << pickup->event.xp << " +Currency=" << pickup->event.currency;
            }
        }
        if (_showSurface && _terrain)
        {
            const auto position = _characterTransform.Position();
            const auto surface = _terrain->Sample({position.x, position.z});
            if (!surface)
            {
                throw std::runtime_error("Client surface diagnostic query failed");
            }
            const float slope = std::acos(std::clamp(surface->normal.y, -1.0f, 1.0f)) * 180.0f / DirectX::XM_PI;
            text << std::fixed << std::setprecision(3) << " | footY=" << position.y - _footOffset
                << "m | surfaceY=" << surface->position.y << "m | slope=" << slope << "deg";
        }
        if (_showPools && _run)
        {
            const auto enemies = _run->Enemies().GetStats();
            const auto projectiles = _run->Projectiles().GetStats();
            const auto drops = _run->Drops().GetStats();
            if (!_gameMode)
            {
                text << " | run=" << _run->RunEpoch();
            }
            text << " | E=" << enemies.active << "/" << enemies.capacity
                << " P=" << projectiles.active << "/" << projectiles.capacity
                << " D=" << drops.active << "/" << drops.capacity;
            if (!_gameMode)
            {
                text << " | peak=" << enemies.highWater << "/" << projectiles.highWater << "/" << drops.highWater
                    << " | fail=" << enemies.reserveFailures << "/" << projectiles.reserveFailures << "/"
                    << drops.reserveFailures << " | merge=" << _run->DropMerges() << " ledger="
                    << _run->Ledger(DropKind::Experience);
            }
        }
        if (_collisionTrial)
        {
            text << " | LF08 target=" << (_nearest ? std::to_string(_nearest->handle->index) : "none")
                << " hit=" << (_lastHit ? std::to_string(static_cast<int>(_lastHit->kind)) : "none");
        }
        return text.str();
    }

    Engine::AppAction LastFieldApp::Update(const Engine::InputState& input, float dt)
    {
        ++_frameCount;
        _audio.Update();
        const bool wasPaused = _paused;
        if (input.IsPressed(Engine::Key::Escape))
        {
            Engine::Log::Write("LastFieldApp: exit requested by Escape");
            return Engine::AppAction::Exit;
        }
        if (_smoke && _frameCount >= kSmokeFrameLimit)
        {
            Engine::Log::Write("LastFieldApp: smoke complete, frames=90");
            return Engine::AppAction::Exit;
        }
        if (!UpdateCamera(dt))
        {
            return Engine::AppAction::Exit;
        }
        if (input.IsPressed(Engine::Key::F2))
        {
            _wireframe = !_wireframe;
            Engine::Log::Write(_wireframe ? "LastFieldApp: Terrain wireframe on" : "LastFieldApp: Terrain wireframe off");
        }
        const bool gameToggle = input.IsPressed(Engine::Key::F8);
        const bool poolToggle = input.IsPressed(Engine::Key::F4);
        const bool collisionToggle = input.IsPressed(Engine::Key::F7);
        const bool restart = input.IsPressed(Engine::Key::F6);
        const bool recycle = input.IsPressed(Engine::Key::F5);
        if (gameToggle)
        {
            _gameMode = !_gameMode;
            _collisionTrial = false;
            _showPools = _gameMode;
            if (!(_gameMode ? RestartGameRun() : RestartTrialRun()))
            {
                throw std::runtime_error("Client game mode transition failed");
            }
            Engine::Log::Write(_gameMode ? "LastFieldApp: F8 game Ready; Enter starts the wave"
                                         : "LastFieldApp: F8 returns to the legacy scene");
        }
        else if (poolToggle)
        {
            const bool wasGame = _gameMode;
            _gameMode = false;
            _collisionTrial = false;
            _showPools = wasGame || !_showPools;
            ResetPresentation();
            if (!(wasGame ? RestartTrialRun() : (_showPools ? _run->BuildTrialWave() : _run->Clear())))
            {
                throw std::runtime_error("Client pool display transition failed");
            }
        }
        else if (collisionToggle)
        {
            const bool wasGame = _gameMode;
            _gameMode = false;
            _collisionTrial = wasGame || !_collisionTrial;
            _showPools = _collisionTrial;
            ResetPresentation();
            if (!(wasGame ? RestartTrialRun() : (_collisionTrial ? _run->BuildCollisionTrial() : _run->Clear())))
            {
                throw std::runtime_error("Client collision trial setup failed");
            }
        }
        else if (restart)
        {
            if (!(_gameMode ? RestartGameRun() : RestartTrialRun()))
            {
                throw std::runtime_error("Client run restart failed");
            }
        }
        else if (recycle)
        {
            if (_gameMode)
            {
                Engine::Log::Write("LastFieldApp: F5 ignored in game mode; F6 starts a new run");
            }
            else
            {
                ResetPresentation();
                if (_showPools && !(_collisionTrial ? _run->BuildCollisionTrial() : _run->BuildTrialWave()))
                {
                    throw std::runtime_error("Client trial pool recycle failed");
                }
            }
        }
        if (gameToggle || poolToggle || collisionToggle || restart || (recycle && !_gameMode))
        {
            SyncPlayerTransform();
        }
        if (_gameMode && input.IsPressed(Engine::Key::F9))
        {
            _automaticAttack = !_automaticAttack;
            _run->SetAutoAttackEnabled(_automaticAttack);
        }
        if (input.IsPressed(Engine::Key::F10))
        {
            _shadows = !_shadows;
        }
        if (input.IsPressed(Engine::Key::F11))
        {
            _instancing = !_instancing;
        }
        if (input.IsPressed(Engine::Key::F12))
        {
            _culling = !_culling;
        }
        const bool resumeAfterReset = wasPaused && (restart || gameToggle);
        if (resumeAfterReset)
        {
            _paused = false;
        }
        const auto hud = Hud();
        auto hudCommand =
            ReadHudCommand(hud, input, _application->Graphics().Width(), _application->Graphics().Height());
        if (input.IsPressed(Engine::Key::M) || hudCommand.kind == HudCommandKind::ToggleMute)
        {
            if (!_audio.SetMuted(!_audio.IsMuted()))
            {
                Engine::Log::Write("LastFieldApp: audio mute failed; simulation retained");
            }
            hudCommand.kind = HudCommandKind::None;
        }
        const bool pauseToggle = !hud.showStart &&
            (input.IsPressed(Engine::Key::P) || hudCommand.kind == HudCommandKind::TogglePause);
        if (hudCommand.kind != HudCommandKind::None && hudCommand.kind != HudCommandKind::TogglePause &&
            !_pendingHudCommand)
        {
            if (_paused && hudCommand.kind == HudCommandKind::Restart)
            {
                ExecuteHudCommand(hudCommand);
                _paused = false;
            }
            else if (!_paused)
            {
                _pendingHudCommand = hudCommand;
            }
        }
        const bool inactive =
            IsIconic(_application->NativeWindow()) != FALSE || GetForegroundWindow() != _application->NativeWindow();
        if (inactive && !_wasInactive)
        {
            _pendingHudCommand.reset();
        }
        _wasInactive = inactive;
        if (!_audio.SetPaused(_paused || inactive))
        {
            Engine::Log::Write("LastFieldApp: audio pause failed; simulation retained");
        }
        if (_run)
        {
            auto target = _run->ViewTarget();
            target.valid = _gameMode && (target.valid || _playerAction == PlayerAction::Death);
            _environment.Update(_camera.Snapshot(), target, dt);
        }
        const bool surfaceToggle = input.IsPressed(Engine::Key::F3);
        if (surfaceToggle)
        {
            _showSurface = !_showSurface;
        }
        if (_showSurface)
        {
            const auto position = _characterTransform.Position();
            if (!_terrain->Sample({position.x, position.z}))
            {
                Engine::Log::Write("LastFieldApp: surface diagnostic query failed");
                return Engine::AppAction::Exit;
            }
        }
        auto action = Engine::AppAction::Continue;
        if (pauseToggle)
        {
            _paused = !_paused;
            _pendingHudCommand.reset();
            _audio.SetPaused(_paused || inactive);
            Engine::Log::Write(_paused ? "LastFieldApp: pause simulation" : "LastFieldApp: resume simulation");
            action = _paused ? Engine::AppAction::PauseSimulation : Engine::AppAction::ResumeSimulation;
        }
        else if (resumeAfterReset || (wasPaused && !_paused))
        {
            action = Engine::AppAction::ResumeSimulation;
        }
        const auto& wave = _run->Wave();
        const bool changed = _diagnosticRunEpoch != _run->RunEpoch() ||
            _diagnosticPhase != wave.phase || _diagnosticOutcome != wave.outcome;
        const bool forced = changed || surfaceToggle || poolToggle || collisionToggle || gameToggle || restart ||
            recycle || input.IsPressed(Engine::Key::F9) || pauseToggle || wasPaused != _paused;
        if (forced)
        {
            _diagnosticSeconds = 0;
        }
        const bool progressing = !_paused && (_gameMode ?
            wave.phase == WavePhase::Combat && wave.outcome == RunOutcome::Playing :
            _showSurface || _showPools || _collisionTrial);
        const bool due = progressing && std::isfinite(dt) && dt > 0 && _diagnosticSeconds >= 1.0;
        if (forced || due)
        {
            if (due)
            {
                _diagnosticSeconds = std::fmod(_diagnosticSeconds, 1.0);
            }
            Engine::Log::Write(std::string("LastFieldApp diagnostics: paused=") + (_paused ? "1 " : "0 ") + DiagnosticText());
        }
        _diagnosticRunEpoch = _run->RunEpoch();
        _diagnosticPhase = wave.phase;
        _diagnosticOutcome = wave.outcome;
        return action;
    }

    void LastFieldApp::AppendAttackWarning(Engine::RenderFrame& frame, const Enemy& enemy,
        const EnemyDefinition& definition) const
    {
        if (!_attackWarningModel || !_terrain)
        {
            return;
        }
        const auto ribbon = [&](Engine::Segment motion, float radius, float height, bool projectile)
        {
            const double dx = static_cast<double>(motion.end.x) - motion.start.x;
            const double dz = static_cast<double>(motion.end.z) - motion.start.z;
            const double length = std::hypot(dx, dz);
            if (!std::isfinite(length) || length <= .001)
            {
                return;
            }
            float fraction = static_cast<float>(std::min(1.0, 20.0 / length));
            auto baseMotion = motion;
            if (projectile)
            {
                baseMotion.start.y -= radius;
                baseMotion.end.y -= radius;
                if (const auto hit = _terrain->SweepSphere(motion, radius))
                {
                    fraction = std::min(fraction, hit->fraction);
                }
            }
            const auto clip = [&](const auto& obstacle)
            {
                if (const auto hit = Engine::SweepCylinder(baseMotion, radius, height, obstacle))
                {
                    fraction = std::min(fraction, hit->fraction);
                }
            };
            for (const auto& obstacle : _environment.SolidCylinders())
            {
                clip(obstacle);
            }
            for (const auto& obstacle : _environment.SolidBoxes())
            {
                clip(obstacle);
            }
            const double warnedLength = length * fraction;
            if (warnedLength <= .001)
            {
                return;
            }
            const float width = std::max(.08f, radius * 2);
            const float rightX = static_cast<float>(dz / length), rightZ = static_cast<float>(-dx / length);
            const auto segments = static_cast<std::uint32_t>(std::ceil(warnedLength / .5));
            auto previous = _terrain->Sample({motion.start.x, motion.start.z});
            for (std::uint32_t i = 1; previous && i <= segments; ++i)
            {
                const double t = fraction * static_cast<double>(i) / segments;
                const float x = static_cast<float>(motion.start.x + dx * t);
                const float z = static_cast<float>(motion.start.z + dz * t);
                const auto next = _terrain->Sample({x, z});
                if (!next || (!projectile && (next->normal.y < _minimumNormalY ||
                    x < _playBounds.min.x + radius || x > _playBounds.max.x - radius ||
                    z < _playBounds.min.y + radius || z > _playBounds.max.y - radius)))
                {
                    break;
                }
                const auto left = _terrain->Sample({(previous->position.x + x) * .5f - rightX * width * .5f,
                    (previous->position.z + z) * .5f - rightZ * width * .5f});
                const auto right = _terrain->Sample({(previous->position.x + x) * .5f + rightX * width * .5f,
                    (previous->position.z + z) * .5f + rightZ * width * .5f});
                const float sideRise = left && right ? right->position.y - left->position.y : 0;
                Engine::DrawItem item;
                item.model = _attackWarningModel;
                item.world = {rightX * width, sideRise, rightZ * width, 0,
                    0, 1, 0, 0, next->position.x - previous->position.x,
                    next->position.y - previous->position.y, next->position.z - previous->position.z, 0,
                    previous->position.x, previous->position.y + .055f, previous->position.z, 1};
                item.tint = projectile ? Engine::Vector4{1, .48f, .12f, 1} : Engine::Vector4{1, .72f, .12f, 1};
                item.fadeCoverage = projectile ? .75f : .4f;
                item.castsShadow = false;
                item.receivesShadow = false;
                frame.draws.push_back(std::move(item));
                previous = next;
            }
        };
        if (definition.charge && enemy.chargePhase == EnemyChargePhase::Windup)
        {
            const auto& charge = *definition.charge;
            ribbon({enemy.position, {enemy.position.x + enemy.chargeDirection.x * charge.speed * charge.duration,
                enemy.position.y, enemy.position.z + enemy.chargeDirection.y * charge.speed * charge.duration}},
                enemy.radius, enemy.height, false);
        }
        if (definition.ranged && enemy.rangedCharging)
        {
            const auto& ranged = *definition.ranged;
            const Engine::Vector3 start{enemy.position.x, enemy.position.y + enemy.height * .55f, enemy.position.z};
            const double x = static_cast<double>(enemy.rangedAim.x) - start.x;
            const double y = static_cast<double>(enemy.rangedAim.y) - start.y;
            const double z = static_cast<double>(enemy.rangedAim.z) - start.z;
            const double length = std::sqrt(x * x + y * y + z * z);
            if (length <= .000001)
            {
                return;
            }
            for (std::uint32_t i = 0; i < ranged.projectileCount; ++i)
            {
                const double angle = ranged.projectileCount > 1 ? ranged.spreadRadians *
                    (static_cast<double>(i) / (ranged.projectileCount - 1) - .5) : 0;
                const double sine = std::sin(angle), cosine = std::cos(angle);
                const Engine::Vector3 velocity{
                    static_cast<float>((x * cosine + z * sine) / length * ranged.projectileSpeed),
                    static_cast<float>(y / length * ranged.projectileSpeed),
                    static_cast<float>((z * cosine - x * sine) / length * ranged.projectileSpeed)};
                ribbon({start, {start.x + velocity.x * ranged.projectileLife,
                    start.y + velocity.y * ranged.projectileLife, start.z + velocity.z * ranged.projectileLife}},
                    ranged.projectileRadius, ranged.projectileRadius * 2, true);
            }
        }
    }

    void LastFieldApp::BuildRenderFrame(Engine::RenderFrame& out)
    {
        out.clearColor =
            _paused ? std::array<float, 4>{0.14f, 0.08f, 0.18f, 1.0f} : std::array<float, 4>{0.04f, 0.12f, 0.18f, 1.0f};
        out.camera = _camera.Snapshot();
        out.instancingEnabled = _instancing;
        out.frustumCullingEnabled = _culling;
        if (_gameMode)
        {
            out.clearColor = {.08f, .095f, .10f, 1.0f};
            out.shadow.enabled = _shadows;
            const auto terrainDesc = _terrain->Description();
            const float terrainShadowScale = std::max(1.0f, static_cast<float>(std::max(terrainDesc.width, terrainDesc.height) - 1) *
                                                               terrainDesc.cellSize / 24.0f);
            const float shadowScale = std::min(2.0f, terrainShadowScale);
            // Larger scenery keeps the existing 68m shadow coverage around the grounded player.
            const auto shadowCenter = terrainShadowScale > 2.0f ? _run->PlayerCollider().base : Engine::Vector3{};
            const auto view =
                DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(shadowCenter.x + 12 * shadowScale,
                                              shadowCenter.y + 24 * shadowScale, shadowCenter.z - 16 * shadowScale, 1),
                                          DirectX::XMVectorSet(shadowCenter.x, shadowCenter.y, shadowCenter.z, 1),
                                          DirectX::XMVectorSet(0, 1, 0, 0));
            DirectX::XMStoreFloat4x4(&out.shadow.viewProjection,
                                     view * DirectX::XMMatrixOrthographicLH(34 * shadowScale, 34 * shadowScale, 1, 65 * shadowScale));
            out.light.direction = {-12, -24, 16};
            out.light.ambient = 0.38f;
            out.light.color = {.82f, .86f, .88f};
            out.light.intensity = .75f;
            out.light.specularIntensity = 0.06f;
            out.light.specularPower = 32;
        }
        auto terrainDraw = _terrain->BuildDrawItem();
        terrainDraw.wireframe = _wireframe;
        out.draws.push_back(std::move(terrainDraw));
        const Engine::Vector4 playerTint =
            _gameMode && _run->Player().dead
                ? Engine::Vector4{0.6f, 0.15f, 0.15f, 1}
                : (_gameMode && _playerHitFlash > 0 ? Engine::Vector4{1, 1, 0.15f, 1} : Engine::Vector4{1, 1, 1, 1});
        out.draws.push_back({_character, _characterTransform.WorldMatrix(), playerTint, false, _animator.Pose()});
        if (_showPools)
        {
            const auto prop = [&](const std::shared_ptr<const Engine::Model>& model, Engine::Vector3 position,
                                  float scale, Engine::Vector4 tint, float yaw = 0)
            {
                Engine::Transform transform;
                if (!model || !transform.SetScale({scale, scale, scale}) || !transform.SetPosition(position) ||
                    !transform.SetRotation({0, std::sin(yaw * .5f), 0, std::cos(yaw * .5f)}))
                {
                    throw std::runtime_error("Client static prop draw is invalid");
                }
                out.draws.push_back({model, transform.WorldMatrix(), tint});
            };
            const auto draw = [&](Engine::Vector3 position, float scale, Engine::Vector4 tint, float yaw = 0,
                                  std::shared_ptr<const Engine::AnimationPose> pose = {})
            {
                Engine::Transform transform;
                position.y -= _character->BoundsMin().y * scale;
                if (!transform.SetScale({scale, scale, scale}) || !transform.SetPosition(position) ||
                    !transform.SetRotation({0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}))
                {
                    throw std::runtime_error("Client diagnostic draw has an invalid transform");
                }
                out.draws.push_back(
                    {_character, transform.WorldMatrix(), tint, false, pose ? std::move(pose) : _poolPose});
            };
            _run->Enemies().ForEachActive(
                [&](Engine::PoolHandle handle, const Enemy& value)
                {
                    const EntityRef ref{EntityKind::Enemy, handle, _run->RunEpoch()};
                    Engine::Vector4 tint = _gameMode ? Engine::Vector4{1, 1, 1, 1}
                                                   : Engine::Vector4{1.0f, 0.25f, 0.25f, 1.0f};
                    const EnemyVisual* visual = nullptr;
                    if (_gameMode && handle.index < _enemyVisuals.size() && _enemyVisuals[handle.index].ref == ref)
                    {
                        visual = &_enemyVisuals[handle.index];
                    }
                    bool targeted = _nearest && ref == *_nearest;
                    if (_gameMode)
                    {
                        for (const auto& slot : _run->WeaponSlots())
                        {
                            targeted = targeted || slot.target == ref;
                        }
                    }
                    if (targeted)
                    {
                        tint = _gameMode ? Engine::Vector4{1, .92f, .8f, 1}
                                         : Engine::Vector4{0.25f, 0.8f, 1, 1};
                    }
                    if (_gameMode && (value.chargePhase == EnemyChargePhase::Recovery || value.rangedRecovery > 0))
                    {
                        tint = {.55f, .70f, .76f, 1};
                    }
                    if ((visual && visual->hitFlash > 0) ||
                        (!_gameMode && _hitFlash > 0 && _lastHit && _lastHit->target == ref))
                    {
                        tint = {1, 1, 0.15f, 1};
                    }
                    if (_gameMode && (value.chargePhase == EnemyChargePhase::Windup || value.rangedCharging))
                    {
                        tint = {1, .58f, .3f, 1};
                    }
                    if (_gameMode && visual)
                    {
                        const auto& asset = _enemyAssets.at(value.definitionId);
                        const float scale = value.height / asset.sourceHeight;
                        auto position = value.position;
                        position.y -= asset.model->BoundsMin().y * scale;
                        Engine::Transform transform;
                        if (!transform.SetScale({scale, scale, scale}) || !transform.SetPosition(position) ||
                            !transform.SetRotation({0, std::sin(visual->yaw * .5f), 0, std::cos(visual->yaw * .5f)}))
                        {
                            throw std::runtime_error("Enemy visual transform is invalid");
                        }
                        out.draws.push_back({asset.model, transform.WorldMatrix(), tint, false, visual->animator.Pose()});
                    }
                    else
                    {
                        draw(value.position, _visualScale, tint, 0, _poolPose);
                    }
                    if (_gameMode)
                    {
                        const auto* definition = _definitions->FindEnemy(value.definitionId);
                        if (definition && value.definitionId == _run->Wave().bossEnemyId && value.hp > 0 &&
                            _run->Wave().phase == WavePhase::Combat && _run->Wave().outcome == RunOutcome::Playing)
                        {
                            AppendAttackWarning(out, value, *definition);
                        }
                        const auto vp =
                            DirectX::XMLoadFloat4x4(&out.camera.view) * DirectX::XMLoadFloat4x4(&out.camera.projection);
                        Engine::Vector4 clip;
                        DirectX::XMStoreFloat4(
                            &clip, DirectX::XMVector4Transform(
                                       DirectX::XMVectorSet(value.position.x, value.position.y + value.height + .15f,
                                                            value.position.z, 1),
                                       vp));
                        if (definition && clip.w > 0)
                        {
                            const float x = clip.x / clip.w * .5f + .5f, y = .5f - clip.y / clip.w * .5f;
                            if (x > .02f && x < .98f && y > .01f && y < .99f)
                            {
                                out.ui.rects.push_back({{x - .018f, y}, {.036f, .005f}, {.03f, .04f, .05f, .9f}});
                                out.ui.rects.push_back(
                                    {{x - .018f, y},
                                     {.036f * std::clamp(value.hp / definition->hp, 0.0f, 1.0f), .005f},
                                     {.95f, .25f, .22f, 1}});
                            }
                        }
                    }
                });
            if (_collisionTrial && _lastHit && _hitFlash > 0)
            {
                draw(_lastHit->position, _visualScale * 0.18f, {1, 0.15f, 1, 1});
            }
            _run->Projectiles().ForEachActive(
                [&](Engine::PoolHandle, const Projectile& value)
                {
                    const float yaw = _gameMode ? std::atan2(value.velocity.x, value.velocity.z) : 0;
                    if (_gameMode)
                    {
                        if (value.owner.kind == EntityKind::Enemy)
                        {
                            const auto* definition = _definitions->FindEnemy(value.definitionId);
                            if (!definition || !definition->ranged)
                            {
                                throw std::runtime_error("Enemy projectile visual references an invalid ranged definition");
                            }
                            prop(_projectileModels[0], value.position, 1.25f, {1, .3f, .12f, 1}, yaw);
                        }
                        else
                        {
                            const auto* definition = _definitions->FindWeapon(value.definitionId);
                            if (!definition || definition->kind == WeaponKind::Melee)
                            {
                                throw std::runtime_error("Projectile visual references an invalid weapon definition");
                            }
                            const auto& model = _projectileModels[definition->kind == WeaponKind::Shotgun ? 1 : 0];
                            prop(model, value.position, 1, {1, 1, 1, 1}, yaw);
                        }
                    }
                    else
                    {
                        draw(value.position, _visualScale * 0.25f, {0.2f, 1.0f, 1.0f, 1.0f}, yaw);
                    }
                });
            _run->Drops().ForEachActive(
                [&](Engine::PoolHandle, const Drop& value)
                {
                    const Engine::Vector4 tint = _gameMode && value.kind == DropKind::Experience
                                                     ? Engine::Vector4{0.3f, 1, 0.35f, 1}
                                                     : Engine::Vector4{1.0f, 0.85f, 0.2f, 1.0f};
                    if (_gameMode)
                    {
                        auto position = value.position;
                        position.y += .2f;
                        prop(value.kind == DropKind::Experience ? _experienceModel : _currencyModel, position, 1,
                             {1, 1, 1, 1});
                    }
                    else
                    {
                        draw(value.position, _visualScale * 0.35f, tint);
                    }
                });
            if (_gameMode)
            {
                const auto slots = _run->WeaponSlots();
                std::optional<WeaponKind> activeKind;
                if (_playerAction == PlayerAction::Attack && _activeWeaponSlot && *_activeWeaponSlot < slots.size())
                {
                    const auto* definition = _definitions->FindWeapon(slots[*_activeWeaponSlot].definitionId);
                    if (!definition)
                    {
                        throw std::runtime_error("Player action references an unknown weapon definition");
                    }
                    activeKind = definition->kind;
                }
                std::array<bool, 3> submitted = {};
                for (const auto& slot : slots)
                {
                    if (_run->Player().dead)
                    {
                        break;
                    }
                    const auto* definition = _definitions->FindWeapon(slot.definitionId);
                    if (!definition)
                    {
                        throw std::runtime_error("Client visual weapon slot references an unknown definition");
                    }
                    const auto kind = static_cast<std::size_t>(definition->kind);
                    if (!_weaponAttachmentsEnabled || definition->kind == WeaponKind::Shotgun || submitted[kind])
                    {
                        continue;
                    }
                    submitted[kind] = true;
                    if (definition->kind == WeaponKind::Melee && activeKind && *activeKind != WeaponKind::Melee)
                    {
                        continue;
                    }
                    const bool stowedBow = definition->kind == WeaponKind::SingleProjectile &&
                        activeKind != WeaponKind::SingleProjectile;
                    const auto& attachment = stowedBow ? _stowedBowAttachment : _weaponAttachments[kind];
                    const auto pose = _animator.Pose();
                    if (!pose || attachment.nodeIndex >= pose->nodeWorld.size())
                    {
                        throw std::runtime_error("Player weapon attachment pose/node is invalid");
                    }
                    Engine::Matrix world;
                    const auto characterWorld = _characterTransform.WorldMatrix();
                    DirectX::XMStoreFloat4x4(&world, DirectX::XMLoadFloat4x4(&attachment.local) *
                        DirectX::XMLoadFloat4x4(&pose->nodeWorld[attachment.nodeIndex]) *
                        DirectX::XMLoadFloat4x4(&characterWorld));
                    out.draws.push_back({_weaponModels[kind], world, {1, 1, 1, 1}});
                }
                for (const auto& marker : _eventMarkers)
                {
                    if (marker.remaining <= 0)
                    {
                        continue;
                    }
                    auto position = marker.event.position;
                    const bool pickup = marker.event.kind == GameEventKind::PickedUp;
                    position.y += pickup ? 0.4f : 0.05f;
                    prop(_experienceModel, position, pickup ? 1.0f : 1.5f,
                         pickup ? Engine::Vector4{0.3f, 1, 0.5f, 1} : Engine::Vector4{1, 0.15f, 0.3f, 1});
                }
            }
        }
        if (_gameMode)
        {
            for (const auto& visual : _deathVisuals)
            {
                if (visual.active)
                {
                    out.draws.push_back({visual.model, visual.world, {.78f, .78f, .78f, 1},
                        false, visual.animator.Pose()});
                }
            }
            _environment.Append(out);
        }
        if (_gameMode && !_combatEffects.Append(out))
        {
            throw std::runtime_error("Client particle snapshot exceeds render capacity");
        }
        auto enemyBars = std::move(out.ui.rects);
        BuildHud(Hud(), out.ui, _application->Graphics().Width(), _application->Graphics().Height());
        out.ui.rects.insert(out.ui.rects.begin(), enemyBars.begin(), enemyBars.end());
    }

    void LastFieldApp::SetParticleEffectsEnabled(bool enabled) noexcept
    {
        _combatEffects.SetEnabled(enabled);
    }

    Engine::ParticleStats LastFieldApp::ParticleStats() const noexcept
    {
        return _combatEffects.Stats();
    }

    void LastFieldApp::Shutdown()
    {
        _audio.Shutdown();
        _sounds.clear();
        _environment.Clear();
        _weaponModels = {};
        _projectileModels = {};
        _experienceModel.reset();
        _currencyModel.reset();
        _attackWarningModel.reset();
        _run.reset();
        ResetPresentation();
        _trialMelee.hitTargets.clear();
        _collisionTrial = false;
        _gameMode = false;
        _automaticAttack = true;
        _enemyVisuals.clear();
        _deathVisuals.clear();
        _enemyAssets.clear();
        _definitions.reset();
        _poolPose.reset();
        _animator.Reset();
        _idleClip.reset();
        _moveClip.reset();
        _attackClip.reset();
        _deathClip.reset();
        _weaponAttackClips = {};
        _weaponAttachments = {};
        _stowedBowAttachment = {};
        _weaponAttachmentsEnabled = false;
        _character.reset();
        _terrain.reset();
        _application = nullptr;
        _paused = false;
        _wireframe = false;
        _showSurface = false;
        _moving = false;
        _showPools = false;
        _frameCount = 0;
        _diagnosticSeconds = 0;
        _diagnosticRunEpoch = 0;
        _diagnosticPhase = WavePhase::Ready;
        _diagnosticOutcome = RunOutcome::Playing;
        Engine::Log::Write("LastFieldApp: shutdown");
    }
}
