#pragma once

#include "App/IGameApp.h"
#include "Audio/AudioDevice.h"
#include "Animation/Animator.h"
#include "Entity/Transform.h"
#include "Graphics/Camera.h"
#include "Game/GameDefinitions.h"
#include "Game/LastFieldRun.h"
#include "Scene/Terrain.h"
#include "Scene/Environment.h"
#include "UI/LastFieldHud.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <map>
#include <string>

namespace Engine
{
    class Model;
    class AnimationClip;
    class AudioClip;
}

namespace Client
{
    class LastFieldApp final : public Engine::IGameApp
    {
    public:
        explicit LastFieldApp(std::filesystem::path resourceRoot, bool smoke = false);

        bool Initialize(Engine::Application& app) override;
        void FixedUpdate(const Engine::InputState& input, float dt) override;
        Engine::AppAction Update(const Engine::InputState& input, float dt) override;
        void BuildRenderFrame(Engine::RenderFrame& out) override;
        void Shutdown() override;
        HudSnapshot Hud() const;

    private:
        bool UpdateCamera(float dt);
        bool RestartTrialRun();
        bool RestartGameRun();
        void ResetPresentation() noexcept;
        void SyncPlayerTransform();
        void UpdatePlayerVisual(double simulationSeconds, double presentationSeconds, bool moved);
        void UpdateEnemyVisuals(double simulationSeconds);
        void UpdateDeathVisuals(double seconds);
        void BeginEnemyDeath(const GameEvent& event);
        void CaptureGameEvents();
        bool LoadPresentation(std::string& error);
        void ExecuteHudCommand(const HudCommand& command);
        void SnapCamera();

        struct CharacterAsset
        {
            std::shared_ptr<const Engine::Model> model;
            std::shared_ptr<const Engine::AnimationClip> idle;
            std::shared_ptr<const Engine::AnimationClip> move;
            float sourceHeight = 1;
            std::shared_ptr<const Engine::AnimationClip> attack;
            std::shared_ptr<const Engine::AnimationClip> death;
        };

        struct WeaponAttachment
        {
            std::size_t nodeIndex = 0;
            Engine::Matrix local = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        };

        struct EnemyVisual
        {
            Engine::Animator animator;
            std::optional<EntityRef> ref;
            Engine::Vector3 position = {};
            float yaw = 0;
            float hitFlash = 0;
            bool moving = false;
            bool seen = false;
            std::uint32_t definitionId = 0;
            bool attacking = false;
        };

        struct DeathVisual
        {
            Engine::Animator animator;
            std::shared_ptr<const Engine::Model> model;
            Engine::Matrix world = {};
            double elapsed = 0;
            double duration = 0;
            bool active = false;
        };

        struct EventMarker
        {
            GameEvent event;
            float remaining = 0;
        };

        std::filesystem::path _resourceRoot;
        Engine::Application* _application = nullptr;
        std::shared_ptr<const Engine::Model> _character;
        std::shared_ptr<const Engine::AnimationClip> _idleClip;
        std::shared_ptr<const Engine::AnimationClip> _moveClip;
        std::shared_ptr<const Engine::AnimationClip> _attackClip;
        std::shared_ptr<const Engine::AnimationClip> _deathClip;
        std::array<std::shared_ptr<const Engine::AnimationClip>, 3> _weaponAttackClips;
        std::array<WeaponAttachment, 3> _weaponAttachments;
        WeaponAttachment _stowedBowAttachment;
        std::shared_ptr<const Engine::AnimationClip> _activeAttackClip;
        std::optional<std::size_t> _activeWeaponSlot;
        bool _weaponAttachmentsEnabled = false;
        std::map<std::uint32_t, CharacterAsset> _enemyAssets;
        std::shared_ptr<const GameDefinitions> _definitions;
        std::array<std::shared_ptr<const Engine::Model>, 3> _weaponModels;
        std::array<std::shared_ptr<const Engine::Model>, 2> _projectileModels;
        std::shared_ptr<const Engine::Model> _experienceModel, _currencyModel;
        std::map<std::string, std::shared_ptr<const Engine::AudioClip>> _sounds;
        Engine::AudioDevice _audio;
        Environment _environment;
        std::optional<HudCommand> _pendingHudCommand;
        std::string _hudMessage;
        Engine::Animator _animator;
        std::shared_ptr<const Engine::AnimationPose> _poolPose;
        std::unique_ptr<Engine::Terrain> _terrain;
        std::unique_ptr<LastFieldRun> _run;
        MeleeAttack _trialMelee;
        std::optional<EntityRef> _nearest;
        std::optional<HitEvent> _lastHit;
        std::optional<GameEvent> _lastGameEvent;
        std::vector<EnemyVisual> _enemyVisuals;
        std::vector<DeathVisual> _deathVisuals;
        std::size_t _nextDeathVisual = 0;
        enum class PlayerAction
        {
            Locomotion,
            Attack,
            Death
        };
        PlayerAction _playerAction = PlayerAction::Locomotion;
        float _attackYaw = 0;
        std::array<EventMarker, 16> _eventMarkers = {};
        std::size_t _nextMarker = 0;
        std::uint64_t _lastEventSequence = 0;
        float _hitFlash = 0;
        float _playerHitFlash = 0;
        Engine::Transform _characterTransform;
        Engine::Camera _camera;
        Engine::Vector3 _cameraFocus = {};
        float _visualScale = 1.0f;
        float _footOffset = 0.0f;
        float _minimumNormalY = 1.0f;
        bool _smoke = false;
        bool _paused = false;
        bool _wireframe = false;
        bool _showSurface = false;
        bool _moving = false;
        bool _showPools = false;
        bool _collisionTrial = false;
        bool _gameMode = false;
        bool _automaticAttack = true;
        bool _shadows = true, _instancing = true;
        bool _culling = false;
        bool _wasInactive = true;
        std::uint64_t _nextRunEpoch = 1;
        std::uint32_t _frameCount = 0;
    };
}
