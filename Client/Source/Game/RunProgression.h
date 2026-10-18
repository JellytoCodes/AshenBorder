#pragma once

#include "Game/GameDefinitions.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Client
{
    enum class ProgressionPhase
    {
        Done,
        Selection,
        Shop
    };

    enum class ProgressionFailure
    {
        None,
        NotInitialized,
        WrongPhase,
        StaleEpoch,
        StaleChoice,
        InvalidIndex,
        EmptyCandidates,
        StaleShopRevision,
        UnknownOffer,
        AlreadyPurchased,
        InsufficientCurrency,
        FullSlots,
        MaxStacks,
        InvalidRewards,
        Overflow,
        PreparationFailed
    };

    struct ProgressionResult
    {
        bool accepted = false;
        ProgressionFailure reason = ProgressionFailure::None;
    };

    struct PassiveSource
    {
        std::uint64_t sourceId = 0;
        std::uint32_t passiveId = 0;

        bool operator==(const PassiveSource&) const = default;
    };

    struct ShopOffer
    {
        std::uint64_t offerId = 0;
        ShopItemKind kind = ShopItemKind::Weapon;
        std::uint32_t definitionId = 0;
        std::uint64_t price = 0;
        bool purchased = false;

        bool operator==(const ShopOffer&) const = default;
    };

    struct ProgressionSnapshot
    {
        std::uint64_t runEpoch = 0;
        ProgressionPhase phase = ProgressionPhase::Done;
        std::uint32_t level = 1;
        std::uint32_t pendingChoices = 0;
        std::uint64_t acquiredXp = 0;
        std::uint64_t acquiredCurrency = 0;
        std::uint64_t spentCurrency = 0;
        std::uint64_t balance = 0;
        std::uint64_t nextLevelXp = 0; // 0 means the explicit XP table is exhausted.
        std::uint64_t choiceId = 0;
        std::vector<std::uint32_t> choices;
        std::uint64_t shopRevision = 0;
        std::uint32_t intermissionIndex = 0;
        std::uint64_t currentRerollCost = 0;
        std::vector<ShopOffer> offers;
        std::vector<PassiveSource> passiveSources;
        std::vector<std::uint32_t> loadout;
        PlayerDefinition finalPlayer;
        std::uint64_t emptyChoiceFailures = 0;
    };

    struct RunProgressionTestAccess;

    class RunProgression
    {
    public:
        bool Initialize(std::shared_ptr<const GameDefinitions> definitions, std::uint64_t runEpoch,
            std::span<const std::uint32_t> loadout = {}, std::uint64_t seed = 42);
        bool SyncRewards(std::uint64_t acquiredXp, std::uint64_t acquiredCurrency) noexcept;

        ProgressionResult BeginIntermission();
        ProgressionResult Choose(std::uint64_t runEpoch, std::uint64_t choiceId, std::uint32_t index);
        ProgressionResult SkipChoice(std::uint64_t runEpoch, std::uint64_t choiceId);
        ProgressionResult Buy(std::uint64_t runEpoch, std::uint64_t revision, std::uint64_t offerId);
        ProgressionResult Reroll(std::uint64_t runEpoch, std::uint64_t revision);
        ProgressionResult FinishShop(std::uint64_t runEpoch, std::uint64_t revision) noexcept;

        ProgressionSnapshot Snapshot() const;
        ProgressionPhase Phase() const noexcept;
        const PlayerDefinition& FinalPlayer() const noexcept;
        std::span<const std::uint32_t> Loadout() const noexcept;
        std::uint64_t Balance() const noexcept;

    private:
        friend struct RunProgressionTestAccess;

        struct State
        {
            ProgressionPhase phase = ProgressionPhase::Done;
            std::uint32_t level = 1;
            std::uint32_t pendingChoices = 0;
            std::uint64_t acquiredXp = 0;
            std::uint64_t acquiredCurrency = 0;
            std::uint64_t spentCurrency = 0;
            std::uint64_t choiceId = 0;
            std::vector<std::uint32_t> choices;
            std::uint64_t shopRevision = 0;
            std::uint32_t intermissionIndex = 0;
            std::uint64_t currentRerollCost = 0;
            std::vector<ShopOffer> offers;
            std::vector<PassiveSource> passiveSources;
            std::vector<std::uint32_t> loadout;
            PlayerDefinition finalPlayer;
            std::uint64_t nextSourceId = 1;
            std::uint64_t nextOfferId = 1;
            std::uint64_t rngState = 42;
            std::uint64_t emptyChoiceFailures = 0;
        };

        ProgressionFailure Check(std::uint64_t runEpoch, ProgressionPhase phase) const noexcept;
        bool PrepareChoice(State& state) const;
        bool PrepareShop(State& state) const;
        std::uint32_t PricePercent(const State& state) const noexcept;
        bool Recalculate(State& state) const;
        bool AddPassive(State& state, std::uint32_t id) const;
        std::uint32_t StackCount(const State& state, std::uint32_t id) const noexcept;
        void Commit(State& state) noexcept;

        std::shared_ptr<const GameDefinitions> _definitions;
        std::uint64_t _runEpoch = 0;
        State _state;
    };
}
