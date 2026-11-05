#pragma once

#include "Effects/ParticleData.h"
#include "Entity/ObjectPool.h"

#include <span>
#include <vector>

namespace Engine
{
    struct RenderFrame;

    class ParticleSystem
    {
    public:
        bool Initialize(std::uint32_t capacity);
        std::uint32_t Emit(std::span<const ParticleSpawn> burst);
        bool Tick(float seconds);
        void Clear() noexcept;
        bool Append(RenderFrame& frame) const;
        ParticleStats Stats() const noexcept;

    private:
        struct Particle
        {
            ParticleSpawn spawn;
            float age = 0;
        };

        ObjectPool<Particle> _pool;
        std::vector<PoolHandle> _reserved;
        ParticleStats _stats;
    };
}
