#include "Game/LastFieldRun.h"

#include "Scene/Terrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace
{
    bool Finite(Engine::Vector3 value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool ValidKind(Client::DropKind kind) noexcept
    {
        return kind == Client::DropKind::Experience || kind == Client::DropKind::Currency;
    }

    bool HandleLess(const Client::EntityRef& first, const Client::EntityRef& second) noexcept
    {
        const auto a = first.handle.value_or(Engine::PoolHandle{});
        const auto b = second.handle.value_or(Engine::PoolHandle{});
        return std::tie(a.storageId, a.index, a.generation) < std::tie(b.storageId, b.index, b.generation);
    }

    bool Targetable(const Client::Enemy& enemy, std::uint64_t tick) noexcept
    {
        return enemy.eligibleTick <= tick && enemy.hp > 0 && !enemy.deathFinalized;
    }
}

namespace Client
{
    LastFieldRun::LastFieldRun(std::uint64_t runEpoch, const Engine::Terrain& terrain)
        : _terrain(terrain), _runEpoch(runEpoch)
    {
    }

    bool LastFieldRun::Initialize(const RunConfig& config)
    {
        _queryGridReady = false;
        if (_initialized || _runEpoch == 0 || config.enemyCapacity == 0 ||
            config.projectileCapacity == 0 || config.dropCapacity == 0 ||
            config.enemyCapacity > 10000 || config.projectileCapacity > 10000 || config.dropCapacity > 10000)
        {
            return false;
        }
        if (config.staticCylinders.size() > 256 || config.staticBoxes.size() > 256 ||
            config.staticCylinders.size() + config.staticBoxes.size() > 256 ||
            std::any_of(config.staticCylinders.begin(), config.staticCylinders.end(),
                [](const auto& shape) { return !Engine::IsValid(shape); }) ||
            std::any_of(config.staticBoxes.begin(), config.staticBoxes.end(),
                [](const auto& shape) { return !Engine::IsValid(shape); }))
        {
            return false;
        }
        const auto terrain = _terrain.Description();
        const Engine::BoundsXZ gridBounds{ {terrain.origin.x, terrain.origin.z},
            {static_cast<float>(static_cast<double>(terrain.origin.x) + (terrain.width - 1) * static_cast<double>(terrain.cellSize)),
                static_cast<float>(static_cast<double>(terrain.origin.z) + (terrain.height - 1) * static_cast<double>(terrain.cellSize))} };
        if (terrain.width < 2 || terrain.height < 2 ||
            !std::isfinite(config.playBounds.min.x) || !std::isfinite(config.playBounds.min.y) ||
            !std::isfinite(config.playBounds.max.x) || !std::isfinite(config.playBounds.max.y) ||
            config.playBounds.min.x >= config.playBounds.max.x || config.playBounds.min.y >= config.playBounds.max.y ||
            !std::isfinite(config.maxSlopeDegrees) || config.maxSlopeDegrees < 0 || config.maxSlopeDegrees >= 90 ||
            !_grid.Initialize(gridBounds, config.cellSize, config.enemyCapacity))
        {
            return false;
        }
        try
        {
            _enemyReservation.reserve(1);
            _projectileReservation.reserve(config.projectileCapacity);
            _dropReservation.reserve(1);
            _candidates.reserve(config.enemyCapacity);
            _bodies.reserve(config.enemyCapacity);
            _staticCylinders.assign(config.staticCylinders.begin(), config.staticCylinders.end());
            _staticBoxes.assign(config.staticBoxes.begin(), config.staticBoxes.end());
            _hits.reserve(static_cast<std::size_t>(config.enemyCapacity) + config.projectileCapacity);
            _newMeleeHits.reserve(config.enemyCapacity);
        }
        catch (const std::exception&)
        {
            return false;
        }
        _initialized = _enemies.Initialize(config.enemyCapacity) &&
            _projectiles.Initialize(config.projectileCapacity) && _drops.Initialize(config.dropCapacity);
        _gridBounds = gridBounds;
        _playBounds = config.playBounds;
        _minimumNormalY = std::cos(config.maxSlopeDegrees * DirectX::XM_PI / 180);
        if (_initialized && SetPlayerPosition({0, 0, 0}))
        {
            return true;
        }
        _initialized = false;
        return false;
    }

    EntityRef LastFieldRun::Ref(EntityKind kind, Engine::PoolHandle handle) const noexcept
    {
        return {kind, handle, _runEpoch};
    }

    bool LastFieldRun::NextEligibleTick(std::uint64_t& out) const noexcept
    {
        if (!_initialized || _tick == std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
        out = _tick + 1;
        return true;
    }

    std::optional<EntityRef> LastFieldRun::SpawnEnemy(const EnemySpawn& spawn)
    {
        std::uint64_t eligibleTick = 0;
        if (!NextEligibleTick(eligibleTick) || !Finite(spawn.position) || spawn.definitionId == 0 ||
            !std::isfinite(spawn.hp) || spawn.hp <= 0 ||
            !Engine::IsValid({spawn.position, spawn.radius, spawn.height}))
        {
            return {};
        }
        if (_definitions && (_wave.phase != WavePhase::Combat || _playerState.dead ||
            !_definitions->FindEnemy(spawn.definitionId)))
        {
            return {};
        }
        if (_definitions && spawn.definitionId == _definitions->waves.back().bossEnemyId &&
            (spawn.definitionId != _wave.bossEnemyId || _wave.bossSpawned))
        {
            return {};
        }
        const auto ground = _terrain.Sample({spawn.position.x, spawn.position.z});
        if (!ground || OverlapsEnvironment({ground->position, spawn.radius, spawn.height}) ||
            !_enemies.Reserve(1, _enemyReservation))
        {
            return {};
        }
        auto* enemy = _enemies.Reserved(_enemyReservation[0]);
        enemy->Reset();
        enemy->position = ground->position;
        enemy->definitionId = spawn.definitionId;
        enemy->hp = spawn.hp;
        enemy->eligibleTick = eligibleTick;
        enemy->radius = spawn.radius;
        enemy->height = spawn.height;
        const auto result = Ref(EntityKind::Enemy, _enemyReservation[0]);
        if (!_enemies.Commit(_enemyReservation))
        {
            enemy->Reset();
            _enemies.Cancel(_enemyReservation);
            return {};
        }
        if (_definitions && spawn.definitionId == _wave.bossEnemyId)
        {
            _boss = result;
            _wave.bossSpawned = true;
            _wave.bossHp = _wave.bossMaxHp = spawn.hp;
        }
        return result;
    }

    bool LastFieldRun::IsValid(const EntityRef& ref) const noexcept
    {
        if (!_initialized || ref.runEpoch != _runEpoch)
        {
            return false;
        }
        if (ref.kind == EntityKind::Player)
        {
            return !ref.handle;
        }
        if (!ref.handle)
        {
            return false;
        }
        switch (ref.kind)
        {
        case EntityKind::Enemy: return _enemies.TryGet(*ref.handle) != nullptr;
        case EntityKind::Projectile: return _projectiles.TryGet(*ref.handle) != nullptr;
        case EntityKind::Drop: return _drops.TryGet(*ref.handle) != nullptr;
        default: return false;
        }
    }

    bool LastFieldRun::SpawnProjectiles(std::span<const ProjectileSpawn> batch)
    {
        std::uint64_t eligibleTick = 0;
        if (!NextEligibleTick(eligibleTick) || batch.empty() || batch.size() > 10000 ||
            _nextAttackId == std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
        if (_definitions && (_wave.phase != WavePhase::Combat || _playerState.dead))
        {
            return false;
        }
        for (const auto& spawn : batch)
        {
            if (!Finite(spawn.position) || !Finite(spawn.velocity) || !IsValid(spawn.owner) ||
                spawn.definitionId == 0 || !std::isfinite(spawn.attackPower) || spawn.attackPower < 0 ||
                !std::isfinite(spawn.ttl) || spawn.ttl <= 0 || !std::isfinite(spawn.knockback) || spawn.knockback < 0 ||
                !Engine::IsValid({spawn.position, spawn.radius, spawn.radius * 2}))
            {
                return false;
            }
        }
        if (!_projectiles.Reserve(static_cast<std::uint32_t>(batch.size()), _projectileReservation))
        {
            ++_projectileBatchFailures;
            return false;
        }
        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            auto* projectile = _projectiles.Reserved(_projectileReservation[i]);
            const auto& spawn = batch[i];
            projectile->Reset();
            projectile->position = spawn.position;
            projectile->velocity = spawn.velocity;
            projectile->owner = spawn.owner;
            projectile->definitionId = spawn.definitionId;
            projectile->attackPower = spawn.attackPower;
            projectile->ttl = spawn.ttl;
            projectile->eligibleTick = eligibleTick;
            projectile->attackId = _nextAttackId;
            projectile->radius = spawn.radius;
            projectile->knockback = spawn.knockback;
            projectile->slotIndex = spawn.slotIndex;
        }
        if (!_projectiles.Commit(_projectileReservation))
        {
            for (const auto handle : _projectileReservation)
            {
                _projectiles.Reserved(handle)->Reset();
            }
            _projectiles.Cancel(_projectileReservation);
            return false;
        }
        ++_nextAttackId;
        return true;
    }

    DropSpawnResult LastFieldRun::SpawnDrop(const DropSpawn& spawn)
    {
        std::uint64_t eligibleTick = 0;
        if (!NextEligibleTick(eligibleTick) || !Finite(spawn.position) || !ValidKind(spawn.kind) ||
            spawn.definitionId == 0 || spawn.value == 0)
        {
            return DropSpawnResult::Rejected;
        }
        auto& generated = spawn.kind == DropKind::Experience ? _rewards.generatedXp : _rewards.generatedCurrency;
        if (_definitions && (_wave.phase != WavePhase::Combat || _playerState.dead ||
                             spawn.value > std::numeric_limits<std::uint64_t>::max() - generated))
        {
            return DropSpawnResult::Rejected;
        }
        const auto ground = _terrain.Sample({spawn.position.x, spawn.position.z});
        if (!ground)
        {
            return DropSpawnResult::Rejected;
        }
        if (_drops.Reserve(1, _dropReservation))
        {
            auto* drop = _drops.Reserved(_dropReservation[0]);
            drop->Reset();
            drop->position = ground->position;
            drop->kind = spawn.kind;
            drop->definitionId = spawn.definitionId;
            drop->value = spawn.value;
            drop->eligibleTick = eligibleTick;
            if (_drops.Commit(_dropReservation))
            {
                if (_definitions)
                {
                    generated += spawn.value;
                }
                return DropSpawnResult::Spawned;
            }
            drop->Reset();
            _drops.Cancel(_dropReservation);
            return DropSpawnResult::Rejected;
        }
        bool merged = false;
        _drops.ForEachActive(
            [&](Engine::PoolHandle, Drop& drop)
            {
                const double dx = static_cast<double>(drop.position.x) - ground->position.x;
                const double dz = static_cast<double>(drop.position.z) - ground->position.z;
                if (!merged && !drop.claimed && drop.kind == spawn.kind && drop.definitionId == spawn.definitionId &&
                    dx * dx + dz * dz <= 0.75 * 0.75 &&
                    spawn.value <= std::numeric_limits<std::uint32_t>::max() - drop.value)
                {
                    drop.value += spawn.value;
                    merged = true;
                }
            });
        if (merged)
        {
            if (_definitions)
            {
                generated += spawn.value;
            }
            ++_dropMerges;
            return DropSpawnResult::Merged;
        }
        auto& ledger = spawn.kind == DropKind::Experience ? _experienceLedger : _currencyLedger;
        if (spawn.value > std::numeric_limits<std::uint64_t>::max() - ledger)
        {
            return DropSpawnResult::Rejected;
        }
        ledger += spawn.value;
        if (_definitions)
        {
            generated += spawn.value;
        }
        ++_dropLedgerAdds;
        return DropSpawnResult::Ledger;
    }

    Enemy* LastFieldRun::TryEnemy(const EntityRef& ref) noexcept
    {
        return ref.kind == EntityKind::Enemy && ref.runEpoch == _runEpoch && _initialized && ref.handle
            ? _enemies.TryGet(*ref.handle) : nullptr;
    }

    Projectile* LastFieldRun::TryProjectile(const EntityRef& ref) noexcept
    {
        return ref.kind == EntityKind::Projectile && ref.runEpoch == _runEpoch && _initialized && ref.handle
            ? _projectiles.TryGet(*ref.handle) : nullptr;
    }

    Drop* LastFieldRun::TryDrop(const EntityRef& ref) noexcept
    {
        return ref.kind == EntityKind::Drop && ref.runEpoch == _runEpoch && _initialized && ref.handle
            ? _drops.TryGet(*ref.handle) : nullptr;
    }

    bool LastFieldRun::RequestRelease(const EntityRef& ref)
    {
        if (!IsValid(ref) || !ref.handle || (_boss && ref == *_boss && _wave.phase == WavePhase::Combat))
        {
            return false;
        }
        switch (ref.kind)
        {
        case EntityKind::Enemy: return _enemies.RequestRelease(*ref.handle);
        case EntityKind::Projectile: return _projectiles.RequestRelease(*ref.handle);
        case EntityKind::Drop:
        {
            auto* drop = _drops.TryGet(*ref.handle);
            if (!_drops.RequestRelease(*ref.handle))
            {
                return false;
            }
            if (_definitions && drop && !drop->claimed)
            {
                auto& discarded = drop->kind == DropKind::Experience ? _rewards.discardedXp : _rewards.discardedCurrency;
                discarded += drop->value;
                drop->claimed = true;
            }
            return true;
        }
        default: return false;
        }
    }

    void LastFieldRun::Tick(float dt, Engine::Vector2 movement)
    {
        _queryGridReady = false;
        if (!_initialized || !std::isfinite(dt) || dt < 0 ||
            !std::isfinite(movement.x) || !std::isfinite(movement.y) || _tick == std::numeric_limits<std::uint64_t>::max())
        {
            throw std::runtime_error("Run tick requires initialized pools, finite nonnegative time and a live tick counter");
        }
        ++_tick;
        if (_definitions)
        {
            TickGame(dt, movement);
            return;
        }
        MoveActors(dt, movement);
        if (!RebuildGrid())
        {
            throw std::runtime_error("Run grid rebuild failed");
        }
        if (!_projectiles.ForEachActive([&](Engine::PoolHandle handle, Projectile& projectile)
            {
                if (projectile.eligibleTick <= _tick)
                {
                    StepProjectile(handle, projectile, dt);
                }
            }))
        {
            throw std::runtime_error("Run projectile iteration failed");
        }
        FlushReturns();
        SortHits();
    }

    bool LastFieldRun::Clear()
    {
        _queryGridReady = false;
        if (!_initialized)
        {
            return false;
        }
        const bool cleared = _enemies.Clear([](Engine::PoolHandle, Enemy& value) noexcept { value.Reset(); }) &&
            _projectiles.Clear([](Engine::PoolHandle, Projectile& value) noexcept { value.Reset(); }) &&
            _drops.Clear([](Engine::PoolHandle, Drop& value) noexcept { value.Reset(); });
        if (cleared)
        {
            _experienceLedger = 0;
            _currencyLedger = 0;
            _hits.clear();
            _grid.Clear();
            _minimumAttackId = _nextAttackId;
            _definitions.reset();
            _weaponSlots.clear();
            _spareWeaponSlots.clear();
            _progression.reset();
            _playerStats = {};
            _observedLevel = 1;
            _damageRequests.clear();
            _appliedThisTick.clear();
            _hitProjectiles.clear();
            _pendingProjectileHandles.clear();
            _pendingShots.clear();
            _events.clear();
            _playerState = {};
            _rewards = {};
            _wave = {};
            _boss.reset();
            _inGameTick = false;
        }
        return cleared;
    }

    bool LastFieldRun::BuildTrialWave()
    {
        if (!Clear())
        {
            return false;
        }
        for (std::uint32_t i = 0; i < 10; ++i)
        {
            SpawnEnemy({{-4.5f + static_cast<float>(i % 4) * 3, 0, 3 + static_cast<float>(i / 4) * 2}, 1, 10});
        }
        const auto enemyStats = _enemies.GetStats();
        if (enemyStats.active != std::min(std::uint32_t{10}, enemyStats.capacity))
        {
            return false;
        }
        std::array<ProjectileSpawn, 4> batch;
        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            const auto ground = _terrain.Sample({-3 + static_cast<float>(i) * 2, -3});
            if (!ground)
            {
                return false;
            }
            batch[i] = {{ground->position.x, ground->position.y + 0.25f, ground->position.z}, {},
                {EntityKind::Player, {}, _runEpoch}, 1, 1, 5};
        }
        if (!SpawnProjectiles(batch))
        {
            return false;
        }
        for (std::uint32_t i = 0; i < 10; ++i)
        {
            const float z = i < 8 ? -4.5f - static_cast<float>(i / 4) * 1.5f : -4.5f;
            if (SpawnDrop({{-4.5f + static_cast<float>(i % 4) * 3, 0, z}, DropKind::Experience, 1, 1}) == DropSpawnResult::Rejected)
            {
                return false;
            }
        }
        return CheckInvariants();
    }

    bool LastFieldRun::SetPlayerPosition(Engine::Vector3 position)
    {
        if (!_initialized || !Finite(position) || position.x < _playBounds.min.x || position.x > _playBounds.max.x ||
            position.z < _playBounds.min.y || position.z > _playBounds.max.y)
        {
            return false;
        }
        const auto surface = _terrain.Sample({position.x, position.z});
        if (!surface || surface->normal.y < _minimumNormalY ||
            OverlapsEnvironment({surface->position, _player.radius, _player.height}))
        {
            return false;
        }
        _player.base = surface->position;
        return true;
    }

    Engine::Cylinder LastFieldRun::PlayerCollider() const noexcept
    {
        return _player;
    }

    Engine::Vector2 LastFieldRun::PlayerFacing() const noexcept
    {
        return _playerFacing;
    }

    bool LastFieldRun::RebuildGrid()
    {
        _queryGridReady = false;
        _grid.Clear();
        bool valid = true;
        const bool visited = _enemies.ForEachActive([&](Engine::PoolHandle handle, const Enemy& enemy)
        {
            if (Targetable(enemy, _tick))
            {
                valid = _grid.Insert(Ref(EntityKind::Enemy, handle), enemy.position, enemy.radius) && valid;
            }
        });
        const bool ready = visited && valid;
        _queryGridReady = _inGameTick && ready;
        return ready;
    }

    bool LastFieldRun::GatherCircle(Engine::Vector3 center, float range, bool useGrid)
    {
        if (useGrid)
        {
            if ((!_inGameTick || !_queryGridReady) && !RebuildGrid())
            {
                return false;
            }
            return _grid.QueryCircle(center, range, _candidates);
        }
        _candidates.clear();
        bool valid = true;
        const bool visited = _enemies.ForEachActive([&](Engine::PoolHandle handle, const Enemy& enemy)
        {
            if (Targetable(enemy, _tick))
            {
                if (!Finite(enemy.position) || !std::isfinite(enemy.radius) || enemy.radius < 0 ||
                    enemy.position.x < _gridBounds.min.x || enemy.position.x > _gridBounds.max.x ||
                    enemy.position.z < _gridBounds.min.y || enemy.position.z > _gridBounds.max.y)
                {
                    valid = false;
                    return;
                }
                _candidates.push_back(Ref(EntityKind::Enemy, handle));
            }
        });
        return visited && valid;
    }

    std::optional<EntityRef> LastFieldRun::FindNearest(Engine::Vector3 origin, float range, float height, bool useGrid)
    {
        if (!_initialized || !Engine::IsValid({origin, range, height}))
        {
            return {};
        }
        if (!GatherCircle(origin, range, useGrid))
        {
            throw std::runtime_error("Run target candidate query failed");
        }
        std::optional<EntityRef> result;
        double best = std::numeric_limits<double>::infinity();
        for (const auto& ref : _candidates)
        {
            const auto* enemy = TryEnemy(ref);
            if (!enemy || !Targetable(*enemy, _tick) ||
                static_cast<double>(enemy->position.y) + enemy->height < origin.y ||
                static_cast<double>(origin.y) + height < enemy->position.y)
            {
                continue;
            }
            const double distance = Engine::DistanceSquaredXZ(origin, enemy->position);
            if (distance <= static_cast<double>(range) * range &&
                (distance < best || (distance == best && (!result || HandleLess(ref, *result)))))
            {
                best = distance;
                result = ref;
            }
        }
        return result;
    }

    bool LastFieldRun::BeginMelee(MeleeAttack& attack, const EntityRef& source)
    {
        if (!IsValid(source) || attack.hitTargets.capacity() < _enemies.GetStats().capacity ||
            _nextAttackId == std::numeric_limits<std::uint64_t>::max() ||
            (_definitions && (_wave.phase != WavePhase::Combat || _playerState.dead)))
        {
            return false;
        }
        attack.id = _nextAttackId++;
        attack.runEpoch = _runEpoch;
        attack.source = source;
        attack.hitTargets.clear();
        attack.attackPower = 1;
        attack.knockback = 0;
        attack.slotIndex = 0;
        return true;
    }

    bool LastFieldRun::TestMelee(MeleeAttack& attack, Engine::Vector3 origin, Engine::Vector2 forward,
        float range, float halfAngleRadians, float height, bool useGrid)
    {
        if (!IsValid(attack.source) || attack.runEpoch != _runEpoch || attack.id < _minimumAttackId ||
            attack.id >= _nextAttackId || !Engine::IsValid({origin, range, height}) ||
            !std::isfinite(forward.x) || !std::isfinite(forward.y) || (forward.x == 0 && forward.y == 0) ||
            !std::isfinite(halfAngleRadians) || halfAngleRadians < 0 || halfAngleRadians > DirectX::XM_PI ||
            !std::isfinite(attack.attackPower) || attack.attackPower < 0 ||
            !std::isfinite(attack.knockback) || attack.knockback < 0 ||
            (_definitions && (_wave.phase != WavePhase::Combat || _playerState.dead)) ||
            !GatherCircle(origin, range, useGrid))
        {
            return false;
        }
        _newMeleeHits.clear();
        for (const auto& ref : _candidates)
        {
            const auto* enemy = TryEnemy(ref);
            if (!enemy || !Targetable(*enemy, _tick) || ref == attack.source ||
                std::find(attack.hitTargets.begin(), attack.hitTargets.end(), ref) != attack.hitTargets.end() ||
                !Engine::IntersectsSector({enemy->position, enemy->radius, enemy->height}, origin, forward, range, halfAngleRadians, height))
            {
                continue;
            }
            _newMeleeHits.push_back({HitKind::Melee, _tick, attack.id, attack.source, {}, ref, enemy->position, 0,
                attack.attackPower, attack.knockback, attack.slotIndex,
                {static_cast<float>(forward.x / std::hypot(static_cast<double>(forward.x), forward.y)),
                 static_cast<float>(forward.y / std::hypot(static_cast<double>(forward.x), forward.y))}});
        }
        if (_newMeleeHits.size() > attack.hitTargets.capacity() - attack.hitTargets.size() ||
            _newMeleeHits.size() > _hits.capacity() - _hits.size() ||
            (_definitions && _newMeleeHits.size() > _damageRequests.capacity() - _damageRequests.size()))
        {
            return false;
        }
        std::sort(_newMeleeHits.begin(), _newMeleeHits.end(), [](const HitEvent& a, const HitEvent& b)
        {
            return HandleLess(*a.target, *b.target);
        });
        for (const auto& hit : _newMeleeHits)
        {
            attack.hitTargets.push_back(*hit.target);
            RecordHit(hit);
        }
        SortHits();
        return true;
    }

    bool LastFieldRun::OverlapsEnvironment(const Engine::Cylinder& body) const noexcept
    {
        return std::any_of(_staticCylinders.begin(), _staticCylinders.end(),
                   [&](const auto& shape) { return Engine::Overlaps(body, shape); }) ||
               std::any_of(_staticBoxes.begin(), _staticBoxes.end(),
                   [&](const auto& shape) { return Engine::Overlaps(body, shape); });
    }

    Engine::Vector3 LastFieldRun::MoveBody(Engine::Cylinder body, Engine::Vector2 displacement,
        const std::optional<EntityRef>& ignored, bool collideWithPlayer)
    {
        if (!Engine::IsValid(body) || !std::isfinite(displacement.x) || !std::isfinite(displacement.y))
        {
            return body.base;
        }
        const auto terrainDesc = _terrain.Description();
        if (terrainDesc.width < 2 || terrainDesc.height < 2 || terrainDesc.width > 1025 || terrainDesc.height > 1025 ||
            !std::isfinite(terrainDesc.cellSize) || terrainDesc.cellSize <= 0)
        {
            return body.base;
        }
        const auto coordinate = [&](float origin, std::uint32_t index)
        {
            return static_cast<float>(static_cast<double>(origin) + static_cast<double>(terrainDesc.cellSize) * index);
        };
        const auto cell = [&](float origin, std::uint32_t count, double value)
        {
            std::uint32_t low = 0;
            std::uint32_t high = count - 1;
            while (low + 1 < high)
            {
                const std::uint32_t middle = low + (high - low) / 2;
                if (coordinate(origin, middle) <= value)
                {
                    low = middle;
                }
                else
                {
                    high = middle;
                }
            }
            return std::min(low, count - 2);
        };
        auto current = body.base;
        Engine::Vector3 destination{std::clamp(current.x + displacement.x, _playBounds.min.x, _playBounds.max.x),
            current.y, std::clamp(current.z + displacement.y, _playBounds.min.y, _playBounds.max.y)};
        for (int pass = 0; pass < 2; ++pass)
        {
            const auto surface = _terrain.Sample({destination.x, destination.z});
            if (!surface || surface->normal.y < _minimumNormalY)
            {
                break;
            }
            destination.y = surface->position.y;
            const double length = std::sqrt(Engine::DistanceSquaredXZ(current, destination));
            const double deltaX = static_cast<double>(destination.x) - current.x;
            const double deltaZ = static_cast<double>(destination.z) - current.z;
            // Terrain vertices are rounded to float after double origin + index * spacing.
            // Split only at these actual cell boundaries, then at the B-C diagonal.
            std::array<double, 2052> crossings;
            std::size_t crossingCount = 2;
            crossings[0] = 0;
            crossings[1] = 1;
            const auto addAxisCrossings = [&](float origin, std::uint32_t count, double start, double delta)
            {
                if (delta == 0)
                {
                    return;
                }
                for (std::uint32_t index = 1; index + 1 < count; ++index)
                {
                    const double t = (static_cast<double>(coordinate(origin, index)) - start) / delta;
                    if (t > 0 && t < 1)
                    {
                        crossings[crossingCount++] = t;
                    }
                }
            };
            addAxisCrossings(terrainDesc.origin.x, terrainDesc.width, current.x, deltaX);
            addAxisCrossings(terrainDesc.origin.z, terrainDesc.height, current.z, deltaZ);
            std::sort(crossings.begin(), crossings.begin() + crossingCount);
            crossingCount = static_cast<std::size_t>(std::unique(crossings.begin(), crossings.begin() + crossingCount) - crossings.begin());
            const auto walkableAt = [&](double t)
            {
                const auto point = _terrain.Sample({static_cast<float>(std::fma(deltaX, t, current.x)),
                    static_cast<float>(std::fma(deltaZ, t, current.z))});
                return point && Finite(point->position) && Finite(point->normal) && point->normal.y >= _minimumNormalY;
            };
            const auto walkableFace = [&](std::uint32_t column, std::uint32_t row, bool firstTriangle)
            {
                const float x0 = coordinate(terrainDesc.origin.x, column);
                const float x1 = coordinate(terrainDesc.origin.x, column + 1);
                const float z0 = coordinate(terrainDesc.origin.z, row);
                const float z1 = coordinate(terrainDesc.origin.z, row + 1);
                const double width = static_cast<double>(x1) - x0;
                const double depth = static_cast<double>(z1) - z0;
                if (width <= 0 || depth <= 0)
                {
                    return false;
                }
                const auto b = _terrain.Sample({x1, z0});
                const auto c = _terrain.Sample({x0, z1});
                const auto corner = _terrain.Sample(firstTriangle ? Engine::Vector2{x0, z0} : Engine::Vector2{x1, z1});
                if (!b || !c || !corner)
                {
                    return false;
                }
                const double slopeX = firstTriangle ? (static_cast<double>(b->position.y) - corner->position.y) / width
                    : (static_cast<double>(corner->position.y) - c->position.y) / width;
                const double slopeZ = firstTriangle ? (static_cast<double>(c->position.y) - corner->position.y) / depth
                    : (static_cast<double>(corner->position.y) - b->position.y) / depth;
                const float normalY = static_cast<float>(1 / std::sqrt(1 + slopeX * slopeX + slopeZ * slopeZ));
                return std::isfinite(normalY) && normalY >= _minimumNormalY;
            };
            const auto walkableTriangle = [&](double first, double last, std::uint32_t column, std::uint32_t row)
            {
                const double middle = first + (last - first) * 0.5;
                if (!walkableAt(middle) || !walkableAt(last))
                {
                    return false;
                }
                if (!(middle > first && middle < last))
                {
                    // Adjacent representable t values need not have a representable midpoint.
                    // Check both faces of every endpoint cell instead of cancelling flat motion.
                    const auto firstColumn = cell(terrainDesc.origin.x, terrainDesc.width, std::fma(deltaX, first, current.x));
                    const auto lastColumn = cell(terrainDesc.origin.x, terrainDesc.width, std::fma(deltaX, last, current.x));
                    const auto firstRow = cell(terrainDesc.origin.z, terrainDesc.height, std::fma(deltaZ, first, current.z));
                    const auto lastRow = cell(terrainDesc.origin.z, terrainDesc.height, std::fma(deltaZ, last, current.z));
                    for (std::uint32_t z = std::min(firstRow, lastRow); z <= std::max(firstRow, lastRow); ++z)
                    {
                        for (std::uint32_t x = std::min(firstColumn, lastColumn); x <= std::max(firstColumn, lastColumn); ++x)
                        {
                            if (!walkableFace(x, z, true) || !walkableFace(x, z, false))
                            {
                                return false;
                            }
                        }
                    }
                    return true;
                }
                const double x0 = coordinate(terrainDesc.origin.x, column);
                const double x1 = coordinate(terrainDesc.origin.x, column + 1);
                const double z0 = coordinate(terrainDesc.origin.z, row);
                const double z1 = coordinate(terrainDesc.origin.z, row + 1);
                const double u = (std::fma(deltaX, middle, current.x) - x0) / (x1 - x0);
                const double v = (std::fma(deltaZ, middle, current.z) - z0) / (z1 - z0);
                // Use stored vertex heights even if the float Sample point rounds to an edge.
                return walkableFace(column, row, u + v <= 1);
            };
            bool walkable = walkableAt(0);
            for (std::size_t interval = 1; interval < crossingCount && walkable; ++interval)
            {
                const double first = crossings[interval - 1];
                const double last = crossings[interval];
                const double middle = first + (last - first) * 0.5;
                const auto column = cell(terrainDesc.origin.x, terrainDesc.width, std::fma(deltaX, middle, current.x));
                const auto row = cell(terrainDesc.origin.z, terrainDesc.height, std::fma(deltaZ, middle, current.z));
                const double x0 = coordinate(terrainDesc.origin.x, column);
                const double x1 = coordinate(terrainDesc.origin.x, column + 1);
                const double z0 = coordinate(terrainDesc.origin.z, row);
                const double z1 = coordinate(terrainDesc.origin.z, row + 1);
                const double width = x1 - x0;
                const double depth = z1 - z0;
                if (width <= 0 || depth <= 0)
                {
                    walkable = false;
                    break;
                }
                const auto diagonalSide = [&](double t)
                {
                    // The interval is already clipped to this cell. Retain exact closed-edge
                    // topology when division/FMA rounds a grid-boundary point just outside it.
                    const double u = std::clamp((std::fma(deltaX, t, current.x) - x0) / width, 0.0, 1.0);
                    const double v = std::clamp((std::fma(deltaZ, t, current.z) - z0) / depth, 0.0, 1.0);
                    return u + v - 1;
                };
                const double firstSide = diagonalSide(first);
                const double lastSide = diagonalSide(last);
                const bool crossesDiagonal = (firstSide < 0 && lastSide > 0) || (firstSide > 0 && lastSide < 0);
                const double diagonal = crossesDiagonal
                    ? first + (last - first) * (std::abs(firstSide) / (std::abs(firstSide) + std::abs(lastSide))) : first;
                if (crossesDiagonal && diagonal > first && diagonal < last)
                {
                    walkable = walkableTriangle(first, diagonal, column, row)
                        && walkableTriangle(diagonal, last, column, row);
                }
                else
                {
                    walkable = (!crossesDiagonal || (walkableFace(column, row, true) && walkableFace(column, row, false)))
                        && walkableTriangle(first, last, column, row);
                }
            }
            if (!walkable)
            {
                break;
            }
            std::optional<Engine::GeometryHit> first;
            const auto consider = [&](const auto& target)
            {
                const auto hit = Engine::SweepCylinder({current, destination}, body.radius, body.height, target);
                if (!hit)
                {
                    return;
                }
                const float direction = (destination.x - current.x) * hit->normal.x +
                    (destination.z - current.z) * hit->normal.z;
                if (hit->fraction == 0 && direction >= 0)
                {
                    return;
                }
                if (!first || hit->fraction < first->fraction)
                {
                    first = hit;
                }
            };
            if (collideWithPlayer)
            {
                consider(_player);
            }
            for (const auto& entry : _bodies)
            {
                if (!ignored || entry.ref != *ignored)
                {
                    consider(entry.body);
                }
            }
            for (const auto& shape : _staticCylinders)
            {
                consider(shape);
            }
            for (const auto& shape : _staticBoxes)
            {
                consider(shape);
            }
            if (!first)
            {
                return destination;
            }
            const float fraction = static_cast<float>(std::max(0.0, first->fraction - (length > 0 ? 0.0001 / length : 0)));
            current.x += (destination.x - current.x) * fraction;
            current.z += (destination.z - current.z) * fraction;
            const auto stopped = _terrain.Sample({current.x, current.z});
            if (!stopped)
            {
                return body.base;
            }
            current.y = stopped->position.y;
            const float dx = destination.x - current.x;
            const float dz = destination.z - current.z;
            const float into = std::min(0.0f, dx * first->normal.x + dz * first->normal.z);
            destination = {std::clamp(current.x + dx - into * first->normal.x, _playBounds.min.x, _playBounds.max.x),
                current.y, std::clamp(current.z + dz - into * first->normal.z, _playBounds.min.y, _playBounds.max.y)};
        }
        return current;
    }

    void LastFieldRun::MoveActors(float dt, Engine::Vector2 movement)
    {
        _bodies.clear();
        if (!_enemies.ForEachActive([&](Engine::PoolHandle handle, const Enemy& enemy)
            {
                if (Targetable(enemy, _tick))
                {
                    _bodies.push_back({Ref(EntityKind::Enemy, handle), {enemy.position, enemy.radius, enemy.height}});
                }
            }))
        {
            throw std::runtime_error("Run movement snapshot failed");
        }
        std::sort(_bodies.begin(), _bodies.end(), [](const BodyEntry& a, const BodyEntry& b)
        {
            return HandleLess(a.ref, b.ref);
        });
        const double length = std::hypot(static_cast<double>(movement.x), movement.y);
        const double divisor = std::max(1.0, length);
        const float speed = _definitions ? _playerStats.speed : 5;
        const Engine::Vector2 delta{static_cast<float>((movement.x / divisor * speed + _playerState.impulse.x) * dt),
            static_cast<float>((movement.y / divisor * speed + _playerState.impulse.z) * dt)};
        _player.base = MoveBody(_player, delta, {}, false);
        const float decay = std::exp(-(_definitions ? _playerStats.knockbackDecay : 8) * dt);
        _playerState.impulse.x *= decay;
        _playerState.impulse.z *= decay;
        if (length > 0)
        {
            _playerFacing = {static_cast<float>(movement.x / length), static_cast<float>(movement.y / length)};
        }
        for (auto& entry : _bodies)
        {
            auto* enemy = TryEnemy(entry.ref);
            if (!enemy)
            {
                continue;
            }
            enemy->position = MoveBody(entry.body, {(enemy->velocity.x + enemy->impulse.x) * dt,
                (enemy->velocity.z + enemy->impulse.z) * dt}, entry.ref, true);
            enemy->impulse.x *= decay;
            enemy->impulse.z *= decay;
            entry.body.base = enemy->position;
            enemy->timer += dt;
            enemy->animationTime += dt;
        }
    }

    void LastFieldRun::StepProjectile(Engine::PoolHandle handle, Projectile& projectile, float dt)
    {
        if (projectile.ttl <= 0)
        {
            _projectiles.RequestRelease(handle);
            return;
        }
        const double seconds = std::min(static_cast<double>(dt), projectile.ttl);
        const Engine::Vector3 end{static_cast<float>(projectile.position.x + projectile.velocity.x * seconds),
            static_cast<float>(projectile.position.y + projectile.velocity.y * seconds),
            static_cast<float>(projectile.position.z + projectile.velocity.z * seconds)};
        if (!Finite(end) || !std::isfinite(projectile.radius) || projectile.radius < 0 ||
            !_grid.QuerySegment({projectile.position, end}, projectile.radius, _candidates))
        {
            throw std::runtime_error("Run projectile sweep is invalid or query capacity is insufficient");
        }
        std::optional<EntityRef> victim;
        float fraction = std::numeric_limits<float>::infinity();
        const Engine::Segment baseMotion{{projectile.position.x, projectile.position.y - projectile.radius, projectile.position.z},
            {end.x, end.y - projectile.radius, end.z}};
        for (const auto& ref : _candidates)
        {
            const auto* enemy = TryEnemy(ref);
            if (!enemy || !Targetable(*enemy, _tick) || ref == projectile.owner)
            {
                continue;
            }
            const auto hit = Engine::SweepCylinder(baseMotion, projectile.radius, projectile.radius * 2,
                {enemy->position, enemy->radius, enemy->height});
            if (hit && (hit->fraction < fraction || (hit->fraction == fraction && (!victim || HandleLess(ref, *victim)))))
            {
                victim = ref;
                fraction = hit->fraction;
            }
        }
        const auto terrain = _terrain.SweepSphere({projectile.position, end}, projectile.radius);
        const bool terrainFirst = terrain && terrain->fraction <= fraction;
        if (terrainFirst || victim)
        {
            if (_hits.size() == _hits.capacity())
            {
                throw std::runtime_error("Run hit queue exhausted; drain value snapshots before the next tick");
            }
            fraction = terrainFirst ? terrain->fraction : fraction;
            const Engine::Vector3 point{projectile.position.x + (end.x - projectile.position.x) * fraction,
                projectile.position.y + (end.y - projectile.position.y) * fraction,
                projectile.position.z + (end.z - projectile.position.z) * fraction};
            RecordHit({terrainFirst ? HitKind::Terrain : HitKind::Projectile, _tick, projectile.attackId,
                projectile.owner, Ref(EntityKind::Projectile, handle), terrainFirst ? std::nullopt : victim,
                terrainFirst ? terrain->position : point, fraction, projectile.attackPower, projectile.knockback,
                projectile.slotIndex});
            if (_definitions && !terrainFirst)
            {
                _hitProjectiles.push_back(handle);
            }
            else
            {
                _projectiles.RequestRelease(handle);
            }
            return;
        }
        projectile.position = end;
        projectile.ttl -= seconds;
        const auto terrainDesc = _terrain.Description();
        if (projectile.ttl <= 0 || end.x < terrainDesc.origin.x || end.z < terrainDesc.origin.z ||
            end.x > static_cast<float>(static_cast<double>(terrainDesc.origin.x) + (terrainDesc.width - 1) * static_cast<double>(terrainDesc.cellSize)) ||
            end.z > static_cast<float>(static_cast<double>(terrainDesc.origin.z) + (terrainDesc.height - 1) * static_cast<double>(terrainDesc.cellSize)))
        {
            _projectiles.RequestRelease(handle);
        }
    }

    void LastFieldRun::SortHits()
    {
        std::sort(_hits.begin(), _hits.end(), [](const HitEvent& a, const HitEvent& b)
        {
            const auto ap = a.projectile ? a.projectile->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
            const auto bp = b.projectile ? b.projectile->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
            const auto at = a.target ? a.target->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
            const auto bt = b.target ? b.target->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
            return std::tie(a.tick, a.attackId, ap.storageId, ap.index, ap.generation, at.storageId, at.index, at.generation) <
                std::tie(b.tick, b.attackId, bp.storageId, bp.index, bp.generation, bt.storageId, bt.index, bt.generation);
        });
    }

    std::span<const HitEvent> LastFieldRun::Hits() const noexcept
    {
        return _hits;
    }

    void LastFieldRun::ClearHits() noexcept
    {
        _hits.clear();
    }

    bool LastFieldRun::BuildCollisionTrial()
    {
        if (!Clear() || !SetPlayerPosition({0, 0, -3}))
        {
            return false;
        }
        for (const auto position : {Engine::Vector3{0, 0, 2}, Engine::Vector3{-3, 0, 3}, Engine::Vector3{3, 0, 3}})
        {
            if (!SpawnEnemy({position, 1, 10}))
            {
                return false;
            }
        }
        return true;
    }

    bool LastFieldRun::FireTrialProjectile()
    {
        const auto target = FindNearest(_player.base, 12, _player.height);
        const auto* enemy = target ? TryEnemy(*target) : nullptr;
        if (!enemy)
        {
            return false;
        }
        const double distance = std::sqrt(Engine::DistanceSquaredXZ(_player.base, enemy->position));
        if (distance == 0)
        {
            return false;
        }
        const ProjectileSpawn shot{{_player.base.x, _player.base.y + 0.8f, _player.base.z},
            {static_cast<float>((enemy->position.x - _player.base.x) / distance * 120), 0,
                static_cast<float>((enemy->position.z - _player.base.z) / distance * 120)},
            {EntityKind::Player, {}, _runEpoch}, 1, 1, 2};
        return SpawnProjectiles(std::span(&shot, 1));
    }

    bool LastFieldRun::ConfigureGame(std::shared_ptr<const GameDefinitions> definitions,
                                     std::span<const std::uint32_t> loadout, std::uint64_t seed)
    {
        std::string error;
        if (!_initialized || _definitions || !definitions || !ValidateGameDefinitions(*definitions, error) ||
            definitions->enemyCapacity > _enemies.GetStats().capacity ||
            definitions->projectileCapacity > _projectiles.GetStats().capacity ||
            definitions->dropCapacity > _drops.GetStats().capacity)
        {
            return false;
        }
        if (loadout.empty())
        {
            loadout = definitions->loadout;
        }
        if (OverlapsEnvironment({_player.base, definitions->player.radius, definitions->player.height}))
        {
            return false;
        }
        if (loadout.empty() || loadout.size() > definitions->slotLimit ||
            std::any_of(loadout.begin(), loadout.end(),
                        [&](std::uint32_t id)
                        {
                            return !definitions->FindWeapon(id);
                        }))
        {
            return false;
        }
        std::vector<WeaponSlot> slots;
        std::vector<WeaponSlot> spareSlots;
        std::unique_ptr<RunProgression> progression;
        const auto hitCapacity = static_cast<std::size_t>(_enemies.GetStats().capacity) * (definitions->slotLimit + 1) +
                                 _projectiles.GetStats().capacity + 64;
        try
        {
            if (definitions->schemaVersion >= 2)
            {
                progression = std::make_unique<RunProgression>();
                if (!progression->Initialize(definitions, _runEpoch, loadout, seed ^ 0xa0761d6478bd642fULL))
                {
                    return false;
                }
            }
            slots.reserve(definitions->slotLimit);
            spareSlots.reserve(definitions->slotLimit);
            for (const auto id : loadout)
            {
                WeaponSlot slot;
                slot.definitionId = id;
                slot.melee.hitTargets.reserve(_enemies.GetStats().capacity);
                slots.push_back(std::move(slot));
            }
            for (std::size_t index = loadout.size(); index < definitions->slotLimit; ++index)
            {
                WeaponSlot slot;
                slot.melee.hitTargets.reserve(_enemies.GetStats().capacity);
                spareSlots.push_back(std::move(slot));
            }
            _hits.reserve(hitCapacity);
            _damageRequests.reserve(hitCapacity);
            _appliedThisTick.reserve(hitCapacity);
            _hitProjectiles.reserve(_projectiles.GetStats().capacity);
            _pendingProjectileHandles.reserve(_projectiles.GetStats().capacity);
            _pendingShots.reserve(definitions->slotLimit);
            _events.reserve(hitCapacity * 2 + _drops.GetStats().capacity * 2 + _enemies.GetStats().capacity + 64);
        }
        catch (const std::exception&)
        {
            return false;
        }
        if (!Clear())
        {
            return false;
        }
        _definitions = std::move(definitions);
        _weaponSlots = std::move(slots);
        _spareWeaponSlots = std::move(spareSlots);
        _progression = std::move(progression);
        _playerStats = _definitions->player;
        _observedLevel = 1;
        _player.radius = _playerStats.radius;
        _player.height = _playerStats.height;
        _playerState.hp = _playerStats.maxHp;
        _wave.waveCount = static_cast<std::uint32_t>(_definitions->waves.size());
        _wave.remaining = _definitions->waves[0].duration;
        _randomState = seed == 0 ? 0x9e3779b97f4a7c15ULL : seed;
        _autoAttack = true;
        return true;
    }

    bool LastFieldRun::GameConfigured() const noexcept
    {
        return _definitions != nullptr;
    }
    const PlayerState& LastFieldRun::Player() const noexcept
    {
        return _playerState;
    }

    const PlayerDefinition& LastFieldRun::PlayerStats() const noexcept
    {
        return _playerStats;
    }

    PlayerViewTarget LastFieldRun::ViewTarget() const noexcept
    {
        PlayerViewTarget target;
        target.runEpoch = _runEpoch;
        target.valid = _initialized && !_playerState.dead;
        target.head = {_player.base.x, _player.base.y + _player.height * 0.9f, _player.base.z};
        target.torso = {_player.base.x, _player.base.y + _player.height * 0.55f, _player.base.z};
        target.pelvis = {_player.base.x, _player.base.y + _player.height * 0.3f, _player.base.z};
        return target;
    }

    ProgressionSnapshot LastFieldRun::Progression() const
    {
        if (_progression)
        {
            return _progression->Snapshot();
        }
        ProgressionSnapshot result;
        result.runEpoch = _runEpoch;
        result.acquiredXp = _rewards.acquiredXp;
        result.acquiredCurrency = _rewards.acquiredCurrency;
        result.balance = _rewards.acquiredCurrency;
        result.finalPlayer = _playerStats;
        for (const auto& slot : _weaponSlots)
        {
            result.loadout.push_back(slot.definitionId);
        }
        return result;
    }

    void LastFieldRun::SyncProgressionRewards()
    {
        if (!_progression)
        {
            return;
        }
        if (!_progression->SyncRewards(_rewards.acquiredXp, _rewards.acquiredCurrency))
        {
            throw std::runtime_error("Run progression reward synchronization failed");
        }
        const auto level =
            1u + static_cast<std::uint32_t>(std::upper_bound(_definitions->progression.cumulativeXp.begin(),
                                                             _definitions->progression.cumulativeXp.end(),
                                                             _rewards.acquiredXp) -
                                            _definitions->progression.cumulativeXp.begin());
        if (level > _observedLevel)
        {
            _observedLevel = level;
            GameEvent event;
            event.kind = GameEventKind::LevelQueued;
            event.definitionId = level;
            event.position = _player.base;
            event.xp = _rewards.acquiredXp;
            Emit(event);
        }
    }

    void LastFieldRun::ApplyProgression()
    {
        if (!_progression)
        {
            return;
        }
        _playerStats = _progression->FinalPlayer();
        _playerState.hp = std::min(_playerState.hp, _playerStats.maxHp);
        _player.radius = _playerStats.radius;
        _player.height = _playerStats.height;
        const auto loadout = _progression->Loadout();
        for (std::size_t index = 0; index < _weaponSlots.size(); ++index)
        {
            if (index >= loadout.size() || _weaponSlots[index].definitionId != loadout[index])
            {
                throw std::runtime_error("Run progression changed an existing weapon slot");
            }
        }
        while (_weaponSlots.size() < loadout.size())
        {
            if (_spareWeaponSlots.empty() || _weaponSlots.size() == _weaponSlots.capacity())
            {
                throw std::runtime_error("Run progression exhausted prepared weapon slots");
            }
            auto slot = std::move(_spareWeaponSlots.back());
            _spareWeaponSlots.pop_back();
            slot.definitionId = loadout[_weaponSlots.size()];
            _weaponSlots.push_back(std::move(slot));
        }
        if (_progression->Phase() == ProgressionPhase::Selection)
        {
            _wave.phase = WavePhase::Selection;
        }
        else if (_progression->Phase() == ProgressionPhase::Shop)
        {
            _wave.phase = WavePhase::Shop;
        }
        else if (_wave.phase == WavePhase::Selection || _wave.phase == WavePhase::Shop)
        {
            _wave.phase = WavePhase::Rest;
        }
    }

    ProgressionResult LastFieldRun::ChooseUpgrade(std::uint64_t epoch, std::uint64_t choiceId, std::uint32_t index)
    {
        if (!_progression || _events.size() == _events.capacity() ||
            _eventSequence == std::numeric_limits<std::uint64_t>::max())
        {
            return {false, ProgressionFailure::PreparationFailed};
        }
        const auto before = _progression->Snapshot();
        const auto result = _progression->Choose(epoch, choiceId, index);
        if (result.accepted)
        {
            ApplyProgression();
            GameEvent event;
            event.kind = GameEventKind::UpgradeChosen;
            event.definitionId = before.choices[index];
            event.position = _player.base;
            Emit(event);
        }
        return result;
    }

    ProgressionResult LastFieldRun::SkipUpgrade(std::uint64_t epoch, std::uint64_t choiceId)
    {
        if (!_progression)
        {
            return {false, ProgressionFailure::NotInitialized};
        }
        const auto result = _progression->SkipChoice(epoch, choiceId);
        if (result.accepted)
        {
            ApplyProgression();
        }
        return result;
    }

    ProgressionResult LastFieldRun::BuyOffer(std::uint64_t epoch, std::uint64_t revision, std::uint64_t offerId)
    {
        if (!_progression || _events.size() == _events.capacity() ||
            _eventSequence == std::numeric_limits<std::uint64_t>::max())
        {
            return {false, ProgressionFailure::PreparationFailed};
        }
        const auto before = _progression->Snapshot();
        const auto result = _progression->Buy(epoch, revision, offerId);
        if (result.accepted)
        {
            ApplyProgression();
            const auto offer = std::find_if(before.offers.begin(), before.offers.end(),
                                            [&](const ShopOffer& value)
                                            {
                                                return value.offerId == offerId;
                                            });
            GameEvent event;
            event.kind = GameEventKind::Purchased;
            event.definitionId = offer->definitionId;
            event.position = _player.base;
            event.currency = offer->price;
            Emit(event);
        }
        return result;
    }

    ProgressionResult LastFieldRun::RerollShop(std::uint64_t epoch, std::uint64_t revision)
    {
        if (!_progression || _events.size() == _events.capacity() ||
            _eventSequence == std::numeric_limits<std::uint64_t>::max())
        {
            return {false, ProgressionFailure::PreparationFailed};
        }
        const auto cost = _progression->Snapshot().currentRerollCost;
        const auto result = _progression->Reroll(epoch, revision);
        if (result.accepted)
        {
            GameEvent event;
            event.kind = GameEventKind::Rerolled;
            event.position = _player.base;
            event.currency = cost;
            Emit(event);
        }
        return result;
    }

    ProgressionResult LastFieldRun::LeaveShop(std::uint64_t epoch, std::uint64_t revision)
    {
        if (!_progression)
        {
            return {false, ProgressionFailure::NotInitialized};
        }
        const auto result = _progression->FinishShop(epoch, revision);
        if (result.accepted)
        {
            ApplyProgression();
        }
        return result;
    }
    std::span<const WeaponSlot> LastFieldRun::WeaponSlots() const noexcept
    {
        return _weaponSlots;
    }
    const WaveSnapshot& LastFieldRun::Wave() const noexcept
    {
        return _wave;
    }
    const RewardTotals& LastFieldRun::Rewards() const noexcept
    {
        return _rewards;
    }
    std::span<const GameEvent> LastFieldRun::Events() const noexcept
    {
        return _events;
    }
    void LastFieldRun::SetAutoAttackEnabled(bool enabled) noexcept
    {
        _autoAttack = enabled;
    }

    bool LastFieldRun::BeginWave()
    {
        if (!_definitions || _wave.phase != WavePhase::Ready || _wave.outcome != RunOutcome::Playing ||
            _playerState.dead)
        {
            return false;
        }
        _wave.phase = WavePhase::Combat;
        _wave.elapsed = 0;
        _wave.remaining = _definitions->waves[_wave.waveIndex].duration;
        _wave.spawnBudget = 0;
        const auto& definition = _definitions->waves[_wave.waveIndex];
        _wave.bossEnemyId = definition.bossEnemyId;
        _wave.bossHp = 0;
        _wave.bossMaxHp = definition.bossEnemyId == 0 ? 0 : _definitions->FindEnemy(definition.bossEnemyId)->hp;
        _wave.bossSpawned = false;
        _wave.bossDefeated = false;
        _boss.reset();
        _events.clear();
        GameEvent event;
        event.kind = GameEventKind::WaveStarted;
        event.definitionId = _definitions->waves[_wave.waveIndex].id;
        Emit(event);
        return true;
    }

    bool LastFieldRun::PrepareNextWave()
    {
        if (!_definitions || _wave.phase != WavePhase::Rest || _wave.outcome != RunOutcome::Playing ||
            _wave.waveIndex + 1 >= _definitions->waves.size() || _playerState.dead)
        {
            return false;
        }
        ++_wave.waveIndex;
        _wave.phase = WavePhase::Ready;
        _wave.elapsed = 0;
        _wave.remaining = _definitions->waves[_wave.waveIndex].duration;
        _wave.spawnBudget = 0;
        _wave.bossEnemyId = 0;
        _wave.bossHp = _wave.bossMaxHp = 0;
        _wave.bossSpawned = _wave.bossDefeated = false;
        _boss.reset();
        return true;
    }

    void LastFieldRun::FlushReturns()
    {
        if (!_enemies.FlushReleases(
                [](Engine::PoolHandle, Enemy& value) noexcept
                {
                    value.Reset();
                }) ||
            !_projectiles.FlushReleases(
                [](Engine::PoolHandle, Projectile& value) noexcept
                {
                    value.Reset();
                }) ||
            !_drops.FlushReleases(
                [](Engine::PoolHandle, Drop& value) noexcept
                {
                    value.Reset();
                }))
        {
            throw std::runtime_error("Run pool release boundary failed");
        }
    }

    void LastFieldRun::Emit(GameEvent event)
    {
        if (_events.size() == _events.capacity() || _eventSequence == std::numeric_limits<std::uint64_t>::max())
        {
            throw std::runtime_error("Run presentation event capacity or sequence exhausted");
        }
        event.runEpoch = _runEpoch;
        event.tick = _inGameTick ? _tick : _tick + 1;
        event.sequence = _eventSequence++;
        _events.push_back(event);
    }

    void LastFieldRun::RecordHit(const HitEvent& hit)
    {
        if (_hits.size() == _hits.capacity())
        {
            throw std::runtime_error("Run hit snapshot capacity exhausted");
        }
        _hits.push_back(hit);
        if (_definitions && hit.target && hit.kind != HitKind::Terrain)
        {
            if (_damageRequests.size() == _damageRequests.capacity())
            {
                throw std::runtime_error("Run damage queue capacity exhausted");
            }
            auto damage = hit;
            damage.tick = _inGameTick ? _tick : _tick + 1;
            _damageRequests.push_back(damage);
        }
    }

    void LastFieldRun::UpdateEnemyIntent(float dt)
    {
        if (!_enemies.ForEachActive(
                [&](Engine::PoolHandle, Enemy& enemy)
                {
                    if (!Targetable(enemy, _tick))
                    {
                        return;
                    }
                    const auto* definition = _definitions->FindEnemy(enemy.definitionId);
                    if (!definition)
                    {
                        throw std::runtime_error("Live Enemy has an unknown definition");
                    }
                    enemy.cooldown = std::max(0.0, enemy.cooldown - dt);
                    const double x = static_cast<double>(_player.base.x) - enemy.position.x;
                    const double z = static_cast<double>(_player.base.z) - enemy.position.z;
                    const double length = std::hypot(x, z);
                    if (length > 0)
                    {
                        enemy.velocity = {static_cast<float>(x / length * definition->speed), 0,
                                          static_cast<float>(z / length * definition->speed)};
                    }
                    else
                    {
                        enemy.velocity = {};
                    }
                }))
        {
            throw std::runtime_error("Run AI iteration failed");
        }
    }

    void LastFieldRun::GatherContactDamage()
    {
        for (const auto& body : _bodies)
        {
            auto* enemy = TryEnemy(body.ref);
            const auto* definition = enemy ? _definitions->FindEnemy(enemy->definitionId) : nullptr;
            if (!enemy || !definition || !Targetable(*enemy, _tick) || definition->contactDamage <= 0 ||
                enemy->cooldown > 0)
            {
                continue;
            }
            const double range = static_cast<double>(enemy->radius) + _player.radius + 0.03;
            if (Engine::DistanceSquaredXZ(enemy->position, _player.base) > range * range ||
                static_cast<double>(enemy->position.y) + enemy->height < _player.base.y ||
                static_cast<double>(_player.base.y) + _player.height < enemy->position.y)
            {
                continue;
            }
            if (_nextAttackId == std::numeric_limits<std::uint64_t>::max())
            {
                throw std::runtime_error("Run contact attack counter exhausted");
            }
            RecordHit({HitKind::Contact,
                       _tick,
                       _nextAttackId++,
                       body.ref,
                       {},
                       EntityRef{EntityKind::Player, {}, _runEpoch},
                       _player.base,
                       0,
                       definition->contactDamage,
                       definition->knockback});
            enemy->cooldown = definition->contactInterval;
            GameEvent event;
            event.kind = GameEventKind::EnemyAttacked;
            event.definitionId = enemy->definitionId;
            event.source = body.ref;
            event.target = EntityRef{EntityKind::Player, {}, _runEpoch};
            event.position = enemy->position;
            Emit(event);
        }
    }

    void LastFieldRun::ApplyDamage()
    {
        std::sort(_damageRequests.begin(), _damageRequests.end(),
                  [](const HitEvent& a, const HitEvent& b)
                  {
                      const auto ap =
                          a.projectile ? a.projectile->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
                      const auto bp =
                          b.projectile ? b.projectile->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
                      const auto at = a.target ? a.target->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
                      const auto bt = b.target ? b.target->handle.value_or(Engine::PoolHandle{}) : Engine::PoolHandle{};
                      return std::tie(a.tick, a.attackId, a.slotIndex, ap.storageId, ap.index, ap.generation,
                                      at.storageId, at.index,
                                      at.generation) < std::tie(b.tick, b.attackId, b.slotIndex, bp.storageId, bp.index,
                                                                bp.generation, bt.storageId, bt.index, bt.generation);
                  });
        for (const auto& hit : _damageRequests)
        {
            if (hit.tick > _tick || !hit.target || hit.target->runEpoch != _runEpoch ||
                hit.source.runEpoch != _runEpoch || !std::isfinite(hit.attackPower) || hit.attackPower < 0)
            {
                continue;
            }
            const bool duplicate =
                std::any_of(_appliedThisTick.begin(), _appliedThisTick.end(),
                            [&](const HitEvent& prior)
                            {
                                return hit.target == prior.target &&
                                       (hit.kind == HitKind::Projectile
                                            ? hit.projectile == prior.projectile && prior.kind == HitKind::Projectile
                                            : hit.kind == prior.kind && hit.source == prior.source &&
                                                  hit.attackId == prior.attackId);
                            });
            if (duplicate)
            {
                continue;
            }
            if (hit.kind == HitKind::Contact)
            {
                const auto* source = TryEnemy(hit.source);
                if (!source || !Targetable(*source, _tick))
                {
                    continue;
                }
            }
            Projectile* projectile = nullptr;
            if (hit.kind == HitKind::Projectile)
            {
                projectile = hit.projectile ? TryProjectile(*hit.projectile) : nullptr;
                if (!projectile || projectile->attackId != hit.attackId || projectile->eligibleTick > _tick ||
                    projectile->hitCount >= projectile->hits.size() ||
                    std::find(projectile->hits.begin(), projectile->hits.begin() + projectile->hitCount, *hit.target) !=
                        projectile->hits.begin() + projectile->hitCount)
                {
                    continue;
                }
            }
            Enemy* victim = hit.target->kind == EntityKind::Enemy ? TryEnemy(*hit.target) : nullptr;
            const bool player = hit.target->kind == EntityKind::Player && !hit.target->handle;
            if ((!player && (!victim || !Targetable(*victim, _tick))) ||
                (player && (_playerState.dead || _playerState.invulnerability > 0)))
            {
                continue;
            }
            const auto* enemyDefinition = victim ? _definitions->FindEnemy(victim->definitionId) : nullptr;
            const float armor = player ? _playerStats.armor : (enemyDefinition ? enemyDefinition->armor : 0);
            const float damage = std::max(1.0f, hit.attackPower - std::max(0.0f, armor));
            auto& hp = player ? _playerState.hp : victim->hp;
            hp = std::max(0.0f, hp - damage);
            Engine::Vector3 sourcePosition = _player.base;
            if (const auto* source = TryEnemy(hit.source))
            {
                sourcePosition = source->position;
            }
            const auto targetPosition = player ? _player.base : victim->position;
            double dx = static_cast<double>(targetPosition.x) - sourcePosition.x;
            double dz = static_cast<double>(targetPosition.z) - sourcePosition.z;
            if (projectile)
            {
                dx = projectile->velocity.x;
                dz = projectile->velocity.z;
            }
            else if (hit.kind == HitKind::Melee)
            {
                dx = hit.knockbackDirection.x;
                dz = hit.knockbackDirection.y;
            }
            const double length = std::hypot(dx, dz);
            if (length == 0)
            {
                dx = 1;
                dz = 0;
            }
            else
            {
                dx /= length;
                dz /= length;
            }
            auto& impulse = player ? _playerState.impulse : victim->impulse;
            impulse = {static_cast<float>(dx * hit.knockback), 0, static_cast<float>(dz * hit.knockback)};
            if (player)
            {
                _playerState.invulnerability = _playerStats.hitInvulnerability;
                if (hp == 0)
                {
                    _playerState.dead = true;
                    ++_rewards.playerDeaths;
                    GameEvent died;
                    died.kind = GameEventKind::PlayerDied;
                    died.position = targetPosition;
                    died.source = hit.source;
                    Emit(died);
                }
            }
            if (projectile)
            {
                projectile->hits[projectile->hitCount++] = *hit.target;
            }
            _appliedThisTick.push_back(hit);
            GameEvent event;
            event.kind = GameEventKind::Damaged;
            event.source = hit.source;
            event.target = hit.target;
            event.position = targetPosition;
            event.damage = damage;
            event.slotIndex = hit.slotIndex;
            Emit(event);
        }
        std::erase_if(_damageRequests,
                      [&](const HitEvent& hit)
                      {
                          return hit.tick <= _tick;
                      });
    }

    bool LastFieldRun::ReserveWeaponShots(std::uint32_t slotIndex, const WeaponDefinition& weapon,
                                          Engine::Vector2 direction)
    {
        if (_nextAttackId == std::numeric_limits<std::uint64_t>::max() ||
            _tick == std::numeric_limits<std::uint64_t>::max() ||
            !_projectiles.Reserve(weapon.projectileCount, _projectileReservation))
        {
            ++_projectileBatchFailures;
            return false;
        }
        const auto start = _pendingProjectileHandles.size();
        const auto* target = _weaponSlots[slotIndex].target ? TryEnemy(*_weaponSlots[slotIndex].target) : nullptr;
        const float startY = _player.base.y + _player.height * 0.55f;
        const double distance = target ? std::sqrt(Engine::DistanceSquaredXZ(_player.base, target->position)) : 0;
        const double y = target ? target->position.y + target->height * 0.55 - startY : 0;
        const double normalizer = std::hypot(distance, y);
        const double horizontal = normalizer > 0 ? distance / normalizer : 1;
        const double vertical = normalizer > 0 ? y / normalizer : 0;
        for (std::uint32_t index = 0; index < weapon.projectileCount; ++index)
        {
            const double angle =
                weapon.projectileCount == 1
                    ? 0
                    : (static_cast<double>(index) / (weapon.projectileCount - 1) - 0.5) * weapon.spreadRadians;
            const double x = direction.x * std::cos(angle) - direction.y * std::sin(angle);
            const double z = direction.x * std::sin(angle) + direction.y * std::cos(angle);
            const auto handle = _projectileReservation[index];
            auto* shot = _projectiles.Reserved(handle);
            shot->Reset();
            shot->position = {_player.base.x, startY, _player.base.z};
            shot->velocity = {static_cast<float>(x * horizontal * weapon.projectileSpeed),
                              static_cast<float>(vertical * weapon.projectileSpeed),
                              static_cast<float>(z * horizontal * weapon.projectileSpeed)};
            shot->owner = {EntityKind::Player, {}, _runEpoch};
            shot->definitionId = weapon.id;
            shot->attackPower = weapon.damage * _playerStats.damageMultiplier;
            shot->ttl = weapon.projectileLife;
            shot->eligibleTick = _tick + 1;
            shot->attackId = _nextAttackId;
            shot->radius = weapon.projectileRadius;
            shot->knockback = weapon.knockback;
            shot->slotIndex = slotIndex;
            _pendingProjectileHandles.push_back(handle);
        }
        ++_nextAttackId;
        _pendingShots.push_back({slotIndex, start, weapon.projectileCount});
        return true;
    }

    void LastFieldRun::CommitWeaponShots(bool allow)
    {
        for (const auto& group : _pendingShots)
        {
            const std::span<const Engine::PoolHandle> handles(_pendingProjectileHandles.data() + group.first,
                                                              group.count);
            if (!allow)
            {
                for (const auto handle : handles)
                {
                    _projectiles.Reserved(handle)->Reset();
                }
                if (!_projectiles.Cancel(handles))
                {
                    throw std::runtime_error("Run attack cancellation failed");
                }
                continue;
            }
            if (!_projectiles.Commit(handles))
            {
                throw std::runtime_error("Run attack publication failed");
            }
            auto& slot = _weaponSlots[group.slotIndex];
            const auto* weapon = _definitions->FindWeapon(slot.definitionId);
            slot.cooldown =
                std::max(_definitions->minimumCooldown, weapon->cooldown / _playerStats.attackSpeed);
            ++slot.fireCount;
            GameEvent event;
            event.kind = GameEventKind::Fired;
            event.definitionId = weapon->id;
            event.slotIndex = group.slotIndex;
            event.source = {EntityKind::Player, {}, _runEpoch};
            event.position = _player.base;
            Emit(event);
        }
        _pendingShots.clear();
        _pendingProjectileHandles.clear();
    }

    void LastFieldRun::AttackWeapons()
    {
        if (!_autoAttack || _playerState.dead)
        {
            return;
        }
        std::uint32_t approved = 0;
        for (std::uint32_t index = 0; index < _weaponSlots.size() && approved < _definitions->maxAttacksPerTick;
             ++index)
        {
            auto& slot = _weaponSlots[index];
            if (slot.cooldown > 0)
            {
                continue;
            }
            const auto* weapon = _definitions->FindWeapon(slot.definitionId);
            slot.target = FindNearest(_player.base, weapon->range, _player.height);
            const auto* enemy = slot.target ? TryEnemy(*slot.target) : nullptr;
            if (!enemy)
            {
                continue;
            }
            const double x = static_cast<double>(enemy->position.x) - _player.base.x;
            const double z = static_cast<double>(enemy->position.z) - _player.base.z;
            const double length = std::hypot(x, z);
            slot.direction = length > 0
                                 ? Engine::Vector2{static_cast<float>(x / length), static_cast<float>(z / length)}
                                 : _playerFacing;
            if (weapon->kind != WeaponKind::Melee)
            {
                if (ReserveWeaponShots(index, *weapon, slot.direction))
                {
                    ++approved;
                }
                continue;
            }
            if (!BeginMelee(slot.melee, {EntityKind::Player, {}, _runEpoch}))
            {
                continue;
            }
            slot.melee.attackPower = weapon->damage * _playerStats.damageMultiplier;
            slot.melee.knockback = weapon->knockback;
            slot.melee.slotIndex = index;
            if (!TestMelee(slot.melee, _player.base, slot.direction, weapon->range, weapon->halfAngleRadians,
                           _player.height))
            {
                throw std::runtime_error("Run automatic melee query failed");
            }
            ApplyDamage();
            slot.cooldown =
                std::max(_definitions->minimumCooldown, weapon->cooldown / _playerStats.attackSpeed);
            ++slot.fireCount;
            ++approved;
            GameEvent event;
            event.kind = GameEventKind::Fired;
            event.definitionId = weapon->id;
            event.slotIndex = index;
            event.source = {EntityKind::Player, {}, _runEpoch};
            event.position = _player.base;
            Emit(event);
        }
    }

    void LastFieldRun::CollectDrops(bool all)
    {
        if (_playerState.dead)
        {
            return;
        }
        const double radius = _playerStats.pickupRadius;
        if (!_drops.ForEachActive(
                [&](Engine::PoolHandle handle, Drop& drop)
                {
                    if (drop.claimed ||
                        (!all && (drop.eligibleTick > _tick ||
                                  Engine::DistanceSquaredXZ(drop.position, _player.base) > radius * radius)))
                    {
                        return;
                    }
                    auto& acquired =
                        drop.kind == DropKind::Experience ? _rewards.acquiredXp : _rewards.acquiredCurrency;
                    if (drop.value > std::numeric_limits<std::uint64_t>::max() - acquired)
                    {
                        throw std::runtime_error("Run acquired reward overflow");
                    }
                    drop.claimed = true;
                    acquired += drop.value;
                    GameEvent event;
                    event.kind = GameEventKind::PickedUp;
                    event.definitionId = drop.definitionId;
                    event.position = drop.position;
                    event.target = Ref(EntityKind::Drop, handle);
                    event.xp = drop.kind == DropKind::Experience ? drop.value : 0;
                    event.currency = drop.kind == DropKind::Currency ? drop.value : 0;
                    Emit(event);
                    _drops.RequestRelease(handle);
                }))
        {
            throw std::runtime_error("Run pickup iteration failed");
        }
        if (all)
        {
            if (_experienceLedger > std::numeric_limits<std::uint64_t>::max() - _rewards.acquiredXp ||
                _currencyLedger > std::numeric_limits<std::uint64_t>::max() - _rewards.acquiredCurrency)
            {
                throw std::runtime_error("Run settlement reward overflow");
            }
            if (_experienceLedger || _currencyLedger)
            {
                GameEvent event;
                event.kind = GameEventKind::PickedUp;
                event.position = _player.base;
                event.xp = _experienceLedger;
                event.currency = _currencyLedger;
                Emit(event);
            }
            _rewards.acquiredXp += _experienceLedger;
            _rewards.acquiredCurrency += _currencyLedger;
            _experienceLedger = 0;
            _currencyLedger = 0;
        }
    }

    void LastFieldRun::FinalizeDeaths(bool spawnDrops)
    {
        if (!_enemies.ForEachActive(
                [&](Engine::PoolHandle handle, Enemy& enemy)
                {
                    if (enemy.hp > 0 || enemy.deathFinalized)
                    {
                        return;
                    }
                    const auto* definition = _definitions->FindEnemy(enemy.definitionId);
                    if (!definition ||
                        definition->xp > std::numeric_limits<std::uint64_t>::max() - _rewards.generatedXp ||
                        definition->currency > std::numeric_limits<std::uint64_t>::max() - _rewards.generatedCurrency ||
                        definition->xp > std::numeric_limits<std::uint64_t>::max() - _experienceLedger ||
                        definition->currency > std::numeric_limits<std::uint64_t>::max() - _currencyLedger ||
                        _rewards.kills == std::numeric_limits<std::uint64_t>::max())
                    {
                        throw std::runtime_error("Run death reward preflight failed");
                    }
                    const auto reward = [&](DropKind kind, std::uint32_t value)
                    {
                        if (value == 0)
                        {
                            return;
                        }
                        if (spawnDrops && SpawnDrop({enemy.position, kind, 1, value}) != DropSpawnResult::Rejected)
                        {
                            return;
                        }
                        auto& ledger = kind == DropKind::Experience ? _experienceLedger : _currencyLedger;
                        auto& generated =
                            kind == DropKind::Experience ? _rewards.generatedXp : _rewards.generatedCurrency;
                        ledger += value;
                        generated += value;
                    };
                    reward(DropKind::Experience, definition->xp);
                    reward(DropKind::Currency, definition->currency);
                    enemy.rewardIssued = true;
                    enemy.deathFinalized = true;
                    if (_boss && *_boss == Ref(EntityKind::Enemy, handle))
                    {
                        _wave.bossHp = 0;
                        _wave.bossDefeated = true;
                    }
                    ++_rewards.kills;
                    GameEvent event;
                    event.kind = GameEventKind::EnemyDied;
                    event.definitionId = enemy.definitionId;
                    event.target = Ref(EntityKind::Enemy, handle);
                    event.position = enemy.position;
                    event.xp = definition->xp;
                    event.currency = definition->currency;
                    Emit(event);
                    _enemies.RequestRelease(handle);
                }))
        {
            throw std::runtime_error("Run death iteration failed");
        }
    }

    void LastFieldRun::FinishWave(bool defeated)
    {
        _wave.phase = WavePhase::Settlement;
        const auto beforeXp = _rewards.acquiredXp;
        const auto beforeCurrency = _rewards.acquiredCurrency;
        if (!defeated)
        {
            CollectDrops(true);
            ++_wave.settlements;
            GameEvent event;
            event.kind = GameEventKind::WaveSettled;
            event.definitionId = _definitions->waves[_wave.waveIndex].id;
            event.xp = _rewards.acquiredXp - beforeXp;
            event.currency = _rewards.acquiredCurrency - beforeCurrency;
            Emit(event);
        }
        else
        {
            _rewards.discardedXp += _experienceLedger;
            _rewards.discardedCurrency += _currencyLedger;
            _experienceLedger = 0;
            _currencyLedger = 0;
            if (!_drops.ForEachActive(
                    [&](Engine::PoolHandle handle, Drop& drop)
                    {
                        if (!drop.claimed)
                        {
                            auto& discarded =
                                drop.kind == DropKind::Experience ? _rewards.discardedXp : _rewards.discardedCurrency;
                            discarded += drop.value;
                            drop.claimed = true;
                        }
                        _drops.RequestRelease(handle);
                    }))
            {
                throw std::runtime_error("Defeat reward discard iteration failed");
            }
        }
        if (!_enemies.ForEachActive(
                [&](Engine::PoolHandle handle, Enemy&)
                {
                    _enemies.RequestRelease(handle);
                }) ||
            !_projectiles.ForEachActive(
                [&](Engine::PoolHandle handle, Projectile&)
                {
                    _projectiles.RequestRelease(handle);
                }))
        {
            throw std::runtime_error("Wave cleanup iteration failed");
        }
        _wave.spawnBudget = 0;
        for (auto& slot : _weaponSlots)
        {
            slot.target.reset();
        }
        _damageRequests.clear();
        _minimumAttackId = _nextAttackId;
        if (defeated || _wave.waveIndex + 1 == _definitions->waves.size())
        {
            _wave.outcome = defeated ? RunOutcome::Defeat : RunOutcome::Victory;
            _wave.phase = WavePhase::Complete;
            GameEvent event;
            event.kind = defeated ? GameEventKind::Defeat : GameEventKind::Victory;
            Emit(event);
        }
        else
        {
            _wave.phase = WavePhase::Rest;
            if (_progression)
            {
                SyncProgressionRewards();
                if (!_progression->BeginIntermission().accepted)
                {
                    throw std::runtime_error("Run intermission preparation failed");
                }
                ApplyProgression();
            }
        }
    }

    std::uint32_t LastFieldRun::RandomU32() noexcept
    {
        _randomState ^= _randomState >> 12;
        _randomState ^= _randomState << 25;
        _randomState ^= _randomState >> 27;
        return static_cast<std::uint32_t>((_randomState * 2685821657736338717ULL) >> 32);
    }

    std::optional<EntityRef> LastFieldRun::TrySpawnWaveEnemy(const EnemyDefinition& definition,
        const WaveDefinition& wave)
    {
        for (std::uint32_t attempt = 0; attempt < wave.maxSpawnAttempts; ++attempt)
        {
            ++_wave.spawnAttempts;
            const double angle = RandomU32() / 4294967296.0 * DirectX::XM_2PI;
            const double u = RandomU32() / 4294967296.0;
            const double radius =
                std::sqrt(static_cast<double>(wave.minSpawnDistance) * wave.minSpawnDistance +
                    u * (static_cast<double>(wave.maxSpawnDistance) * wave.maxSpawnDistance -
                        static_cast<double>(wave.minSpawnDistance) * wave.minSpawnDistance));
            Engine::Vector3 point{static_cast<float>(_player.base.x + std::cos(angle) * radius), 0,
                static_cast<float>(_player.base.z + std::sin(angle) * radius)};
            if (point.x < _playBounds.min.x || point.x > _playBounds.max.x || point.z < _playBounds.min.y ||
                point.z > _playBounds.max.y)
            {
                continue;
            }
            const auto ground = _terrain.Sample({point.x, point.z});
            if (!ground || ground->normal.y < _minimumNormalY)
            {
                continue;
            }
            point.y = ground->position.y;
            const Engine::Cylinder candidate{point, definition.radius, definition.height};
            bool clear = !Engine::Overlaps(candidate, _player);
            if (!_enemies.ForEachActive(
                    [&](Engine::PoolHandle, const Enemy& enemy)
                    {
                        clear = clear && !Engine::Overlaps(candidate, {enemy.position, enemy.radius, enemy.height});
                    }))
            {
                throw std::runtime_error("Wave spawn overlap query failed");
            }
            if (clear)
            {
                const auto spawned = SpawnEnemy({point, definition.id, definition.hp, definition.radius, definition.height});
                if (spawned)
                {
                    return spawned;
                }
            }
        }
        return {};
    }

    void LastFieldRun::SpawnWaveEnemies(float dt)
    {
        if (dt <= 0)
        {
            return;
        }
        const auto& wave = _definitions->waves[_wave.waveIndex];
        std::uint32_t spawned = 0;
        if (wave.bossEnemyId != 0 && !_wave.bossSpawned)
        {
            if (_enemies.GetStats().active >= wave.maxAlive)
            {
                return;
            }
            if (!TrySpawnWaveEnemy(*_definitions->FindEnemy(wave.bossEnemyId), wave))
            {
                ++_wave.spawnFailures;
                return;
            }
            ++spawned;
        }
        if (_wave.remaining <= 0)
        {
            _wave.spawnBudget = 0;
            return;
        }
        _wave.spawnBudget =
            std::min(static_cast<double>(_definitions->maxSpawnsPerTick), _wave.spawnBudget + dt * wave.spawnPerSecond);
        double weightSum = 0;
        for (const auto& weight : wave.enemyWeights)
        {
            weightSum += weight.weight;
        }
        for (; spawned < _definitions->maxSpawnsPerTick && _wave.spawnBudget >= 1; ++spawned)
        {
            if (_enemies.GetStats().active >= wave.maxAlive)
            {
                _wave.spawnBudget = std::min(1.0, _wave.spawnBudget);
                break;
            }
            const double choice = RandomU32() / 4294967296.0 * weightSum;
            double sum = 0;
            const EnemyDefinition* definition = nullptr;
            for (const auto& weight : wave.enemyWeights)
            {
                sum += weight.weight;
                if (choice < sum && weight.weight > 0)
                {
                    definition = _definitions->FindEnemy(weight.enemyId);
                    break;
                }
            }
            if (!definition)
            {
                throw std::runtime_error("Wave weight selection failed");
            }
            const bool accepted = TrySpawnWaveEnemy(*definition, wave).has_value();
            _wave.spawnBudget -= 1;
            if (!accepted)
            {
                ++_wave.spawnFailures;
                _wave.spawnBudget = std::min(1.0, _wave.spawnBudget);
                break;
            }
        }
    }

    void LastFieldRun::TickGame(float dt, Engine::Vector2 movement)
    {
        _queryGridReady = false;
        std::erase_if(_events, [&](const GameEvent& event) { return event.tick < _tick; });
        _hits.clear();
        if (_wave.phase != WavePhase::Combat || _wave.outcome != RunOutcome::Playing)
        {
            return;
        }
        _inGameTick = true;
        try
        {
            const auto& definition = _definitions->waves[_wave.waveIndex];
            const bool bossWave = definition.bossEnemyId != 0;
            const double exactSeconds = bossWave ? static_cast<double>(dt) : std::min(static_cast<double>(dt), _wave.remaining);
            const float seconds = static_cast<float>(exactSeconds);
            const bool expires = !bossWave && static_cast<double>(dt) >= _wave.remaining;
            _appliedThisTick.clear();
            _hitProjectiles.clear();
            _playerState.invulnerability = std::max(0.0, _playerState.invulnerability - seconds);
            for (auto& slot : _weaponSlots)
            {
                slot.cooldown = std::max(0.0, slot.cooldown - seconds);
            }
            UpdateEnemyIntent(seconds);
            MoveActors(seconds, movement);
            if (!RebuildGrid())
            {
                throw std::runtime_error("Combat grid rebuild failed");
            }
            if (!_projectiles.ForEachActive(
                    [&](Engine::PoolHandle handle, Projectile& projectile)
                    {
                        if (projectile.eligibleTick <= _tick)
                        {
                            StepProjectile(handle, projectile, seconds);
                        }
                    }))
            {
                throw std::runtime_error("Combat projectile iteration failed");
            }
            GatherContactDamage();
            ApplyDamage();
            if (!_playerState.dead && !expires)
            {
                AttackWeapons();
            }
            CollectDrops(false);
            FinalizeDeaths(!_playerState.dead && !expires);
            if (_boss && !_wave.bossDefeated)
            {
                const auto* boss = TryEnemy(*_boss);
                _wave.bossHp = boss ? std::max(0.0f, boss->hp) : 0;
            }
            for (const auto handle : _hitProjectiles)
            {
                _projectiles.RequestRelease(handle);
            }
            _wave.elapsed = bossWave ? _wave.elapsed + exactSeconds : std::min(definition.duration, _wave.elapsed + exactSeconds);
            _wave.remaining = std::max(0.0, definition.duration - _wave.elapsed);
            if (_playerState.dead || expires || (bossWave && _wave.bossDefeated))
            {
                FinishWave(_playerState.dead);
            }
            FlushReturns();
            const bool continues = _wave.phase == WavePhase::Combat && !_playerState.dead;
            CommitWeaponShots(continues);
            if (continues)
            {
                SpawnWaveEnemies(seconds);
            }
            SyncProgressionRewards();
            SortHits();
            if (!RewardConservation())
            {
                throw std::runtime_error("Run reward conservation failed");
            }
        }
        catch (...)
        {
            _queryGridReady = false;
            _inGameTick = false;
            throw;
        }
        _queryGridReady = false;
        _inGameTick = false;
    }

    bool LastFieldRun::RewardConservation() const noexcept
    {
        if (!_definitions)
        {
            return true;
        }
        std::uint64_t xp = _experienceLedger, currency = _currencyLedger;
        bool valid = true;
        const auto add = [&](std::uint64_t& sum, std::uint64_t amount)
        {
            if (amount > std::numeric_limits<std::uint64_t>::max() - sum)
            {
                valid = false;
            }
            else
            {
                sum += amount;
            }
        };
        if (!_drops.ForEachActive(
                [&](Engine::PoolHandle, const Drop& drop)
                {
                    if (!drop.claimed)
                    {
                        add(drop.kind == DropKind::Experience ? xp : currency, drop.value);
                    }
                }))
        {
            return false;
        }
        add(xp, _rewards.acquiredXp);
        add(xp, _rewards.discardedXp);
        add(currency, _rewards.acquiredCurrency);
        add(currency, _rewards.discardedCurrency);
        return valid && xp == _rewards.generatedXp && currency == _rewards.generatedCurrency;
    }

    bool LastFieldRun::CheckInvariants() const noexcept
    {
        return _initialized && _enemies.CheckInvariants() && _projectiles.CheckInvariants() && _drops.CheckInvariants() &&
            _grid.CheckInvariants() && _hits.size() <= _hits.capacity() &&
            _damageRequests.size() <= _damageRequests.capacity() && _events.size() <= _events.capacity() &&
            (!_definitions || RewardConservation());
    }

    std::uint64_t LastFieldRun::RunEpoch() const noexcept
    {
        return _runEpoch;
    }

    std::uint64_t LastFieldRun::TickIndex() const noexcept
    {
        return _tick;
    }
    std::uint64_t LastFieldRun::Ledger(DropKind kind) const noexcept
    {
        return kind == DropKind::Experience ? _experienceLedger : (kind == DropKind::Currency ? _currencyLedger : 0);
    }
    std::uint64_t LastFieldRun::ProjectileBatchFailures() const noexcept
    {
        return _projectileBatchFailures;
    }

    std::uint64_t LastFieldRun::DropMerges() const noexcept
    {
        return _dropMerges;
    }

    std::uint64_t LastFieldRun::DropLedgerAdds() const noexcept
    {
        return _dropLedgerAdds;
    }

    const Engine::ObjectPool<Enemy>& LastFieldRun::Enemies() const noexcept
    {
        return _enemies;
    }

    const Engine::ObjectPool<Projectile>& LastFieldRun::Projectiles() const noexcept
    {
        return _projectiles;
    }

    const Engine::ObjectPool<Drop>& LastFieldRun::Drops() const noexcept
    {
        return _drops;
    }
}
