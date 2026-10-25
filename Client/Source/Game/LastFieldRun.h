#pragma once

#include "Entity/ObjectPool.h"
#include "Game/PoolEntities.h"
#include "Game/GameDefinitions.h"
#include "Game/RunProgression.h"
#include "Scene/Geometry.h"
#include "Scene/SpatialGrid.h"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <memory>
#include <span>
#include <vector>

namespace Engine
{
    class Terrain;
}

namespace Client
{
    struct RunConfig
    {
        std::uint32_t enemyCapacity = 8;
        std::uint32_t projectileCapacity = 16;
        std::uint32_t dropCapacity = 8;
        Engine::BoundsXZ playBounds{{-8, -8}, {8, 8}};
        float cellSize = 2;
        float maxSlopeDegrees = 30;
        std::span<const Engine::Cylinder> staticCylinders;
        std::span<const Engine::UprightBox> staticBoxes;
    };

    struct EnemySpawn
    {
        Engine::Vector3 position = {};
        std::uint32_t definitionId = 1;
        float hp = 10;
        float radius = 0.35f;
        float height = 1.8f;
    };

    struct ProjectileSpawn
    {
        Engine::Vector3 position = {};
        Engine::Vector3 velocity = {};
        EntityRef owner;
        std::uint32_t definitionId = 1;
        float attackPower = 1;
        double ttl = 3;
        float radius = 0.12f;
        float knockback = 0;
        std::uint32_t slotIndex = 0;
    };

    struct DropSpawn
    {
        Engine::Vector3 position = {};
        DropKind kind = DropKind::Experience;
        std::uint32_t definitionId = 1;
        std::uint32_t value = 1;
    };

    enum class DropSpawnResult
    {
        Rejected,
        Spawned,
        Merged,
        Ledger
    };

    // Client owns combat and wave policy; Engine pools/geometry remain game independent.
    class LastFieldRun final
    {
    public:
        LastFieldRun(std::uint64_t runEpoch, const Engine::Terrain& terrain);
        bool Initialize(const RunConfig& config);
        std::optional<EntityRef> SpawnEnemy(const EnemySpawn& spawn);
        bool SpawnProjectiles(std::span<const ProjectileSpawn> batch);
        DropSpawnResult SpawnDrop(const DropSpawn& spawn);
        Enemy* TryEnemy(const EntityRef& ref) noexcept;
        Projectile* TryProjectile(const EntityRef& ref) noexcept;
        Drop* TryDrop(const EntityRef& ref) noexcept;
        bool IsValid(const EntityRef& ref) const noexcept;
        bool RequestRelease(const EntityRef& ref);
        void Tick(float dt, Engine::Vector2 movement = {});
        bool Clear();
        bool BuildTrialWave();
        bool BuildCollisionTrial();
        bool SetPlayerPosition(Engine::Vector3 position);
        Engine::Cylinder PlayerCollider() const noexcept;
        Engine::Vector2 PlayerFacing() const noexcept;
        std::optional<EntityRef> FindNearest(Engine::Vector3 origin, float range, float height, bool useGrid = true);
        bool BeginMelee(MeleeAttack& attack, const EntityRef& source);
        bool TestMelee(MeleeAttack& attack, Engine::Vector3 origin, Engine::Vector2 forward,
            float range, float halfAngleRadians, float height, bool useGrid = true);
        bool FireTrialProjectile();
        bool ConfigureGame(std::shared_ptr<const GameDefinitions> definitions,
            std::span<const std::uint32_t> loadout = {}, std::uint64_t seed = 42);
        bool GameConfigured() const noexcept;
        bool BeginWave();
        bool PrepareNextWave();
        void SetAutoAttackEnabled(bool enabled) noexcept;
        const PlayerState& Player() const noexcept;
        const PlayerDefinition& PlayerStats() const noexcept;
        PlayerViewTarget ViewTarget() const noexcept;
        ProgressionSnapshot Progression() const;
        ProgressionResult ChooseUpgrade(std::uint64_t epoch, std::uint64_t choiceId, std::uint32_t index);
        ProgressionResult SkipUpgrade(std::uint64_t epoch, std::uint64_t choiceId);
        ProgressionResult BuyOffer(std::uint64_t epoch, std::uint64_t revision, std::uint64_t offerId);
        ProgressionResult RerollShop(std::uint64_t epoch, std::uint64_t revision);
        ProgressionResult LeaveShop(std::uint64_t epoch, std::uint64_t revision);
        std::span<const WeaponSlot> WeaponSlots() const noexcept;
        const WaveSnapshot& Wave() const noexcept;
        const RewardTotals& Rewards() const noexcept;
        std::span<const GameEvent> Events() const noexcept;
        bool RewardConservation() const noexcept;
        std::span<const HitEvent> Hits() const noexcept;
        void ClearHits() noexcept;
        bool CheckInvariants() const noexcept;
        std::uint64_t RunEpoch() const noexcept;
        std::uint64_t TickIndex() const noexcept;
        std::uint64_t Ledger(DropKind kind) const noexcept;
        std::uint64_t ProjectileBatchFailures() const noexcept;
        std::uint64_t DropMerges() const noexcept;
        std::uint64_t DropLedgerAdds() const noexcept;

        const Engine::ObjectPool<Enemy>& Enemies() const noexcept;
        const Engine::ObjectPool<Projectile>& Projectiles() const noexcept;
        const Engine::ObjectPool<Drop>& Drops() const noexcept;

    private:
        struct NavigationProfile
        {
            float radius = 0, height = 0;
            // Bits 0..7 are edges; bit 8 marks a walkable node.
            std::vector<std::uint16_t> links;
            std::vector<std::uint32_t> distance;
            std::vector<std::uint32_t> queue;
        };

        bool PrepareNavigation(const GameDefinitions& definitions, std::vector<NavigationProfile>& out);
        void UpdateNavigation(float dt);
        Engine::Vector2 PursuitDirection(const Enemy& enemy) const;
        Engine::Vector3 NavigationApproach(Engine::Vector3 from, float radius) const noexcept;
        bool NavigationSegmentClear(Engine::Vector3 from, Engine::Vector3 to,
            float radius, float height) const noexcept;

        bool NavigationRouteClear(Engine::Vector3 from, Engine::Vector3 to,
            float radius, float height) const;

        EntityRef Ref(EntityKind kind, Engine::PoolHandle handle) const noexcept;
        bool NextEligibleTick(std::uint64_t& out) const noexcept;
        bool RebuildGrid();
        bool GatherCircle(Engine::Vector3 center, float range, bool useGrid);
        bool MeleePathClear(Engine::Vector3 origin, float height, const Enemy& enemy) const noexcept;
        Engine::Vector3 WeaponShotVelocity(const WeaponDefinition& weapon, Engine::Vector2 direction,
            const Enemy* target, std::uint32_t index) const noexcept;
        bool ProjectilePathClear(Engine::Vector3 start, Engine::Vector3 velocity, double life,
            float radius, const Engine::Cylinder& target) const;
        std::optional<EntityRef> FindWeaponTarget(const WeaponDefinition& weapon);
        bool EnemyHasShot(const Enemy& enemy, const EnemyRangedDefinition& ranged) const;
        void PrepareStaticGrid(Engine::BoundsXZ bounds);
        // Single-threaded scratch: callers finish exact Engine geometry tests before another query.
        std::span<const std::uint32_t> StaticCandidates(const Engine::Segment& motion, float radius) const noexcept;
        bool OverlapsEnvironment(const Engine::Cylinder& body) const noexcept;
        std::optional<Engine::GeometryHit> FirstStaticHit(const Engine::Segment& motion,
            float radius, float height) const noexcept;
        Engine::Vector3 MoveBody(Engine::Cylinder body, Engine::Vector2 displacement,
            const std::optional<EntityRef>& ignored, bool collideWithPlayer);
        void MoveActors(float dt, Engine::Vector2 movement);
        void StepProjectile(Engine::PoolHandle handle, Projectile& projectile, float dt);
        void SortHits();
        void TickGame(float dt, Engine::Vector2 movement);
        void UpdateEnemyIntent(float dt);
        void UpdateEnemyCharge(Enemy& enemy, const EnemyChargeDefinition& charge, float dt);
        void GatherContactDamage();
        void ApplyDamage();
        void AttackWeapons();
        void AttackRangedEnemies(float dt);
        bool ReserveWeaponShots(std::uint32_t slotIndex, const WeaponDefinition& weapon,
            Engine::Vector2 direction);
        void CommitWeaponShots(bool allow);
        void FinalizeDeaths(bool spawnDrops);
        void CollectDrops(bool all);
        void FinishWave(bool defeated);
        void SpawnWaveEnemies(float dt);
        std::optional<EntityRef> TrySpawnWaveEnemy(const EnemyDefinition& enemy, const WaveDefinition& wave);
        void FlushReturns();
        void RecordHit(const HitEvent& hit);
        void Emit(GameEvent event);
        void SyncProgressionRewards();
        void ApplyProgression();
        std::uint32_t RandomU32() noexcept;

        struct BodyEntry
        {
            EntityRef ref;
            Engine::Cylinder body;
        };

        std::vector<NavigationProfile> _navigationProfiles;
        std::vector<Engine::Vector3> _navigationPoints;
        Engine::Vector2 _navigationOrigin{};
        Engine::Vector3 _navigationTarget{};
        float _navigationCellSize = 1;
        std::uint32_t _navigationColumns = 0, _navigationRows = 0;
        std::uint32_t _navigationGoal = UINT32_MAX;
        float _navigationRefresh = 0;

        const Engine::Terrain& _terrain;
        Engine::ObjectPool<Enemy> _enemies;
        Engine::ObjectPool<Projectile> _projectiles;
        Engine::ObjectPool<Drop> _drops;
        std::vector<Engine::PoolHandle> _enemyReservation;
        std::vector<Engine::PoolHandle> _projectileReservation;
        std::vector<Engine::PoolHandle> _dropReservation;
        Engine::SpatialGrid<EntityRef> _grid;
        std::vector<EntityRef> _candidates;
        std::vector<BodyEntry> _bodies;
        std::vector<Engine::Cylinder> _staticCylinders;
        std::vector<Engine::UprightBox> _staticBoxes;
        Engine::SpatialGrid<std::uint32_t> _staticGrid;
        std::vector<std::uint32_t> _staticIds;
        mutable std::vector<std::uint32_t> _staticCandidates;
        bool _staticGridReady = false;
        std::vector<HitEvent> _hits;
        std::vector<HitEvent> _newMeleeHits;
        struct PendingShot
        {
            std::uint32_t slotIndex = 0;
            std::size_t first = 0, count = 0;
            EntityRef owner;
        };
        std::shared_ptr<const GameDefinitions> _definitions;
        std::vector<WeaponSlot> _weaponSlots;
        std::vector<WeaponSlot> _spareWeaponSlots;
        std::unique_ptr<RunProgression> _progression;
        PlayerDefinition _playerStats;
        std::uint32_t _observedLevel = 1;
        std::vector<HitEvent> _damageRequests;
        std::vector<HitEvent> _appliedThisTick;
        std::vector<Engine::PoolHandle> _hitProjectiles;
        std::vector<Engine::PoolHandle> _pendingProjectileHandles;
        std::vector<PendingShot> _pendingShots;
        std::vector<GameEvent> _events;
        PlayerState _playerState;
        RewardTotals _rewards;
        WaveSnapshot _wave;
        std::optional<EntityRef> _boss;
        std::uint64_t _randomState = 42, _eventSequence = 1;
        bool _autoAttack = true, _inGameTick = false;
        bool _queryGridReady = false;
        Engine::Cylinder _player{{0, 0, 0}, 0.35f, 1.8f};
        Engine::Vector2 _playerFacing{0, 1};
        Engine::BoundsXZ _gridBounds{};
        Engine::BoundsXZ _playBounds{{-8, -8}, {8, 8}};
        float _minimumNormalY = 0.8660254f;
        std::uint64_t _nextAttackId = 1;
        std::uint64_t _minimumAttackId = 1;
        std::uint64_t _runEpoch = 0;
        std::uint64_t _tick = 0;
        std::uint64_t _experienceLedger = 0;
        std::uint64_t _currencyLedger = 0;
        std::uint64_t _projectileBatchFailures = 0;
        std::uint64_t _dropMerges = 0;
        std::uint64_t _dropLedgerAdds = 0;
        bool _initialized = false;
    };
}
