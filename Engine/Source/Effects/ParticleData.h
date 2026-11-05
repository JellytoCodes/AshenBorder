#pragma once

#include "Types/Math.h"

#include <cstdint>

namespace Engine
{
    inline constexpr std::uint32_t kMaxParticleInstances = 4096;
    inline constexpr std::uint32_t kMaxParticleBurst = 64;

    enum class ParticleShape : std::uint32_t
    {
        Shard,
        Glow,
        Arc,
        Ash,
        Streak,
        Ring
    };

    enum class ParticleFacing : std::uint32_t
    {
        Camera,
        Horizontal,
        Axis
    };

    struct ParticleSpawn
    {
        Vector3 position = {};
        Vector3 velocity = {};
        Vector3 acceleration = {};
        Vector3 axis = {0, 0, 1};
        Vector4 startColor = {1, 1, 1, 1};
        Vector4 endColor = {1, 1, 1, 0};
        Vector2 startSize = {0.1f, 0.1f};
        Vector2 endSize = {0.01f, 0.01f};
        float lifetime = 0.25f;
        float rotation = 0;
        float angularVelocity = 0;
        float drag = 0;
        float parameter = 0.5f;
        ParticleShape shape = ParticleShape::Shard;
        ParticleFacing facing = ParticleFacing::Camera;
    };

    // Four float4 values are the per-instance vertex stream shared with Particle.hlsl.
    struct ParticleDraw
    {
        Vector4 positionRotation = {};
        Vector4 sizeShape = {};
        Vector4 color = {};
        Vector4 axisFacing = {};
    };

    struct ParticleStats
    {
        std::uint32_t active = 0;
        std::uint32_t capacity = 0;
        std::uint32_t peak = 0;
        std::uint64_t emitted = 0;
        std::uint64_t dropped = 0;
        std::uint64_t invalid = 0;
    };

    bool ValidParticleDraw(const ParticleDraw& value) noexcept;
}
