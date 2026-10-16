#pragma once

#include "Game/GameDefinitions.h"
#include "Game/PoolEntities.h"
#include "Game/RunProgression.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine
{
    struct InputState;
    struct UiFrame;
}

namespace Client
{
    struct HudWeaponSlot
    {
        WeaponKind kind = WeaponKind::Melee;
        double cooldown = 0;
        float period = 1;
    };

    struct HudSnapshot
    {
        bool gameMode = false;
        bool showStart = false;
        bool paused = false;
        bool muted = false;
        std::string message;
        std::uint64_t runEpoch = 0;
        PlayerState player;
        PlayerDefinition playerStats;
        WaveSnapshot wave;
        ProgressionSnapshot progression;
        std::vector<HudWeaponSlot> weapons;
        std::shared_ptr<const GameDefinitions> definitions;
    };

    enum class HudCommandKind
    {
        None,
        Start,
        Choose,
        Skip,
        Buy,
        Reroll,
        NextWave,
        Restart,
        TogglePause,
        ToggleMute
    };

    struct HudCommand
    {
        HudCommandKind kind = HudCommandKind::None;
        std::uint64_t runEpoch = 0;
        std::uint64_t choiceId = 0;
        std::uint64_t revision = 0;
        std::uint64_t offerId = 0;
        std::uint32_t index = 0;
    };

    void BuildHud(const HudSnapshot& snapshot, Engine::UiFrame& out,
        std::uint32_t width = 1280, std::uint32_t height = 720);
    HudCommand ReadHudCommand(const HudSnapshot& snapshot, const Engine::InputState& input,
        std::uint32_t width, std::uint32_t height);
}
