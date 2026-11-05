#include "Effects/ParticleSystem.h"

#include "Graphics/RenderFrame.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
    bool Finite(Engine::Vector2 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    bool Finite(Engine::Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool Finite(Engine::Vector4 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    }

    bool Bounded(Engine::Vector3 value, float limit)
    {
        return Finite(value) && std::abs(value.x) <= limit && std::abs(value.y) <= limit && std::abs(value.z) <= limit;
    }

    bool Color(Engine::Vector4 value)
    {
        return Finite(value) && value.x >= 0 && value.x <= 8 && value.y >= 0 && value.y <= 8 &&
            value.z >= 0 && value.z <= 8 && value.w >= 0 && value.w <= 1;
    }

    bool Size(Engine::Vector2 value)
    {
        return Finite(value) && value.x > 0 && value.x <= 128 && value.y > 0 && value.y <= 128;
    }

    bool Axis(Engine::Vector3 value)
    {
        const double length = static_cast<double>(value.x) * value.x +
            static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
        return Finite(value) && length > 1e-10 && length < 1e12;
    }

    bool Valid(const Engine::ParticleSpawn& value)
    {
        return Bounded(value.position, 1e6f) && Bounded(value.velocity, 1e4f) && Bounded(value.acceleration, 1e4f) &&
            Color(value.startColor) && Color(value.endColor) && Size(value.startSize) && Size(value.endSize) &&
            std::isfinite(value.lifetime) && value.lifetime > 0 && value.lifetime <= 10 &&
            std::isfinite(value.rotation) && std::abs(value.rotation) <= 10000 &&
            std::isfinite(value.angularVelocity) && std::abs(value.angularVelocity) <= 1000 &&
            std::isfinite(value.drag) && value.drag >= 0 && value.drag <= 100 &&
            std::isfinite(value.parameter) && value.parameter >= -1 && value.parameter <= 1 &&
            static_cast<std::uint32_t>(value.shape) <= static_cast<std::uint32_t>(Engine::ParticleShape::Ring) &&
            static_cast<std::uint32_t>(value.facing) <= static_cast<std::uint32_t>(Engine::ParticleFacing::Axis) &&
            (value.facing == Engine::ParticleFacing::Camera || Axis(value.axis)) &&
            (value.facing != Engine::ParticleFacing::Horizontal ||
                static_cast<double>(value.axis.x) * value.axis.x + static_cast<double>(value.axis.z) * value.axis.z > 1e-10);
    }

    float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }
}

namespace Engine
{
    bool ValidParticleDraw(const ParticleDraw& value) noexcept
    {
        return Finite(value.positionRotation) && Bounded({value.positionRotation.x, value.positionRotation.y,
                value.positionRotation.z}, 1e6f) && Finite(value.sizeShape) && Color(value.color) &&
            Finite(value.axisFacing) && value.sizeShape.x > 0 && value.sizeShape.x <= 64 &&
            value.sizeShape.y > 0 && value.sizeShape.y <= 64 &&
            value.sizeShape.z >= 0 && value.sizeShape.z <= static_cast<float>(ParticleShape::Ring) &&
            value.sizeShape.z == std::floor(value.sizeShape.z) &&
            value.sizeShape.w >= -1 && value.sizeShape.w <= 1 &&
            value.axisFacing.w >= 0 && value.axisFacing.w <= static_cast<float>(ParticleFacing::Axis) &&
            value.axisFacing.w == std::floor(value.axisFacing.w) &&
            (value.axisFacing.w == 0 || Axis({value.axisFacing.x, value.axisFacing.y, value.axisFacing.z})) &&
            (value.axisFacing.w != static_cast<float>(ParticleFacing::Horizontal) ||
                static_cast<double>(value.axisFacing.x) * value.axisFacing.x +
                static_cast<double>(value.axisFacing.z) * value.axisFacing.z > 1e-10);
    }

    bool ParticleSystem::Initialize(std::uint32_t capacity)
    {
        if (capacity == 0 || capacity > kMaxParticleInstances)
        {
            return false;
        }
        try
        {
            _reserved.reserve(kMaxParticleBurst);
        }
        catch (...)
        {
            return false;
        }
        if (!_pool.Initialize(capacity))
        {
            return false;
        }
        _stats = {};
        return true;
    }

    std::uint32_t ParticleSystem::Emit(std::span<const ParticleSpawn> burst)
    {
        if (burst.empty())
        {
            return 0;
        }
        if (burst.size() > kMaxParticleBurst ||
            !std::all_of(burst.begin(), burst.end(), [](const ParticleSpawn& value) { return Valid(value); }))
        {
            _stats.invalid += burst.size();
            return 0;
        }
        const auto count = std::min<std::uint32_t>(static_cast<std::uint32_t>(burst.size()), _pool.GetStats().free);
        _stats.dropped += burst.size() - count;
        if (count == 0 || !_pool.Reserve(count, _reserved))
        {
            return 0;
        }
        for (std::uint32_t index = 0; index < count; ++index)
        {
            auto spawn = burst[index];
            if (spawn.facing != ParticleFacing::Camera)
            {
                const double length = std::sqrt(static_cast<double>(spawn.axis.x) * spawn.axis.x +
                    static_cast<double>(spawn.axis.y) * spawn.axis.y + static_cast<double>(spawn.axis.z) * spawn.axis.z);
                spawn.axis = {static_cast<float>(spawn.axis.x / length), static_cast<float>(spawn.axis.y / length),
                    static_cast<float>(spawn.axis.z / length)};
            }
            *_pool.Reserved(_reserved[index]) = {spawn, 0};
        }
        if (!_pool.Commit(_reserved))
        {
            _pool.Cancel(_reserved);
            return 0;
        }
        _stats.emitted += count;
        _stats.peak = std::max(_stats.peak, _pool.GetStats().active);
        return count;
    }

    bool ParticleSystem::Tick(float seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0)
        {
            return false;
        }
        if (seconds == 0)
        {
            return true;
        }
        if (!_pool.ForEachActive([&](PoolHandle handle, Particle& particle)
            {
                particle.age += seconds;
                if (particle.age >= particle.spawn.lifetime)
                {
                    _pool.RequestRelease(handle);
                    return;
                }
                auto& spawn = particle.spawn;
                const float halfSquared = 0.5f * seconds * seconds;
                spawn.position.x += spawn.velocity.x * seconds + spawn.acceleration.x * halfSquared;
                spawn.position.y += spawn.velocity.y * seconds + spawn.acceleration.y * halfSquared;
                spawn.position.z += spawn.velocity.z * seconds + spawn.acceleration.z * halfSquared;
                const float damping = std::exp(-spawn.drag * seconds);
                spawn.velocity = {(spawn.velocity.x + spawn.acceleration.x * seconds) * damping,
                    (spawn.velocity.y + spawn.acceleration.y * seconds) * damping,
                    (spawn.velocity.z + spawn.acceleration.z * seconds) * damping};
                spawn.rotation = std::remainder(spawn.rotation + spawn.angularVelocity * seconds,
                    2.0f * std::numbers::pi_v<float>);
                if (!Bounded(spawn.position, 1e6f) || !Finite(spawn.velocity))
                {
                    _pool.RequestRelease(handle);
                }
            }))
        {
            return false;
        }
        return _pool.FlushReleases([](PoolHandle, Particle& value) noexcept { value = {}; });
    }

    void ParticleSystem::Clear() noexcept
    {
        if (_pool.Clear([](PoolHandle, Particle& value) noexcept { value = {}; }))
        {
            _stats = {};
        }
    }

    bool ParticleSystem::Append(RenderFrame& frame) const
    {
        const auto active = _pool.GetStats().active;
        if (frame.particles.size() > kMaxParticleInstances ||
            active > kMaxParticleInstances - frame.particles.size())
        {
            return false;
        }
        // Frame storage is retained by RenderFrame::Clear; allocate its full capacity only once.
        const auto capacity = std::min<std::size_t>(kMaxParticleInstances,
            frame.particles.size() + _pool.GetStats().capacity);
        if (frame.particles.capacity() < capacity)
        {
            frame.particles.reserve(capacity);
        }
        return _pool.ForEachActive([&](PoolHandle, const Particle& particle)
            {
                const auto& s = particle.spawn;
                const float t = std::clamp(particle.age / s.lifetime, 0.0f, 1.0f);
                frame.particles.push_back({{s.position.x, s.position.y, s.position.z, s.rotation},
                    {Lerp(s.startSize.x, s.endSize.x, t) * 0.5f, Lerp(s.startSize.y, s.endSize.y, t) * 0.5f,
                        static_cast<float>(s.shape), s.parameter},
                    {Lerp(s.startColor.x, s.endColor.x, t), Lerp(s.startColor.y, s.endColor.y, t),
                        Lerp(s.startColor.z, s.endColor.z, t), Lerp(s.startColor.w, s.endColor.w, t)},
                    {s.axis.x, s.axis.y, s.axis.z, static_cast<float>(s.facing)}});
            });
    }

    ParticleStats ParticleSystem::Stats() const noexcept
    {
        auto result = _stats;
        result.active = _pool.GetStats().active;
        result.capacity = _pool.GetStats().capacity;
        return result;
    }
}
