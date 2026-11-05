#pragma once

#include "Effects/ParticleSystem.h"
#include "Game/PoolEntities.h"

#include <cstdint>
#include <vector>

namespace Client
{
    class LastFieldRun;
    struct GameDefinitions;

    class CombatEffects
    {
    public:
        bool Initialize(std::uint32_t projectileCapacity);
        void Reset(std::uint64_t runEpoch) noexcept;
        void SetEnabled(bool enabled) noexcept;
        bool Advance(float seconds, const LastFieldRun& run);
        void OnEvent(const GameEvent& event, const LastFieldRun& run, const GameDefinitions& definitions);
        bool Append(Engine::RenderFrame& frame) const;
        Engine::ParticleStats Stats() const noexcept;

    private:
        struct Trail
        {
            Engine::PoolHandle handle;
            Engine::Vector3 position = {};
            bool valid = false;
        };

        float Random(float minimum, float maximum) noexcept;
        void Impact(Engine::Vector3 position, std::uint32_t count, bool warm);
        void Ash(Engine::Vector3 position, std::uint32_t count, float height);
        void Ring(Engine::Vector3 position, float size, bool warm);

        Engine::ParticleSystem _particles;
        std::vector<Trail> _trails;
        std::uint64_t _runEpoch = 0;
        std::uint64_t _lastSequence = 0, _lastTick = 0;
        std::uint32_t _randomState = 1;
        Engine::Vector3 _previousPlayer = {};
        float _stepDistance = 0;
        bool _hasPlayer = false;
        bool _enabled = true;
    };
}
