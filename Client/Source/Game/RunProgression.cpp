#include "Game/RunProgression.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>

namespace
{
    Client::ProgressionResult Reject(Client::ProgressionFailure reason) noexcept
    {
        return {false, reason};
    }

    double RandomUnit(std::uint64_t& state) noexcept
    {
        state += 0x9e3779b97f4a7c15ULL;
        auto value = state;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        value ^= value >> 31;
        return static_cast<double>(value >> 11) * (1.0 / 9007199254740992.0);
    }
}

namespace Client
{
    bool RunProgression::Initialize(std::shared_ptr<const GameDefinitions> definitions,
        std::uint64_t runEpoch, std::span<const std::uint32_t> loadout, std::uint64_t seed)
    {
        try
        {
            std::string error;
            if (!definitions || runEpoch == 0 || definitions->schemaVersion < 2 ||
                !ValidateGameDefinitions(*definitions, error))
            {
                return false;
            }
            if (loadout.empty())
            {
                loadout = definitions->loadout;
            }
            if (loadout.empty() || loadout.size() > definitions->slotLimit ||
                std::any_of(loadout.begin(), loadout.end(), [&](std::uint32_t id)
                {
                    return definitions->FindWeapon(id) == nullptr;
                }))
            {
                return false;
            }
            State candidate;
            candidate.loadout.assign(loadout.begin(), loadout.end());
            candidate.finalPlayer = definitions->player;
            candidate.currentRerollCost = definitions->shop.rerollCost;
            candidate.rngState = seed;
            _definitions = std::move(definitions);
            _runEpoch = runEpoch;
            Commit(candidate);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool RunProgression::SyncRewards(std::uint64_t acquiredXp, std::uint64_t acquiredCurrency) noexcept
    {
        if (!_definitions || acquiredXp < _state.acquiredXp || acquiredCurrency < _state.acquiredCurrency ||
            acquiredCurrency < _state.spentCurrency)
        {
            return false;
        }
        const auto& thresholds = _definitions->progression.cumulativeXp;
        const auto reached = static_cast<std::uint32_t>(
            std::upper_bound(thresholds.begin(), thresholds.end(), acquiredXp) - thresholds.begin());
        const auto level = reached + 1;
        const auto added = level - _state.level;
        if (added > std::numeric_limits<std::uint32_t>::max() - _state.pendingChoices)
        {
            return false;
        }
        _state.acquiredXp = acquiredXp;
        _state.acquiredCurrency = acquiredCurrency;
        _state.level = level;
        _state.pendingChoices += added;
        return true;
    }

    ProgressionFailure RunProgression::Check(std::uint64_t runEpoch, ProgressionPhase phase) const noexcept
    {
        if (!_definitions)
        {
            return ProgressionFailure::NotInitialized;
        }
        if (runEpoch != _runEpoch)
        {
            return ProgressionFailure::StaleEpoch;
        }
        return _state.phase == phase ? ProgressionFailure::None : ProgressionFailure::WrongPhase;
    }

    std::uint32_t RunProgression::StackCount(const State& state, std::uint32_t id) const noexcept
    {
        return static_cast<std::uint32_t>(std::count_if(state.passiveSources.begin(),
            state.passiveSources.end(), [id](const PassiveSource& source)
            {
                return source.passiveId == id;
            }));
    }

    bool RunProgression::PrepareChoice(State& state) const
    {
        if (state.choiceId == std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
        std::vector<const PassiveDefinition*> candidates;
        for (const auto& passive : _definitions->passives)
        {
            if (passive.weight > 0 && StackCount(state, passive.id) < passive.maxStacks)
            {
                candidates.push_back(&passive);
            }
        }
        state.choices.clear();
        const auto count = std::min<std::size_t>(_definitions->progression.choiceCount, candidates.size());
        state.choices.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            double sum = 0;
            for (const auto* passive : candidates)
            {
                sum += passive->weight;
            }
            const double sample = RandomUnit(state.rngState) * sum;
            double cumulative = 0;
            std::size_t selected = candidates.size() - 1;
            for (std::size_t j = 0; j < candidates.size(); ++j)
            {
                cumulative += candidates[j]->weight;
                if (sample < cumulative)
                {
                    selected = j;
                    break;
                }
            }
            state.choices.push_back(candidates[selected]->id);
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(selected));
        }
        if (state.choices.empty())
        {
            if (state.emptyChoiceFailures == std::numeric_limits<std::uint64_t>::max())
            {
                return false;
            }
            ++state.emptyChoiceFailures;
        }
        ++state.choiceId;
        state.phase = ProgressionPhase::Selection;
        return true;
    }

    std::uint32_t RunProgression::PricePercent(const State& state) const noexcept
    {
        const auto& percentages = _definitions->shop.stagePricePercent;
        return percentages.empty() ? 100 : percentages[std::min<std::size_t>(
            state.intermissionIndex > 0 ? state.intermissionIndex - 1 : 0, percentages.size() - 1)];
    }

    bool RunProgression::PrepareShop(State& state) const
    {
        std::vector<const ShopItemDefinition*> candidates;
        double sum = 0;
        for (const auto& item : _definitions->shop.items)
        {
            const bool capacity = item.kind == ShopItemKind::Weapon ? state.loadout.size() < _definitions->slotLimit :
                StackCount(state, item.definitionId) < _definitions->FindPassive(item.definitionId)->maxStacks;
            if (item.weight > 0 && capacity)
            {
                candidates.push_back(&item);
                sum += item.weight;
            }
        }
        const auto count = candidates.empty() ? 0 : _definitions->shop.offerCount;
        if (state.shopRevision == std::numeric_limits<std::uint64_t>::max() ||
            count > std::numeric_limits<std::uint64_t>::max() - state.nextOfferId)
        {
            return false;
        }
        std::vector<ShopOffer> offers;
        offers.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const double sample = RandomUnit(state.rngState) * sum;
            double cumulative = 0;
            const ShopItemDefinition* selected = nullptr;
            for (const auto* item : candidates)
            {
                selected = item;
                cumulative += item->weight;
                if (sample < cumulative)
                {
                    break;
                }
            }
            if (!selected)
            {
                return false;
            }
            std::uint64_t price = 0;
            if (!ScaleShopPrice(selected->price, PricePercent(state), price))
            {
                return false;
            }
            offers.push_back({state.nextOfferId++, selected->kind, selected->definitionId, price, false});
        }
        state.offers.swap(offers);
        state.choices.clear();
        ++state.shopRevision;
        state.phase = ProgressionPhase::Shop;
        return true;
    }

    bool RunProgression::Recalculate(State& state) const
    {
        auto sources = state.passiveSources;
        std::sort(sources.begin(), sources.end(), [](const PassiveSource& a, const PassiveSource& b)
        {
            return a.sourceId < b.sourceId;
        });
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            if (sources[i].sourceId == 0 || (i != 0 && sources[i - 1].sourceId == sources[i].sourceId))
            {
                return false;
            }
        }
        std::sort(sources.begin(), sources.end(), [](const PassiveSource& a, const PassiveSource& b)
        {
            return a.passiveId == b.passiveId ? a.sourceId < b.sourceId : a.passiveId < b.passiveId;
        });
        std::array<double, 6> flat{};
        std::array<double, 6> percent{};
        std::uint32_t stacks = 0;
        std::uint32_t previous = 0;
        for (const auto& source : sources)
        {
            const auto* passive = _definitions->FindPassive(source.passiveId);
            if (!passive)
            {
                return false;
            }
            stacks = source.passiveId == previous ? stacks + 1 : 1;
            previous = source.passiveId;
            if (stacks > passive->maxStacks)
            {
                return false;
            }
            for (const auto& modifier : passive->modifiers)
            {
                const auto index = static_cast<std::size_t>(modifier.stat);
                (modifier.operation == ModifierOperation::Flat ? flat : percent)[index] += modifier.value;
            }
        }
        const auto& base = _definitions->player;
        const std::array<double, 6> values{base.maxHp, base.speed, base.armor,
            base.damageMultiplier, base.attackSpeed, base.pickupRadius};
        constexpr std::array<double, 6> minimum{1, 0, 0, 0, 0.1, 0};
        constexpr std::array<double, 6> maximum{1000000, 120, 1000000, 1000000, 10, 1000000};
        std::array<float, 6> result{};
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            const double value = (values[i] + flat[i]) * (1 + percent[i]);
            if (!std::isfinite(value))
            {
                return false;
            }
            result[i] = static_cast<float>(std::clamp(value, minimum[i], maximum[i]));
        }
        state.finalPlayer = base;
        state.finalPlayer.maxHp = result[0];
        state.finalPlayer.speed = result[1];
        state.finalPlayer.armor = result[2];
        state.finalPlayer.damageMultiplier = result[3];
        state.finalPlayer.attackSpeed = result[4];
        state.finalPlayer.pickupRadius = result[5];
        return true;
    }

    bool RunProgression::AddPassive(State& state, std::uint32_t id) const
    {
        if (state.nextSourceId == std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
        state.passiveSources.push_back({state.nextSourceId++, id});
        return Recalculate(state);
    }

    void RunProgression::Commit(State& state) noexcept
    {
        static_assert(noexcept(std::swap(_state, state)));
        std::swap(_state, state);
    }

    ProgressionResult RunProgression::BeginIntermission()
    {
        const auto failure = Check(_runEpoch, ProgressionPhase::Done);
        if (failure != ProgressionFailure::None)
        {
            return Reject(failure);
        }
        try
        {
            auto candidate = _state;
            if (candidate.intermissionIndex == std::numeric_limits<std::uint32_t>::max())
            {
                return Reject(ProgressionFailure::Overflow);
            }
            ++candidate.intermissionIndex;
            if (!ScaleShopPrice(_definitions->shop.rerollCost, PricePercent(candidate), candidate.currentRerollCost))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            if (!(candidate.pendingChoices > 0 ? PrepareChoice(candidate) : PrepareShop(candidate)))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            Commit(candidate);
            return {true, ProgressionFailure::None};
        }
        catch (const std::exception&)
        {
            return Reject(ProgressionFailure::PreparationFailed);
        }
    }

    ProgressionResult RunProgression::Choose(std::uint64_t runEpoch, std::uint64_t choiceId,
        std::uint32_t index)
    {
        const auto failure = Check(runEpoch, ProgressionPhase::Selection);
        if (failure != ProgressionFailure::None)
        {
            return Reject(failure);
        }
        if (choiceId != _state.choiceId)
        {
            return Reject(ProgressionFailure::StaleChoice);
        }
        if (index >= _state.choices.size())
        {
            return Reject(_state.choices.empty() ? ProgressionFailure::EmptyCandidates : ProgressionFailure::InvalidIndex);
        }
        const auto id = _state.choices[index];
        const auto* passive = _definitions->FindPassive(id);
        if (!passive || StackCount(_state, id) >= passive->maxStacks)
        {
            return Reject(ProgressionFailure::MaxStacks);
        }
        if (_state.nextSourceId == std::numeric_limits<std::uint64_t>::max())
        {
            return Reject(ProgressionFailure::Overflow);
        }
        try
        {
            auto candidate = _state;
            if (!AddPassive(candidate, id))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            --candidate.pendingChoices;
            if (!(candidate.pendingChoices > 0 ? PrepareChoice(candidate) : PrepareShop(candidate)))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            Commit(candidate);
            return {true, ProgressionFailure::None};
        }
        catch (const std::exception&)
        {
            return Reject(ProgressionFailure::PreparationFailed);
        }
    }

    ProgressionResult RunProgression::SkipChoice(std::uint64_t runEpoch, std::uint64_t choiceId)
    {
        const auto failure = Check(runEpoch, ProgressionPhase::Selection);
        if (failure != ProgressionFailure::None)
        {
            return Reject(failure);
        }
        if (choiceId != _state.choiceId)
        {
            return Reject(ProgressionFailure::StaleChoice);
        }
        if (!_state.choices.empty())
        {
            return Reject(ProgressionFailure::InvalidIndex);
        }
        try
        {
            auto candidate = _state;
            --candidate.pendingChoices;
            if (!(candidate.pendingChoices > 0 ? PrepareChoice(candidate) : PrepareShop(candidate)))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            Commit(candidate);
            return {true, ProgressionFailure::None};
        }
        catch (const std::exception&)
        {
            return Reject(ProgressionFailure::PreparationFailed);
        }
    }

    ProgressionResult RunProgression::Buy(std::uint64_t runEpoch, std::uint64_t revision,
        std::uint64_t offerId)
    {
        const auto failure = Check(runEpoch, ProgressionPhase::Shop);
        if (failure != ProgressionFailure::None)
        {
            return Reject(failure);
        }
        if (revision != _state.shopRevision)
        {
            return Reject(ProgressionFailure::StaleShopRevision);
        }
        const auto found = std::find_if(_state.offers.begin(), _state.offers.end(), [offerId](const ShopOffer& offer)
        {
            return offer.offerId == offerId;
        });
        if (found == _state.offers.end())
        {
            return Reject(ProgressionFailure::UnknownOffer);
        }
        if (found->purchased)
        {
            return Reject(ProgressionFailure::AlreadyPurchased);
        }
        if (found->price > Balance())
        {
            return Reject(ProgressionFailure::InsufficientCurrency);
        }
        if (found->kind == ShopItemKind::Weapon && _state.loadout.size() >= _definitions->slotLimit)
        {
            return Reject(ProgressionFailure::FullSlots);
        }
        if (found->kind == ShopItemKind::Passive &&
            StackCount(_state, found->definitionId) >= _definitions->FindPassive(found->definitionId)->maxStacks)
        {
            return Reject(ProgressionFailure::MaxStacks);
        }
        if (_state.shopRevision == std::numeric_limits<std::uint64_t>::max() ||
            (found->kind == ShopItemKind::Passive && _state.nextSourceId == std::numeric_limits<std::uint64_t>::max()))
        {
            return Reject(ProgressionFailure::Overflow);
        }
        try
        {
            auto candidate = _state;
            const auto index = static_cast<std::size_t>(found - _state.offers.begin());
            auto& offer = candidate.offers[index];
            if (offer.kind == ShopItemKind::Weapon)
            {
                candidate.loadout.push_back(offer.definitionId);
            }
            else if (!AddPassive(candidate, offer.definitionId))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            candidate.spentCurrency += offer.price; // price <= acquired - spent proves this cannot overflow.
            offer.purchased = true;
            ++candidate.shopRevision;
            Commit(candidate);
            return {true, ProgressionFailure::None};
        }
        catch (const std::exception&)
        {
            return Reject(ProgressionFailure::PreparationFailed);
        }
    }

    ProgressionResult RunProgression::Reroll(std::uint64_t runEpoch, std::uint64_t revision)
    {
        const auto failure = Check(runEpoch, ProgressionPhase::Shop);
        if (failure != ProgressionFailure::None)
        {
            return Reject(failure);
        }
        if (revision != _state.shopRevision)
        {
            return Reject(ProgressionFailure::StaleShopRevision);
        }
        if (_state.currentRerollCost > Balance())
        {
            return Reject(ProgressionFailure::InsufficientCurrency);
        }
        try
        {
            auto candidate = _state;
            if (!PrepareShop(candidate))
            {
                return Reject(ProgressionFailure::PreparationFailed);
            }
            if (candidate.offers.empty())
            {
                return Reject(ProgressionFailure::EmptyCandidates);
            }
            candidate.spentCurrency += _state.currentRerollCost;
            Commit(candidate);
            return {true, ProgressionFailure::None};
        }
        catch (const std::exception&)
        {
            return Reject(ProgressionFailure::PreparationFailed);
        }
    }

    ProgressionResult RunProgression::FinishShop(std::uint64_t runEpoch, std::uint64_t revision) noexcept
    {
        const auto failure = Check(runEpoch, ProgressionPhase::Shop);
        if (failure != ProgressionFailure::None)
        {
            return Reject(failure);
        }
        if (revision != _state.shopRevision)
        {
            return Reject(ProgressionFailure::StaleShopRevision);
        }
        if (_state.shopRevision == std::numeric_limits<std::uint64_t>::max())
        {
            return Reject(ProgressionFailure::Overflow);
        }
        _state.phase = ProgressionPhase::Done;
        _state.offers.clear();
        ++_state.shopRevision;
        return {true, ProgressionFailure::None};
    }

    ProgressionSnapshot RunProgression::Snapshot() const
    {
        ProgressionSnapshot result;
        result.runEpoch = _runEpoch;
        result.phase = _state.phase;
        result.level = _state.level;
        result.pendingChoices = _state.pendingChoices;
        result.acquiredXp = _state.acquiredXp;
        result.acquiredCurrency = _state.acquiredCurrency;
        result.spentCurrency = _state.spentCurrency;
        result.balance = Balance();
        if (_definitions && _state.level <= _definitions->progression.cumulativeXp.size())
        {
            result.nextLevelXp = _definitions->progression.cumulativeXp[_state.level - 1];
        }
        result.choiceId = _state.choiceId;
        result.choices = _state.choices;
        result.shopRevision = _state.shopRevision;
        result.intermissionIndex = _state.intermissionIndex;
        result.currentRerollCost = _state.currentRerollCost;
        result.offers = _state.offers;
        result.passiveSources = _state.passiveSources;
        result.loadout = _state.loadout;
        result.finalPlayer = _state.finalPlayer;
        result.emptyChoiceFailures = _state.emptyChoiceFailures;
        return result;
    }

    ProgressionPhase RunProgression::Phase() const noexcept
    {
        return _state.phase;
    }

    const PlayerDefinition& RunProgression::FinalPlayer() const noexcept
    {
        return _state.finalPlayer;
    }

    std::span<const std::uint32_t> RunProgression::Loadout() const noexcept
    {
        return _state.loadout;
    }

    std::uint64_t RunProgression::Balance() const noexcept
    {
        return _state.acquiredCurrency - _state.spentCurrency;
    }
}
