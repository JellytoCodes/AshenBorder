#include "Game/GameDefinitions.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;

    constexpr float kMaximumStat = 1000000;
    constexpr float kMaximumSpeed = 120;
    constexpr float kMaximumSeconds = 3600;
    constexpr float kMinimumCooldown = 0.05f;

    bool Between(float value, float minimum, float maximum) noexcept
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }

    bool Positive(float value, float maximum = kMaximumStat) noexcept
    {
        return std::isfinite(value) && value > 0 && value <= maximum;
    }

    bool Reject(std::string& error, std::string_view message)
    {
        error = message;
        return false;
    }

    void Fields(const Json& value, std::initializer_list<std::string_view> names,
        std::string_view context)
    {
        if (!value.is_object())
        {
            throw std::runtime_error(std::string(context) + " must be an object");
        }
        for (const auto name : names)
        {
            if (!value.contains(std::string(name)))
            {
                throw std::runtime_error(std::string(context) + " is missing " + std::string(name));
            }
        }
        for (const auto& item : value.items())
        {
            if (std::find(names.begin(), names.end(), std::string_view(item.key())) == names.end())
            {
                throw std::runtime_error(std::string(context) + " has unsupported field " + item.key());
            }
        }
    }

    double Number(const Json& value, const char* name)
    {
        const auto& item = value.at(name);
        if (!item.is_number())
        {
            throw std::runtime_error(std::string(name) + " must be numeric");
        }
        const double result = item.get<double>();
        if (!std::isfinite(result))
        {
            throw std::runtime_error(std::string(name) + " must be finite");
        }
        return result;
    }

    float Scalar(const Json& value, const char* name)
    {
        const double result = Number(value, name);
        if (result < -std::numeric_limits<float>::max() || result > std::numeric_limits<float>::max())
        {
            throw std::runtime_error(std::string(name) + " exceeds the float range");
        }
        return static_cast<float>(result);
    }

    std::uint32_t Unsigned(const Json& item, std::string_view name)
    {
        if (!item.is_number_integer())
        {
            throw std::runtime_error(std::string(name) + " must be a uint32 integer");
        }
        if (item.is_number_unsigned())
        {
            const auto result = item.get<std::uint64_t>();
            if (result <= std::numeric_limits<std::uint32_t>::max())
            {
                return static_cast<std::uint32_t>(result);
            }
        }
        else
        {
            const auto result = item.get<std::int64_t>();
            if (result >= 0 && static_cast<std::uint64_t>(result) <= std::numeric_limits<std::uint32_t>::max())
            {
                return static_cast<std::uint32_t>(result);
            }
        }
        throw std::runtime_error(std::string(name) + " is outside uint32 range");
    }

    std::uint32_t Unsigned(const Json& object, const char* name)
    {
        return Unsigned(object.at(name), std::string_view(name));
    }

    std::uint64_t Unsigned64(const Json& item, std::string_view name)
    {
        if (item.is_number_unsigned())
        {
            return item.get<std::uint64_t>();
        }
        if (item.is_number_integer())
        {
            const auto value = item.get<std::int64_t>();
            if (value >= 0)
            {
                return static_cast<std::uint64_t>(value);
            }
        }
        throw std::runtime_error(std::string(name) + " must be a uint64 integer");
    }

    Client::StatKind ParseStat(const Json& value)
    {
        if (value == "MaxHp")
        {
            return Client::StatKind::MaxHp;
        }
        if (value == "Speed")
        {
            return Client::StatKind::Speed;
        }
        if (value == "Armor")
        {
            return Client::StatKind::Armor;
        }
        if (value == "DamageMultiplier")
        {
            return Client::StatKind::DamageMultiplier;
        }
        if (value == "AttackSpeed")
        {
            return Client::StatKind::AttackSpeed;
        }
        if (value == "PickupRadius")
        {
            return Client::StatKind::PickupRadius;
        }
        throw std::runtime_error("Unsupported passive stat");
    }

    Client::PassiveDefinition ParsePassive(const Json& value)
    {
        Fields(value, {"id", "name", "maxStacks", "weight", "modifiers"}, "passive");
        Client::PassiveDefinition result;
        result.id = Unsigned(value, "id");
        if (!value.at("name").is_string())
        {
            throw std::runtime_error("Passive name must be a string");
        }
        result.name = value.at("name").get<std::string>();
        result.maxStacks = Unsigned(value, "maxStacks");
        result.weight = Number(value, "weight");
        const auto& modifiers = value.at("modifiers");
        if (!modifiers.is_array())
        {
            throw std::runtime_error("Passive modifiers must be an array");
        }
        for (const auto& modifier : modifiers)
        {
            Fields(modifier, {"stat", "operation", "value"}, "stat modifier");
            const auto& operation = modifier.at("operation");
            if (operation != "Flat" && operation != "Percent")
            {
                throw std::runtime_error("Unsupported modifier operation");
            }
            result.modifiers.push_back({ParseStat(modifier.at("stat")),
                operation == "Flat" ? Client::ModifierOperation::Flat : Client::ModifierOperation::Percent,
                Scalar(modifier, "value")});
        }
        return result;
    }

    const Json& Array(const Json& object, const char* name)
    {
        const auto& result = object.at(name);
        if (!result.is_array())
        {
            throw std::runtime_error(std::string(name) + " must be an array");
        }
        return result;
    }

    Client::PlayerDefinition ParsePlayer(const Json& value)
    {
        Fields(value, {"maxHp", "speed", "armor", "damageMultiplier", "attackSpeed", "pickupRadius",
            "hitInvulnerability", "knockbackDecay", "radius", "height"}, "player");
        Client::PlayerDefinition result;
        result.maxHp = Scalar(value, "maxHp");
        result.speed = Scalar(value, "speed");
        result.armor = Scalar(value, "armor");
        result.damageMultiplier = Scalar(value, "damageMultiplier");
        result.attackSpeed = Scalar(value, "attackSpeed");
        result.pickupRadius = Scalar(value, "pickupRadius");
        result.hitInvulnerability = Scalar(value, "hitInvulnerability");
        result.knockbackDecay = Scalar(value, "knockbackDecay");
        result.radius = Scalar(value, "radius");
        result.height = Scalar(value, "height");
        return result;
    }

    Client::EnemyDefinition ParseEnemy(const Json& value, std::uint32_t schemaVersion)
    {
        const bool hasRanged = schemaVersion >= 3 && value.contains("ranged");
        const bool hasCharge = schemaVersion == 4 && value.contains("charge");
        if (hasRanged && hasCharge)
        {
            throw std::runtime_error("An enemy cannot combine ranged and charge attacks");
        }
        if (hasCharge)
        {
            Fields(value, {"id", "hp", "speed", "armor", "radius", "height", "contactDamage",
                "contactInterval", "knockback", "xp", "currency", "charge"}, "enemy");
        }
        else if (hasRanged)
        {
            Fields(value, {"id", "hp", "speed", "armor", "radius", "height", "contactDamage",
                "contactInterval", "knockback", "xp", "currency", "ranged"}, "enemy");
        }
        else
        {
            Fields(value, {"id", "hp", "speed", "armor", "radius", "height", "contactDamage",
                "contactInterval", "knockback", "xp", "currency"}, "enemy");
        }
        Client::EnemyDefinition result;
        result.id = Unsigned(value, "id");
        result.hp = Scalar(value, "hp");
        result.speed = Scalar(value, "speed");
        result.armor = Scalar(value, "armor");
        result.radius = Scalar(value, "radius");
        result.height = Scalar(value, "height");
        result.contactDamage = Scalar(value, "contactDamage");
        result.contactInterval = Scalar(value, "contactInterval");
        result.knockback = Scalar(value, "knockback");
        result.xp = Unsigned(value, "xp");
        result.currency = Unsigned(value, "currency");
        if (hasRanged)
        {
            const auto& ranged = value.at("ranged");
            Fields(ranged, {"damage", "interval", "projectileSpeed", "projectileLife", "projectileRadius",
                "range", "preferredDistance", "windup", "spawnDelay"}, "enemy ranged attack");
            result.ranged = Client::EnemyRangedDefinition{Scalar(ranged, "damage"), Scalar(ranged, "interval"),
                Scalar(ranged, "projectileSpeed"), Scalar(ranged, "projectileLife"), Scalar(ranged, "projectileRadius"),
                Scalar(ranged, "range"), Scalar(ranged, "preferredDistance"), Scalar(ranged, "windup"),
                Scalar(ranged, "spawnDelay")};
        }
        if (hasCharge)
        {
            const auto& charge = value.at("charge");
            const bool hasFirstStage = charge.contains("firstStage");
            if (hasFirstStage)
            {
                Fields(charge, {"range", "windup", "speed", "duration", "recovery", "interval", "spawnDelay", "firstStage"},
                    "enemy charge attack");
            }
            else
            {
                Fields(charge, {"range", "windup", "speed", "duration", "recovery", "interval", "spawnDelay"},
                    "enemy charge attack");
            }
            result.charge = Client::EnemyChargeDefinition{Scalar(charge, "range"), Scalar(charge, "windup"),
                Scalar(charge, "speed"), Scalar(charge, "duration"), Scalar(charge, "recovery"),
                Scalar(charge, "interval"), Scalar(charge, "spawnDelay")};
            if (hasFirstStage)
            {
                result.charge->firstStage = Unsigned(charge, "firstStage");
            }
        }
        return result;
    }

    Client::WeaponDefinition ParseWeapon(const Json& value)
    {
        if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string())
        {
            throw std::runtime_error("weapon kind must be a string");
        }
        const auto kind = value.at("kind").get<std::string>();
        Client::WeaponDefinition result;
        if (kind == "Melee")
        {
            Fields(value, {"id", "kind", "damage", "cooldown", "range", "knockback", "halfAngleRadians"}, "Melee weapon");
            result.kind = Client::WeaponKind::Melee;
            result.halfAngleRadians = Scalar(value, "halfAngleRadians");
        }
        else if (kind == "SingleProjectile" || kind == "Shotgun")
        {
            Fields(value, {"id", "kind", "damage", "cooldown", "range", "knockback", "projectileSpeed",
                "projectileLife", "projectileRadius", "projectileCount", "spreadRadians"}, "projectile weapon");
            result.kind = kind == "Shotgun" ? Client::WeaponKind::Shotgun : Client::WeaponKind::SingleProjectile;
            result.projectileSpeed = Scalar(value, "projectileSpeed");
            result.projectileLife = Scalar(value, "projectileLife");
            result.projectileRadius = Scalar(value, "projectileRadius");
            result.projectileCount = Unsigned(value, "projectileCount");
            result.spreadRadians = Scalar(value, "spreadRadians");
        }
        else
        {
            throw std::runtime_error("Unsupported weapon kind: " + kind);
        }
        result.id = Unsigned(value, "id");
        result.damage = Scalar(value, "damage");
        result.cooldown = Scalar(value, "cooldown");
        result.range = Scalar(value, "range");
        result.knockback = Scalar(value, "knockback");
        return result;
    }

    Client::WaveDefinition ParseWave(const Json& value, std::uint32_t schemaVersion)
    {
        const bool hasBoss = schemaVersion >= 3 && value.contains("boss");
        if (hasBoss)
        {
            Fields(value, {"id", "duration", "maxAlive", "spawnPerSecond", "minSpawnDistance",
                "maxSpawnDistance", "maxSpawnAttempts", "enemyWeights", "boss"}, "wave");
        }
        else
        {
            Fields(value, {"id", "duration", "maxAlive", "spawnPerSecond", "minSpawnDistance",
                "maxSpawnDistance", "maxSpawnAttempts", "enemyWeights"}, "wave");
        }
        Client::WaveDefinition result;
        result.id = Unsigned(value, "id");
        result.duration = Number(value, "duration");
        result.maxAlive = Unsigned(value, "maxAlive");
        result.spawnPerSecond = Number(value, "spawnPerSecond");
        result.minSpawnDistance = Scalar(value, "minSpawnDistance");
        result.maxSpawnDistance = Scalar(value, "maxSpawnDistance");
        result.maxSpawnAttempts = Unsigned(value, "maxSpawnAttempts");
        for (const auto& weight : Array(value, "enemyWeights"))
        {
            Fields(weight, {"enemyId", "weight"}, "enemy weight");
            result.enemyWeights.push_back({Unsigned(weight, "enemyId"), Scalar(weight, "weight")});
        }
        if (hasBoss)
        {
            const auto& boss = value.at("boss");
            Fields(boss, {"enemyId", "name"}, "wave boss");
            result.bossEnemyId = Unsigned(boss, "enemyId");
            if (result.bossEnemyId == 0 || !boss.at("name").is_string())
            {
                throw std::runtime_error("A wave boss requires a nonzero enemy ID and a name string");
            }
            result.bossName = boss.at("name").get<std::string>();
        }
        return result;
    }
}

namespace Client
{
    bool ScaleShopPrice(std::uint64_t base, std::uint32_t percent, std::uint64_t& out) noexcept
    {
        if (percent == 0)
        {
            return false;
        }
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto whole = base / 100;
        const auto fraction = ((base % 100) * percent + 99) / 100;
        if (whole > maximum / percent || whole * percent > maximum - fraction)
        {
            return false;
        }
        out = whole * percent + fraction;
        return true;
    }

    const EnemyDefinition* GameDefinitions::FindEnemy(std::uint32_t id) const noexcept
    {
        const auto found = std::find_if(enemies.begin(), enemies.end(), [id](const EnemyDefinition& value)
        {
            return value.id == id;
        });
        return found == enemies.end() ? nullptr : &*found;
    }

    const WeaponDefinition* GameDefinitions::FindWeapon(std::uint32_t id) const noexcept
    {
        const auto found = std::find_if(weapons.begin(), weapons.end(), [id](const WeaponDefinition& value)
        {
            return value.id == id;
        });
        return found == weapons.end() ? nullptr : &*found;
    }

    const PassiveDefinition* GameDefinitions::FindPassive(std::uint32_t id) const noexcept
    {
        const auto found = std::find_if(passives.begin(), passives.end(), [id](const PassiveDefinition& value)
        {
            return value.id == id;
        });
        return found == passives.end() ? nullptr : &*found;
    }

    bool ValidateGameDefinitions(const GameDefinitions& definitions, std::string& error)
    {
        try
        {
            if (definitions.schemaVersion < 1 || definitions.schemaVersion > 4)
            {
                return Reject(error, "Unsupported game definitions schemaVersion");
            }
            if (definitions.schemaVersion < 3 && !definitions.shop.stagePricePercent.empty())
            {
                return Reject(error, "Stage shop prices require schema three or four");
            }
            const auto& player = definitions.player;
            if (!Positive(player.maxHp) || !Between(player.speed, 0, kMaximumSpeed) ||
                !Between(player.armor, 0, kMaximumStat) || !Between(player.damageMultiplier, 0, kMaximumStat) ||
                !Between(player.attackSpeed, 0.1f, 10) || !Between(player.pickupRadius, 0, kMaximumStat) ||
                !Between(player.hitInvulnerability, 0, kMaximumSeconds) || !Between(player.knockbackDecay, 0, kMaximumStat) ||
                !Positive(player.radius) || !Positive(player.height))
            {
                return Reject(error, "Player fields exceed their finite gameplay ranges");
            }
            if (definitions.enemyCapacity < 1 || definitions.enemyCapacity > 10000 ||
                definitions.projectileCapacity < 1 || definitions.projectileCapacity > 10000 ||
                definitions.dropCapacity < 1 || definitions.dropCapacity > 10000 ||
                definitions.slotLimit < 1 || definitions.slotLimit > 6 ||
                definitions.maxAttacksPerTick < 1 || definitions.maxAttacksPerTick > definitions.slotLimit ||
                definitions.maxSpawnsPerTick < 1 || definitions.maxSpawnsPerTick > 64 ||
                !Between(definitions.minimumCooldown, kMinimumCooldown, kMaximumSeconds))
            {
                return Reject(error, "Pool capacities, slot/tick budgets or minimumCooldown are invalid");
            }
            if (definitions.enemies.empty() || definitions.weapons.empty() || definitions.waves.empty() ||
                definitions.loadout.empty() || definitions.loadout.size() > definitions.slotLimit)
            {
                return Reject(error, "Enemy/weapon/wave catalogs and a loadout within slotLimit are required");
            }
            std::unordered_set<std::uint32_t> enemyIds;
            for (const auto& enemy : definitions.enemies)
            {
                if (enemy.id == 0 || !enemyIds.insert(enemy.id).second)
                {
                    return Reject(error, "Enemy IDs must be nonzero and unique within the enemy catalog");
                }
                if (!Positive(enemy.hp) || !Between(enemy.speed, 0, kMaximumSpeed) ||
                    !Between(enemy.armor, 0, kMaximumStat) || !Positive(enemy.radius) || !Positive(enemy.height) ||
                    !Between(enemy.contactDamage, 0, kMaximumStat) ||
                    !Between(enemy.contactInterval, kMinimumCooldown, kMaximumSeconds) ||
                    !Between(enemy.knockback, 0, kMaximumSpeed))
                {
                    return Reject(error, "Enemy fields exceed their finite gameplay ranges");
                }
                if (enemy.ranged)
                {
                    const auto& ranged = *enemy.ranged;
                    if (definitions.schemaVersion < 3 || !Positive(ranged.damage) ||
                        !Between(ranged.interval, kMinimumCooldown, kMaximumSeconds) ||
                        !Positive(ranged.projectileSpeed, kMaximumSpeed) || !Positive(ranged.projectileLife, kMaximumSeconds) ||
                        !Positive(ranged.projectileRadius) || !Positive(ranged.range) ||
                        !Between(ranged.preferredDistance, 0, ranged.range) ||
                        !Between(ranged.windup, kMinimumCooldown, kMaximumSeconds) ||
                        !Between(ranged.spawnDelay, kMinimumCooldown, kMaximumSeconds) ||
                        static_cast<double>(ranged.range) > static_cast<double>(ranged.projectileSpeed) * ranged.projectileLife)
                    {
                        return Reject(error, "Schema 3/4 ranged attacks require finite reachable range, positive shot fields and visible attack delays");
                    }
                }
                if (enemy.charge)
                {
                    const auto& charge = *enemy.charge;
                    if (definitions.schemaVersion != 4 || enemy.ranged || !Positive(charge.range, 10000) ||
                        !Positive(charge.speed, kMaximumSpeed) ||
                        !Between(charge.windup, kMinimumCooldown, kMaximumSeconds) ||
                        !Between(charge.duration, kMinimumCooldown, kMaximumSeconds) ||
                        !Between(charge.recovery, kMinimumCooldown, kMaximumSeconds) ||
                        !Between(charge.interval, kMinimumCooldown, kMaximumSeconds) ||
                        !Between(charge.spawnDelay, kMinimumCooldown, kMaximumSeconds) ||
                        charge.firstStage < 1 || charge.firstStage > definitions.waves.size())
                    {
                        return Reject(error, "Schema 4 charge attacks require exclusive finite positive movement, visible phase delays and a valid firstStage");
                    }
                }
            }
            std::unordered_set<std::uint32_t> weaponIds;
            for (const auto& weapon : definitions.weapons)
            {
                if (weapon.id == 0 || !weaponIds.insert(weapon.id).second)
                {
                    return Reject(error, "Weapon IDs must be nonzero and unique within the weapon catalog");
                }
                if (!Between(weapon.damage, 0, kMaximumStat) || !Between(weapon.cooldown, kMinimumCooldown, kMaximumSeconds) ||
                    !Positive(weapon.range) || !Between(weapon.knockback, 0, kMaximumSpeed) ||
                    !std::isfinite(weapon.halfAngleRadians) || !std::isfinite(weapon.projectileSpeed) ||
                    !std::isfinite(weapon.projectileLife) || !std::isfinite(weapon.projectileRadius) ||
                    !std::isfinite(weapon.spreadRadians))
                {
                    return Reject(error, "Weapon common fields exceed their finite gameplay ranges");
                }
                if (weapon.kind == WeaponKind::Melee)
                {
                    if (!Between(weapon.halfAngleRadians, 0, std::numbers::pi_v<float>))
                    {
                        return Reject(error, "Melee halfAngleRadians must be in [0,pi]");
                    }
                }
                else if (weapon.kind == WeaponKind::SingleProjectile || weapon.kind == WeaponKind::Shotgun)
                {
                    if (!Positive(weapon.projectileSpeed, kMaximumSpeed) ||
                        !Positive(weapon.projectileLife, kMaximumSeconds) || !Between(weapon.projectileRadius, 0, kMaximumStat) ||
                        !Between(weapon.spreadRadians, 0, std::numbers::pi_v<float>) ||
                        (weapon.kind == WeaponKind::SingleProjectile && (weapon.projectileCount != 1 || weapon.spreadRadians != 0)) ||
                        (weapon.kind == WeaponKind::Shotgun && (weapon.projectileCount < 2 || weapon.projectileCount > 16)))
                    {
                        return Reject(error, "Projectile fields, pellet count or spread do not match the weapon kind");
                    }
                }
                else
                {
                    return Reject(error, "Unsupported WeaponKind value");
                }
            }
            for (const auto weaponId : definitions.loadout)
            {
                if (!weaponIds.contains(weaponId))
                {
                    return Reject(error, "Loadout references an unknown weapon ID");
                }
            }
            std::unordered_set<std::uint32_t> bossIds;
            for (const auto& wave : definitions.waves)
            {
                if (wave.bossEnemyId != 0)
                {
                    if (definitions.schemaVersion < 3 || !enemyIds.contains(wave.bossEnemyId) ||
                        !bossIds.insert(wave.bossEnemyId).second || wave.bossName.empty() || wave.bossName.size() > 128 ||
                        std::any_of(wave.bossName.begin(), wave.bossName.end(), [](unsigned char value)
                        {
                            return value < 0x20 || value == 0x7f;
                        }))
                    {
                        return Reject(error, "Schema 3/4 bosses require unique known enemy IDs and bounded nonempty display names");
                    }
                }
                else if (!wave.bossName.empty())
                {
                    return Reject(error, "A boss name requires its enemy ID");
                }
            }
            std::unordered_set<std::uint32_t> waveIds;
            for (const auto& wave : definitions.waves)
            {
                if (wave.id == 0 || !waveIds.insert(wave.id).second)
                {
                    return Reject(error, "Wave IDs must be nonzero and unique within the wave catalog");
                }
                if (!std::isfinite(wave.duration) || wave.duration <= 0 || wave.duration > kMaximumSeconds ||
                    wave.maxAlive < 1 || wave.maxAlive > definitions.enemyCapacity ||
                    !std::isfinite(wave.spawnPerSecond) || wave.spawnPerSecond < 0 || wave.spawnPerSecond > kMaximumStat ||
                    !Between(wave.minSpawnDistance, 0, kMaximumStat) || !Positive(wave.maxSpawnDistance) ||
                    wave.minSpawnDistance > wave.maxSpawnDistance || wave.maxSpawnAttempts < 1 || wave.maxSpawnAttempts > 64 ||
                    wave.enemyWeights.empty())
                {
                    return Reject(error, "Wave time, capacity, spawn budget or distance interval is invalid");
                }
                double totalWeight = 0;
                std::unordered_set<std::uint32_t> weightedEnemies;
                for (const auto& weight : wave.enemyWeights)
                {
                    if (bossIds.contains(weight.enemyId) ||
                        !enemyIds.contains(weight.enemyId) || !weightedEnemies.insert(weight.enemyId).second ||
                        !std::isfinite(weight.weight) || weight.weight < 0)
                    {
                        return Reject(error, "Wave weights require unique known enemy IDs and finite nonnegative weights");
                    }
                    totalWeight += weight.weight;
                }
                if (!std::isfinite(totalWeight) || totalWeight <= 0)
                {
                    return Reject(error, "Wave enemy weights must have a positive finite sum");
                }
            }
            if (definitions.schemaVersion >= 2)
            {
                if (definitions.passives.empty() || definitions.passives.size() > 256 ||
                    definitions.progression.cumulativeXp.empty() || definitions.progression.cumulativeXp.size() > 1024 ||
                    definitions.progression.choiceCount < 1 || definitions.progression.choiceCount > 3 ||
                    definitions.shop.offerCount < 1 || definitions.shop.offerCount > 8 ||
                    definitions.shop.items.empty() || definitions.shop.items.size() > 256)
                {
                    return Reject(error, "Schema 2 and 3 require bounded passive, XP, choice and shop catalogs");
                }
                std::uint64_t previousXp = 0;
                for (const auto xp : definitions.progression.cumulativeXp)
                {
                    if (xp <= previousXp)
                    {
                        return Reject(error, "Cumulative XP thresholds must be positive and strictly increasing");
                    }
                    previousXp = xp;
                }
                std::unordered_set<std::uint32_t> passiveIds;
                double passiveWeight = 0;
                std::uint32_t totalStacks = 0;
                for (const auto& passive : definitions.passives)
                {
                    if (passive.id == 0 || !passiveIds.insert(passive.id).second ||
                        passive.name.empty() || passive.name.size() > 128 ||
                        passive.maxStacks < 1 || passive.maxStacks > 1000 ||
                        !std::isfinite(passive.weight) || passive.weight < 0 ||
                        passive.modifiers.empty() || passive.modifiers.size() > 32)
                    {
                        return Reject(error, "Passive ID, name, stack limit, weight or modifiers are invalid");
                    }
                    totalStacks += passive.maxStacks;
                    passiveWeight += passive.weight;
                    for (const auto& modifier : passive.modifiers)
                    {
                        if (modifier.stat < StatKind::MaxHp || modifier.stat > StatKind::PickupRadius ||
                            (modifier.operation != ModifierOperation::Flat && modifier.operation != ModifierOperation::Percent) ||
                            !Between(modifier.value, -kMaximumStat, kMaximumStat))
                        {
                            return Reject(error, "Passive modifier stat, operation or finite value is invalid");
                        }
                    }
                }
                if (totalStacks > 4096 || !std::isfinite(passiveWeight) || passiveWeight <= 0)
                {
                    return Reject(error, "Passive total stack budget or positive finite weight sum is invalid");
                }
                std::unordered_set<std::uint64_t> shopKeys;
                double shopWeight = 0;
                for (const auto& item : definitions.shop.items)
                {
                    const bool weapon = item.kind == ShopItemKind::Weapon;
                    if ((item.kind != ShopItemKind::Weapon && item.kind != ShopItemKind::Passive) ||
                        (weapon ? !weaponIds.contains(item.definitionId) : !passiveIds.contains(item.definitionId)) ||
                        !std::isfinite(item.weight) || item.weight < 0)
                    {
                        return Reject(error, "Shop kind, definition reference or finite weight is invalid");
                    }
                    const auto key = (static_cast<std::uint64_t>(item.kind) << 32) | item.definitionId;
                    if (!shopKeys.insert(key).second)
                    {
                        return Reject(error, "Shop catalog repeats the same kind and definition");
                    }
                    shopWeight += item.weight;
                }
                if (!std::isfinite(shopWeight) || shopWeight <= 0)
                {
                    return Reject(error, "Shop weights must have a positive finite sum");
                }
                if (definitions.shop.stagePricePercent.size() > 1024)
                {
                    return Reject(error, "Stage shop prices require schema three or four and at most 1024 percentages");
                }
                std::uint32_t previousPercent = 0;
                for (const auto percent : definitions.shop.stagePricePercent)
                {
                    std::uint64_t price = 0;
                    if (percent < 1 || percent > 1000000 || percent < previousPercent ||
                        !ScaleShopPrice(definitions.shop.rerollCost, percent, price))
                    {
                        return Reject(error, "Stage price percentages must be positive, nondecreasing and uint64 safe");
                    }
                    for (const auto& item : definitions.shop.items)
                    {
                        if (!ScaleShopPrice(item.price, percent, price))
                        {
                            return Reject(error, "A scaled shop item price exceeds uint64");
                        }
                    }
                    previousPercent = percent;
                }
            }
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Game definition validation failed: ") + exception.what();
            return false;
        }
    }

    bool LoadGameDefinitions(const std::filesystem::path& file, GameDefinitions& out, std::string& error)
    {
        try
        {
            std::ifstream input(file, std::ios::binary);
            if (!input)
            {
                return Reject(error, "Cannot open game definitions file");
            }
            std::vector<std::unordered_set<std::string>> objectKeys;
            const auto rejectDuplicateKeys = [&](int, Json::parse_event_t event, Json& parsed)
            {
                if (event == Json::parse_event_t::object_start)
                {
                    objectKeys.emplace_back();
                }
                else if (event == Json::parse_event_t::key)
                {
                    const auto name = parsed.get<std::string>();
                    if (!objectKeys.back().insert(name).second)
                    {
                        throw std::runtime_error("Duplicate JSON field: " + name);
                    }
                }
                else if (event == Json::parse_event_t::object_end)
                {
                    objectKeys.pop_back();
                }
                return true;
            };
            const auto root = Json::parse(input, rejectDuplicateKeys);
            if (input.bad())
            {
                return Reject(error, "Failed while reading game definitions file");
            }
            const auto schemaVersion = Unsigned(root, "schemaVersion");
            if (schemaVersion == 1)
            {
                Fields(root, {"schemaVersion", "player", "pools", "slotLimit", "maxAttacksPerTick", "maxSpawnsPerTick",
                    "minimumCooldown", "enemies", "weapons", "waves", "loadout"}, "game definitions");
            }
            else if (schemaVersion >= 2 && schemaVersion <= 4)
            {
                Fields(root, {"schemaVersion", "player", "pools", "slotLimit", "maxAttacksPerTick", "maxSpawnsPerTick",
                    "minimumCooldown", "enemies", "weapons", "waves", "loadout", "passives", "progression", "shop"}, "game definitions");
            }
            else
            {
                return Reject(error, "Unsupported game definitions schemaVersion; expected 1, 2, 3 or 4");
            }
            GameDefinitions candidate;
            candidate.schemaVersion = schemaVersion;
            candidate.player = ParsePlayer(root.at("player"));
            const auto& pools = root.at("pools");
            Fields(pools, {"enemyCapacity", "projectileCapacity", "dropCapacity"}, "pools");
            candidate.enemyCapacity = Unsigned(pools, "enemyCapacity");
            candidate.projectileCapacity = Unsigned(pools, "projectileCapacity");
            candidate.dropCapacity = Unsigned(pools, "dropCapacity");
            candidate.slotLimit = Unsigned(root, "slotLimit");
            candidate.maxAttacksPerTick = Unsigned(root, "maxAttacksPerTick");
            candidate.maxSpawnsPerTick = Unsigned(root, "maxSpawnsPerTick");
            candidate.minimumCooldown = Scalar(root, "minimumCooldown");
            for (const auto& value : Array(root, "enemies"))
            {
                candidate.enemies.push_back(ParseEnemy(value, schemaVersion));
            }
            for (const auto& value : Array(root, "weapons"))
            {
                candidate.weapons.push_back(ParseWeapon(value));
            }
            for (const auto& value : Array(root, "waves"))
            {
                candidate.waves.push_back(ParseWave(value, schemaVersion));
            }
            for (const auto& value : Array(root, "loadout"))
            {
                candidate.loadout.push_back(Unsigned(value, std::string_view("loadout ID")));
            }
            if (schemaVersion >= 2)
            {
                for (const auto& value : Array(root, "passives"))
                {
                    candidate.passives.push_back(ParsePassive(value));
                }
                const auto& progression = root.at("progression");
                Fields(progression, {"cumulativeXp", "choiceCount"}, "progression");
                candidate.progression.choiceCount = Unsigned(progression, "choiceCount");
                for (const auto& value : Array(progression, "cumulativeXp"))
                {
                    candidate.progression.cumulativeXp.push_back(Unsigned64(value, "XP threshold"));
                }
                const auto& shop = root.at("shop");
                const bool stagePrices = schemaVersion >= 3 && shop.contains("stagePricePercent");
                if (stagePrices)
                {
                    Fields(shop, {"offerCount", "rerollCost", "items", "stagePricePercent"}, "shop");
                    const auto& percentages = Array(shop, "stagePricePercent");
                    if (percentages.empty())
                    {
                        throw std::runtime_error("An explicit stage price table cannot be empty");
                    }
                    for (const auto& percent : percentages)
                    {
                        candidate.shop.stagePricePercent.push_back(Unsigned(percent, std::string_view("stage price percent")));
                    }
                }
                else
                {
                    Fields(shop, {"offerCount", "rerollCost", "items"}, "shop");
                }
                candidate.shop.offerCount = Unsigned(shop, "offerCount");
                candidate.shop.rerollCost = Unsigned64(shop.at("rerollCost"), "rerollCost");
                for (const auto& value : Array(shop, "items"))
                {
                    Fields(value, {"kind", "definitionId", "price", "weight"}, "shop item");
                    const auto& kind = value.at("kind");
                    if (kind != "Weapon" && kind != "Passive")
                    {
                        throw std::runtime_error("Unsupported shop item kind");
                    }
                    candidate.shop.items.push_back({kind == "Weapon" ? ShopItemKind::Weapon : ShopItemKind::Passive,
                        Unsigned(value, "definitionId"), Unsigned64(value.at("price"), "price"), Number(value, "weight")});
                }
            }
            if (!ValidateGameDefinitions(candidate, error))
            {
                return false;
            }
            out = std::move(candidate);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Game definition load failed: ") + exception.what();
            return false;
        }
    }
}
