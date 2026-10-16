#pragma once

#include "Entity/PoolHandle.h"
#include "Types/Math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Client
{
    enum class EntityKind
    {
        Player,
        Enemy,
        Projectile,
        Drop
    };

    struct EntityRef
    {
        EntityKind kind = EntityKind::Player;
        std::optional<Engine::PoolHandle> handle;
        std::uint64_t runEpoch = 0;

        bool operator==(const EntityRef&) const = default;
    };

    enum class DropKind
    {
        Experience,
        Currency
    };

    inline constexpr std::size_t kProjectileHitCapacity = 16;

    struct Enemy
    {
        Engine::Vector3 position = {};
        Engine::Vector3 velocity = {};
        std::optional<EntityRef> target;
        std::uint32_t definitionId = 0;
        float hp = 0;
        double timer = 0;
        double cooldown = 0;
        double animationTime = 0;
        std::uint64_t eligibleTick = 0;
        bool deathFinalized = false;
        bool rewardIssued = false;
        float radius = 0.35f;
        float height = 1.8f;
        Engine::Vector3 impulse = {};

        void Reset() noexcept
        {
            *this = Enemy{};
        }
    };

    struct Projectile
    {
        Engine::Vector3 position = {};
        Engine::Vector3 velocity = {};
        EntityRef owner;
        std::array<EntityRef, kProjectileHitCapacity> hits = {};
        std::uint32_t definitionId = 0;
        std::uint32_t hitCount = 0;
        float attackPower = 0;
        double ttl = 0;
        std::uint64_t eligibleTick = 0;
        std::uint64_t attackId = 0;
        float radius = 0.12f;
        float knockback = 0;
        std::uint32_t slotIndex = 0;

        void Reset() noexcept
        {
            *this = Projectile{};
        }
    };

    struct Drop
    {
        Engine::Vector3 position = {};
        DropKind kind = DropKind::Experience;
        std::uint32_t definitionId = 0;
        std::uint32_t value = 0;
        std::uint64_t eligibleTick = 0;
        bool claimed = false;

        void Reset() noexcept
        {
            *this = Drop{};
        }
    };

    enum class HitKind
    {
        Melee,
        Projectile,
        Terrain,
        Contact
    };

    struct HitEvent
    {
        HitKind kind = HitKind::Projectile;
        std::uint64_t tick = 0;
        std::uint64_t attackId = 0;
        EntityRef source;
        std::optional<EntityRef> projectile;
        std::optional<EntityRef> target;
        Engine::Vector3 position = {};
        float fraction = 0;
        float attackPower = 1;
        float knockback = 0;
        std::uint32_t slotIndex = 0;
        Engine::Vector2 knockbackDirection{1, 0};
    };

    struct MeleeAttack
    {
        std::uint64_t id = 0;
        std::uint64_t runEpoch = 0;
        EntityRef source;
        // Reserve enemy capacity before BeginMelee; the attack keeps its own victim history.
        std::vector<EntityRef> hitTargets;
        float attackPower = 1;
        float knockback = 0;
        std::uint32_t slotIndex = 0;
    };

    enum class RunOutcome
    {
        Playing,
        Victory,
        Defeat
    };

    enum class WavePhase
    {
        Ready,
        Combat,
        Settlement,
        Rest,
        Complete,
        Selection,
        Shop
    };

    struct PlayerState
    {
        float hp = 0;
        double invulnerability = 0;
        Engine::Vector3 impulse = {};
        bool dead = false;
    };

    struct WeaponSlot
    {
        std::uint32_t definitionId = 0;
        double cooldown = 0;
        std::optional<EntityRef> target;
        Engine::Vector2 direction{0, 1};
        std::uint64_t fireCount = 0;
        MeleeAttack melee;
    };

    struct RewardTotals
    {
        std::uint64_t generatedXp = 0, generatedCurrency = 0;
        std::uint64_t acquiredXp = 0, acquiredCurrency = 0;
        std::uint64_t discardedXp = 0, discardedCurrency = 0;
        std::uint64_t kills = 0, playerDeaths = 0;
    };

    struct WaveSnapshot
    {
        RunOutcome outcome = RunOutcome::Playing;
        WavePhase phase = WavePhase::Ready;
        std::uint32_t waveIndex = 0, waveCount = 0;
        double elapsed = 0, remaining = 0, spawnBudget = 0;
        std::uint64_t spawnAttempts = 0, spawnFailures = 0, settlements = 0;
    };

    enum class GameEventKind
    {
        Fired,
        Damaged,
        EnemyDied,
        PlayerDied,
        PickedUp,
        WaveStarted,
        WaveSettled,
        Victory,
        Defeat,
        LevelQueued,
        UpgradeChosen,
        Purchased,
        Rerolled,
        EnemyAttacked
    };

    struct GameEvent
    {
        GameEventKind kind = GameEventKind::Fired;
        std::uint64_t runEpoch = 0, tick = 0, sequence = 0;
        std::uint32_t definitionId = 0, slotIndex = 0;
        EntityRef source;
        std::optional<EntityRef> target;
        Engine::Vector3 position = {};
        float damage = 0;
        std::uint64_t xp = 0, currency = 0;
    };

    struct PlayerViewTarget
    {
        std::uint64_t runEpoch = 0;
        bool valid = false;
        Engine::Vector3 head = {}, torso = {}, pelvis = {};
    };
}
