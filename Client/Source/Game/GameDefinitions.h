#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Client
{
    enum class WeaponKind
    {
        Melee,
        SingleProjectile,
        Shotgun
    };

    struct PlayerDefinition
    {
        float maxHp = 100, speed = 5, armor = 0;
        float damageMultiplier = 1, attackSpeed = 1, pickupRadius = 2;
        float hitInvulnerability = 0.5f, knockbackDecay = 8;
        float radius = 0.35f, height = 1.8f;
    };

    struct EnemyDefinition
    {
        std::uint32_t id = 0;
        float hp = 20, speed = 2, armor = 0, radius = 0.35f, height = 1.8f;
        float contactDamage = 5, contactInterval = 0.8f, knockback = 2;
        std::uint32_t xp = 2, currency = 1;
    };

    struct WeaponDefinition
    {
        std::uint32_t id = 0;
        WeaponKind kind = WeaponKind::SingleProjectile;
        float damage = 10, cooldown = 0.8f, range = 12, knockback = 0;
        float halfAngleRadians = 1.04719755f;
        float projectileSpeed = 20, projectileLife = 1, projectileRadius = 0.12f;
        std::uint32_t projectileCount = 1;
        float spreadRadians = 0;
    };

    struct EnemyWeight
    {
        std::uint32_t enemyId = 0;
        float weight = 0;
    };

    struct WaveDefinition
    {
        std::uint32_t id = 0;
        double duration = 12;
        std::uint32_t maxAlive = 24;
        double spawnPerSecond = 2;
        float minSpawnDistance = 4, maxSpawnDistance = 7;
        std::uint32_t maxSpawnAttempts = 12;
        std::vector<EnemyWeight> enemyWeights;
    };

    enum class StatKind
    {
        MaxHp,
        Speed,
        Armor,
        DamageMultiplier,
        AttackSpeed,
        PickupRadius
    };

    enum class ModifierOperation
    {
        Flat,
        Percent
    };

    struct StatModifierDefinition
    {
        StatKind stat = StatKind::MaxHp;
        ModifierOperation operation = ModifierOperation::Flat;
        float value = 0;
    };

    struct PassiveDefinition
    {
        std::uint32_t id = 0;
        std::string name;
        std::uint32_t maxStacks = 1;
        double weight = 1;
        std::vector<StatModifierDefinition> modifiers;
    };

    enum class ShopItemKind
    {
        Weapon,
        Passive
    };

    struct ShopItemDefinition
    {
        ShopItemKind kind = ShopItemKind::Weapon;
        std::uint32_t definitionId = 0;
        std::uint64_t price = 0;
        double weight = 1;
    };

    struct ProgressionDefinition
    {
        std::vector<std::uint64_t> cumulativeXp;
        std::uint32_t choiceCount = 3;
    };

    struct ShopDefinition
    {
        std::uint32_t offerCount = 4;
        std::uint64_t rerollCost = 2;
        std::vector<ShopItemDefinition> items;
    };

    struct GameDefinitions
    {
        std::uint32_t schemaVersion = 1;
        PlayerDefinition player;
        std::uint32_t enemyCapacity = 64, projectileCapacity = 128, dropCapacity = 64;
        std::uint32_t slotLimit = 6, maxAttacksPerTick = 6, maxSpawnsPerTick = 8;
        float minimumCooldown = 0.1f;
        std::vector<EnemyDefinition> enemies;
        std::vector<WeaponDefinition> weapons;
        std::vector<WaveDefinition> waves;
        std::vector<std::uint32_t> loadout;
        std::vector<PassiveDefinition> passives;
        ProgressionDefinition progression;
        ShopDefinition shop;

        const EnemyDefinition* FindEnemy(std::uint32_t id) const noexcept;
        const WeaponDefinition* FindWeapon(std::uint32_t id) const noexcept;
        const PassiveDefinition* FindPassive(std::uint32_t id) const noexcept;
    };

    bool ValidateGameDefinitions(const GameDefinitions& definitions, std::string& error);
    // Parse and validate a temporary value, then replace out only after complete success.
    bool LoadGameDefinitions(const std::filesystem::path& file, GameDefinitions& out, std::string& error);
}
