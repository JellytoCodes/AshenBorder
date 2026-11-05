#include "Presentation/CombatEffects.h"

#include "Game/GameDefinitions.h"
#include "Game/LastFieldRun.h"
#include "Graphics/RenderFrame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace
{
    constexpr Engine::Vector4 kAsh = {.72f, .75f, .70f, .65f};
    constexpr Engine::Vector4 kSteel = {.82f, .88f, .80f, .65f};
    constexpr Engine::Vector4 kEmber = {1.0f, .43f, .17f, .72f};
    constexpr std::uint32_t kCapacity = 2048;
    constexpr std::uint32_t kTrailsPerTick = 32;

    Engine::Vector3 Raised(Engine::Vector3 position, float height)
    {
        position.y += height;
        return position;
    }
}

namespace Client
{
    bool CombatEffects::Initialize(std::uint32_t projectileCapacity)
    {
        if (projectileCapacity == 0 || !_particles.Initialize(kCapacity))
        {
            return false;
        }
        try
        {
            _trails.resize(projectileCapacity);
        }
        catch (...)
        {
            return false;
        }
        Reset(0);
        return true;
    }

    void CombatEffects::Reset(std::uint64_t runEpoch) noexcept
    {
        _particles.Clear();
        std::fill(_trails.begin(), _trails.end(), Trail{});
        _runEpoch = runEpoch;
        _lastSequence = _lastTick = 0;
        _randomState = 0x5a73c9e1u ^ static_cast<std::uint32_t>(runEpoch) ^
            static_cast<std::uint32_t>(runEpoch >> 32);
        if (_randomState == 0)
        {
            _randomState = 1;
        }
        _stepDistance = 0;
        _hasPlayer = false;
    }

    void CombatEffects::SetEnabled(bool enabled) noexcept
    {
        if (_enabled != enabled)
        {
            Reset(_runEpoch);
            _enabled = enabled;
        }
    }

    float CombatEffects::Random(float minimum, float maximum) noexcept
    {
        // Visual randomness has no access to the run's simulation RNG.
        _randomState ^= _randomState << 13;
        _randomState ^= _randomState >> 17;
        _randomState ^= _randomState << 5;
        return minimum + (maximum - minimum) * static_cast<float>(_randomState >> 8) / 16777216.0f;
    }

    void CombatEffects::Impact(Engine::Vector3 position, std::uint32_t count, bool warm)
    {
        std::array<Engine::ParticleSpawn, 16> burst{};
        count = std::min<std::uint32_t>(count, static_cast<std::uint32_t>(burst.size()) - 1);
        auto& glow = burst[0];
        glow.position = position;
        glow.shape = Engine::ParticleShape::Glow;
        glow.startColor = warm ? kEmber : kSteel;
        glow.endColor = {glow.startColor.x, glow.startColor.y, glow.startColor.z, 0};
        glow.startSize = {.36f, .36f};
        glow.endSize = {.04f, .04f};
        glow.lifetime = .10f;
        for (std::uint32_t i = 1; i <= count; ++i)
        {
            auto& p = burst[i];
            const float angle = Random(0, 2 * std::numbers::pi_v<float>);
            const float speed = Random(.65f, 2.8f);
            p.position = position;
            p.velocity = {std::cos(angle) * speed, Random(.3f, 1.9f), std::sin(angle) * speed};
            p.acceleration = {0, -4, 0};
            p.startSize = {Random(.035f, .07f), Random(.07f, .14f)};
            p.endSize = {.012f, .02f};
            p.startColor = warm && (i % 3 == 0) ? kEmber : kSteel;
            p.endColor = {.46f, .47f, .43f, 0};
            p.rotation = angle;
            p.angularVelocity = Random(-7, 7);
            p.lifetime = Random(.16f, .30f);
            p.drag = 2;
        }
        _particles.Emit(std::span(burst.data(), count + 1));
    }

    void CombatEffects::Ash(Engine::Vector3 position, std::uint32_t count, float height)
    {
        std::array<Engine::ParticleSpawn, Engine::kMaxParticleBurst> burst{};
        count = std::min<std::uint32_t>(count, static_cast<std::uint32_t>(burst.size()));
        for (std::uint32_t i = 0; i < count; ++i)
        {
            auto& p = burst[i];
            const float angle = Random(0, 2 * std::numbers::pi_v<float>);
            const float speed = Random(.2f, 1.5f);
            p.position = {position.x + Random(-.15f, .15f), position.y + Random(.08f, height),
                position.z + Random(-.15f, .15f)};
            p.velocity = {std::cos(angle) * speed, Random(.2f, 1.3f), std::sin(angle) * speed};
            p.acceleration = {0, -.65f, 0};
            p.drag = 1.3f;
            const bool haze = i % 5 == 0;
            p.shape = haze ? Engine::ParticleShape::Ash : Engine::ParticleShape::Shard;
            const float size = haze ? Random(.18f, .30f) : Random(.04f, .09f);
            p.startSize = {size, size};
            p.endSize = haze ? Engine::Vector2{size * 2, size * 2} : Engine::Vector2{.018f, .018f};
            p.startColor = kAsh;
            p.startColor.w = haze ? .20f : .62f;
            p.endColor = {.38f, .40f, .37f, 0};
            p.lifetime = Random(.36f, .72f);
            p.rotation = angle;
            p.angularVelocity = Random(-3, 3);
        }
        _particles.Emit(std::span(burst.data(), count));
    }

    void CombatEffects::Ring(Engine::Vector3 position, float size, bool warm)
    {
        Engine::ParticleSpawn p;
        p.position = Raised(position, .06f);
        p.facing = Engine::ParticleFacing::Horizontal;
        p.shape = Engine::ParticleShape::Ring;
        p.startSize = {size * .55f, size * .55f};
        p.endSize = {size, size};
        p.startColor = warm ? Engine::Vector4{.85f, .63f, .32f, .35f} : Engine::Vector4{.69f, .76f, .65f, .3f};
        p.endColor = {p.startColor.x, p.startColor.y, p.startColor.z, 0};
        p.lifetime = .35f;
        _particles.Emit(std::span(&p, 1));
    }

    bool CombatEffects::Advance(float seconds, const LastFieldRun& run)
    {
        if (!std::isfinite(seconds) || seconds < 0)
        {
            return false;
        }
        if (run.RunEpoch() != _runEpoch)
        {
            Reset(run.RunEpoch());
        }
        if (!_enabled || seconds == 0)
        {
            return true;
        }
        if (!_particles.Tick(seconds))
        {
            return false;
        }
        if (run.Wave().phase != WavePhase::Combat || _lastTick == run.TickIndex())
        {
            _hasPlayer = false;
            return true;
        }
        _lastTick = run.TickIndex();
        std::uint32_t trailCount = 0;
        if (!run.Projectiles().ForEachActive([&](Engine::PoolHandle handle, const Projectile& projectile)
            {
                if (handle.index >= _trails.size())
                {
                    return;
                }
                auto& previous = _trails[handle.index];
                if (previous.valid && previous.handle == handle)
                {
                    const Engine::Vector3 delta{projectile.position.x - previous.position.x,
                        projectile.position.y - previous.position.y, projectile.position.z - previous.position.z};
                    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                    if (length > .03f && length < 2.5f && trailCount < kTrailsPerTick)
                    {
                        Engine::ParticleSpawn p;
                        p.position = {(projectile.position.x + previous.position.x) * .5f,
                            (projectile.position.y + previous.position.y) * .5f,
                            (projectile.position.z + previous.position.z) * .5f};
                        p.axis = delta;
                        p.facing = Engine::ParticleFacing::Axis;
                        p.shape = Engine::ParticleShape::Streak;
                        p.startSize = {.045f, length + .05f};
                        p.endSize = {.008f, length * .8f};
                        const bool hostile = projectile.owner.kind == EntityKind::Enemy;
                        p.startColor = hostile ? Engine::Vector4{.92f, .44f, .24f, .6f} : kSteel;
                        p.endColor = {p.startColor.x, p.startColor.y, p.startColor.z, 0};
                        p.lifetime = hostile ? .12f : .09f;
                        _particles.Emit(std::span(&p, 1));
                        ++trailCount;
                    }
                }
                previous = {handle, projectile.position, true};
            }))
        {
            return false;
        }
        const auto player = run.PlayerCollider().base;
        if (_hasPlayer && !run.Player().dead)
        {
            const float dx = player.x - _previousPlayer.x, dz = player.z - _previousPlayer.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            _stepDistance = distance < 2 ? _stepDistance + distance : 0;
            if (_stepDistance >= .8f)
            {
                _stepDistance = 0;
                Ash(player, 2, .10f);
            }
        }
        _previousPlayer = player;
        _hasPlayer = true;
        if (!run.Enemies().ForEachActive([&](Engine::PoolHandle handle, const Enemy& enemy)
            {
                if (enemy.chargePhase == EnemyChargePhase::Charging &&
                    (run.TickIndex() + handle.index) % 6 == 0)
                {
                    Ash(enemy.position, 2, .12f);
                }
            }))
        {
            return false;
        }
        for (const auto& hit : run.Hits())
        {
            if (hit.tick == run.TickIndex() &&
                (hit.kind == HitKind::StaticGeometry || hit.kind == HitKind::Terrain))
            {
                Impact(hit.position, 3, false);
            }
        }
        return true;
    }

    void CombatEffects::OnEvent(const GameEvent& event, const LastFieldRun& run, const GameDefinitions& definitions)
    {
        if (!_enabled || event.runEpoch != _runEpoch || event.sequence <= _lastSequence)
        {
            return;
        }
        _lastSequence = event.sequence;
        switch (event.kind)
        {
        case GameEventKind::Fired:
        {
            const auto* weapon = definitions.FindWeapon(event.definitionId);
            const auto slots = run.WeaponSlots();
            if (!weapon || event.slotIndex >= slots.size() || slots[event.slotIndex].definitionId != event.definitionId)
            {
                break;
            }
            const auto direction = slots[event.slotIndex].direction;
            if (weapon->kind == WeaponKind::Melee)
            {
                Engine::ParticleSpawn p;
                p.position = Raised(event.position, .72f);
                p.axis = {direction.x, 0, direction.y};
                p.facing = Engine::ParticleFacing::Horizontal;
                p.shape = Engine::ParticleShape::Arc;
                p.parameter = std::cos(weapon->halfAngleRadians);
                p.startSize = {weapon->range * 1.85f, weapon->range * 1.85f};
                p.endSize = {weapon->range * 2, weapon->range * 2};
                p.startColor = {.82f, .87f, .77f, .48f};
                p.endColor = {.54f, .62f, .51f, 0};
                p.rotation = -.18f;
                p.angularVelocity = 2.6f;
                p.lifetime = .14f;
                _particles.Emit(std::span(&p, 1));
            }
            else
            {
                auto position = Raised(event.position, .85f);
                position.x += direction.x * .5f;
                position.z += direction.y * .5f;
                Impact(position, 2, false);
            }
            break;
        }
        case GameEventKind::Damaged:
            if (event.damage > 0)
            {
                const bool player = event.target && event.target->kind == EntityKind::Player;
                Impact(Raised(event.position, .8f), player ? 9 : 5, player);
            }
            break;
        case GameEventKind::EnemyDied:
        {
            const auto* enemy = definitions.FindEnemy(event.definitionId);
            const bool boss = run.Wave().bossEnemyId == event.definitionId;
            Ash(event.position, boss ? 44 : 12, enemy ? enemy->height * .7f : 1.1f);
            if (boss)
            {
                Ring(event.position, 3.5f, false);
            }
            break;
        }
        case GameEventKind::PlayerDied:
            Ash(event.position, 22, 1.3f);
            break;
        case GameEventKind::PickedUp:
            Impact(Raised(event.position, .28f), 2, true);
            break;
        case GameEventKind::LevelQueued:
            Ring(run.PlayerCollider().base, 1.7f, true);
            break;
        case GameEventKind::EnemyAttacked:
        {
            const auto* enemy = definitions.FindEnemy(event.definitionId);
            if (enemy && enemy->ranged)
            {
                Impact(Raised(event.position, enemy->height * .58f), 2, true);
            }
            break;
        }
        case GameEventKind::WaveStarted:
            Ring(run.PlayerCollider().base, 1.35f, false);
            break;
        case GameEventKind::Victory:
            Ash(run.PlayerCollider().base, 28, 1.2f);
            break;
        default:
            break;
        }
    }

    bool CombatEffects::Append(Engine::RenderFrame& frame) const
    {
        return !_enabled || _particles.Append(frame);
    }

    Engine::ParticleStats CombatEffects::Stats() const noexcept
    {
        return _particles.Stats();
    }
}
