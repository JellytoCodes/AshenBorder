#pragma once

#include "Entity/PoolHandle.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace Engine
{
    struct ObjectPoolTestAccess;

    // Single-threaded storage. Pointers are short-lived; keep handles across operations.
    template<typename T>
    class ObjectPool
    {
        static_assert(std::is_default_constructible_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>);

    public:
        ObjectPool() = default;
        ObjectPool(const ObjectPool&) = delete;
        ObjectPool& operator=(const ObjectPool&) = delete;
        ObjectPool(ObjectPool&&) = delete;
        ObjectPool& operator=(ObjectPool&&) = delete;

        bool Initialize(std::uint32_t capacity) noexcept
        {
            if (_operation != Operation::Idle || capacity == 0)
            {
                return false;
            }

            _operation = Operation::Initializing;
            bool initialized = false;

            try
            {
                // Stage every allocation and T constructor before replacing the old pool.
                std::vector<std::uint32_t> free;
                std::vector<PoolHandle> active;
                std::vector<PoolHandle> pending;
                std::vector<std::uint8_t> batchMarks(capacity, 0);
                std::vector<std::uint8_t> checkMarks(capacity, 0);
                free.reserve(capacity);
                active.reserve(capacity);
                pending.reserve(capacity);

                for (std::uint32_t remaining = capacity; remaining != 0; --remaining)
                {
                    free.push_back(remaining - 1);
                }

                std::unique_ptr<Slot[]> slots = std::make_unique<Slot[]>(capacity);
                const std::uint64_t storageId = Detail::AllocatePoolStorageId();

                if (storageId != 0)
                {
                    _slots.swap(slots);
                    _free.swap(free);
                    _active.swap(active);
                    _pending.swap(pending);
                    _batchMarks.swap(batchMarks);
                    _checkMarks.swap(checkMarks);
                    _capacity = capacity;
                    _storageId = storageId;
                    _stats = {};
                    _stats.capacity = capacity;
                    _stats.free = capacity;
                    initialized = true;
                }
                // Old T values are destroyed here while mutating reentry is still blocked.
            }
            catch (...)
            {
                _operation = Operation::Idle;
                return false;
            }

            _operation = Operation::Idle;
            return initialized;
        }

        // Replaces out on success. Its capacity must already cover count.
        bool Reserve(std::uint32_t count, std::vector<PoolHandle>& out) noexcept
        {
            if (_operation != Operation::Idle)
            {
                return false;
            }

            if (count == 0 || count > _free.size() || out.capacity() < count)
            {
                IncrementSaturated(_stats.reserveFailures);
                return false;
            }

            out.clear();

            for (std::uint32_t index = 0; index < count; ++index)
            {
                const std::uint32_t slotIndex = _free.back();
                Slot& slot = _slots[slotIndex];
                _free.pop_back();
                slot.state = SlotState::Reserved;
                out.push_back(MakeHandle(slotIndex));
            }

            _stats.free -= count;
            _stats.reserved += count;
            UpdateHighWater();
            return true;
        }

        T* Reserved(PoolHandle handle) noexcept
        {
            return Matches(handle, SlotState::Reserved) ? &_slots[handle.index].value : nullptr;
        }

        bool Commit(std::span<const PoolHandle> batch) noexcept
        {
            if (_operation != Operation::Idle || !ValidateReservedBatch(batch))
            {
                return false;
            }

            for (PoolHandle handle : batch)
            {
                Slot& slot = _slots[handle.index];
                slot.state = SlotState::Active;
                slot.activePosition = static_cast<std::uint32_t>(_active.size());
                _active.push_back(handle);
            }

            const std::uint32_t count = static_cast<std::uint32_t>(batch.size());
            _stats.reserved -= count;
            _stats.active += count;
            return true;
        }

        // The caller resets Reserved values before cancellation; T stays constructed.
        bool Cancel(std::span<const PoolHandle> batch) noexcept
        {
            if (_operation != Operation::Idle || !ValidateReservedBatch(batch))
            {
                return false;
            }

            for (PoolHandle handle : batch)
            {
                --_stats.reserved;
                ReturnSlot(handle.index);
            }

            return true;
        }

        T* TryGet(PoolHandle handle) noexcept
        {
            return Matches(handle, SlotState::Active) ? &_slots[handle.index].value : nullptr;
        }

        const T* TryGet(PoolHandle handle) const noexcept
        {
            return Matches(handle, SlotState::Active) ? &_slots[handle.index].value : nullptr;
        }

        template<typename F>
        bool ForEachActive(F&& visitor)
        {
            if (_operation != Operation::Idle)
            {
                return false;
            }

            _operation = Operation::Iterating;

            try
            {
                for (PoolHandle handle : _active)
                {
                    if (Matches(handle, SlotState::Active))
                    {
                        std::invoke(visitor, handle, _slots[handle.index].value);
                    }
                }
            }
            catch (...)
            {
                _operation = Operation::Idle;
                throw;
            }

            _operation = Operation::Idle;
            return true;
        }

        template<typename F>
        bool ForEachActive(F&& visitor) const
        {
            if (_operation != Operation::Idle)
            {
                return false;
            }

            _operation = Operation::Iterating;

            try
            {
                for (PoolHandle handle : _active)
                {
                    if (Matches(handle, SlotState::Active))
                    {
                        const T& value = _slots[handle.index].value;
                        std::invoke(visitor, handle, value);
                    }
                }
            }
            catch (...)
            {
                _operation = Operation::Idle;
                throw;
            }

            _operation = Operation::Idle;
            return true;
        }

        bool RequestRelease(PoolHandle handle) noexcept
        {
            if ((_operation != Operation::Idle && _operation != Operation::Iterating)
                || !Matches(handle, SlotState::Active))
            {
                return false;
            }

            _slots[handle.index].state = SlotState::PendingRelease;
            _pending.push_back(handle);
            --_stats.active;
            ++_stats.pending;
            return true;
        }

        template<typename F>
            requires std::is_nothrow_invocable_v<F&, PoolHandle, T&>
        bool FlushReleases(F&& beforeRelease) noexcept
        {
            if (_operation != Operation::Idle)
            {
                return false;
            }

            _operation = Operation::Flushing;
            FlushPending(beforeRelease);
            _operation = Operation::Idle;
            return true;
        }

        template<typename F>
            requires std::is_nothrow_invocable_v<F&, PoolHandle, T&>
        bool Clear(F&& beforeRelease) noexcept
        {
            if (_operation != Operation::Idle)
            {
                return false;
            }

            _operation = Operation::Clearing;

            // Hide all active values before any cleanup callback observes the pool.
            for (PoolHandle handle : _active)
            {
                Slot& slot = _slots[handle.index];

                if (slot.state == SlotState::Active)
                {
                    slot.state = SlotState::PendingRelease;
                    _pending.push_back(handle);
                    --_stats.active;
                    ++_stats.pending;
                }
            }

            FlushPending(beforeRelease);

            for (std::uint32_t index = 0; index < _capacity; ++index)
            {
                Slot& slot = _slots[index];

                if (slot.state == SlotState::Reserved)
                {
                    std::invoke(beforeRelease, MakeHandle(index), slot.value);
                    --_stats.reserved;
                    ReturnSlot(index);
                }
            }

            _operation = Operation::Idle;
            return true;
        }

        PoolStats GetStats() const noexcept
        {
            return _stats;
        }

        bool CheckInvariants() const noexcept
        {
            if (_capacity == 0)
            {
                return _storageId == 0 && !_slots && _free.empty() && _active.empty()
                    && _pending.empty() && _batchMarks.empty() && _checkMarks.empty()
                    && _stats.capacity == 0 && _stats.free == 0 && _stats.reserved == 0
                    && _stats.active == 0 && _stats.pending == 0 && _stats.retired == 0
                    && _stats.highWater == 0 && _stats.releases == 0;
            }

            const std::uint64_t total = static_cast<std::uint64_t>(_stats.free)
                + _stats.reserved + _stats.active + _stats.pending + _stats.retired;
            const std::uint64_t occupied = static_cast<std::uint64_t>(_stats.reserved)
                + _stats.active + _stats.pending;

            if (_storageId == 0 || !_slots || _stats.capacity != _capacity || total != _capacity
                || _free.size() != _stats.free
                || _active.size() != static_cast<std::size_t>(_stats.active) + _stats.pending
                || _pending.size() != _stats.pending || _free.capacity() < _capacity
                || _active.capacity() < _capacity || _pending.capacity() < _capacity
                || _batchMarks.size() != _capacity || _checkMarks.size() != _capacity
                || _stats.highWater < occupied || _stats.highWater > _capacity)
            {
                return false;
            }

            std::fill(_checkMarks.begin(), _checkMarks.end(), std::uint8_t{0});

            for (std::uint32_t index : _free)
            {
                if (index >= _capacity || _slots[index].state != SlotState::Free
                    || _checkMarks[index] != 0)
                {
                    return false;
                }

                _checkMarks[index] = 1;
            }

            for (std::size_t position = 0; position < _active.size(); ++position)
            {
                const PoolHandle handle = _active[position];

                if (!Matches(handle, SlotState::Active) && !Matches(handle, SlotState::PendingRelease))
                {
                    return false;
                }

                if (_checkMarks[handle.index] != 0 || _slots[handle.index].activePosition != position)
                {
                    return false;
                }

                _checkMarks[handle.index] = 2;
            }

            for (PoolHandle handle : _pending)
            {
                if (!Matches(handle, SlotState::PendingRelease) || _checkMarks[handle.index] != 2)
                {
                    return false;
                }

                _checkMarks[handle.index] = 6;
            }

            std::uint32_t counts[5] = {};

            for (std::uint32_t index = 0; index < _capacity; ++index)
            {
                const Slot& slot = _slots[index];
                const auto stateIndex = static_cast<std::uint32_t>(slot.state);

                if (stateIndex >= 5 || slot.generation == 0 || _batchMarks[index] != 0)
                {
                    return false;
                }

                const bool retired = slot.state == SlotState::Retired;
                const bool listed = slot.state == SlotState::Active || slot.state == SlotState::PendingRelease;

                if (retired != (slot.generation == std::numeric_limits<std::uint64_t>::max())
                    || (!listed && slot.activePosition != kInvalidIndex))
                {
                    return false;
                }

                const std::uint8_t expectedMarks[5] = {1, 0, 2, 6, 0};

                if (_checkMarks[index] != expectedMarks[stateIndex])
                {
                    return false;
                }

                ++counts[stateIndex];
            }

            return counts[0] == _stats.free && counts[1] == _stats.reserved
                && counts[2] == _stats.active && counts[3] == _stats.pending
                && counts[4] == _stats.retired;
        }

    private:
        friend struct ObjectPoolTestAccess;

        static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

        enum class Operation : std::uint8_t
        {
            Idle,
            Initializing,
            Iterating,
            Flushing,
            Clearing
        };

        struct Slot
        {
            T value{};
            std::uint64_t generation = 1;
            SlotState state = SlotState::Free;
            std::uint32_t activePosition = kInvalidIndex;
        };

        PoolHandle MakeHandle(std::uint32_t index) const noexcept
        {
            return {_storageId, index, _slots[index].generation};
        }

        bool Matches(PoolHandle handle, SlotState state) const noexcept
        {
            return handle.storageId != 0 && handle.storageId == _storageId && handle.index < _capacity
                && handle.generation != 0 && _slots[handle.index].generation == handle.generation
                && _slots[handle.index].state == state;
        }

        bool ValidateReservedBatch(std::span<const PoolHandle> batch) noexcept
        {
            if (batch.size() > _capacity)
            {
                return false;
            }

            std::size_t marked = 0;
            bool valid = true;

            for (PoolHandle handle : batch)
            {
                if (!Matches(handle, SlotState::Reserved) || _batchMarks[handle.index] != 0)
                {
                    valid = false;
                    break;
                }

                _batchMarks[handle.index] = 1;
                ++marked;
            }

            for (std::size_t index = 0; index < marked; ++index)
            {
                _batchMarks[batch[index].index] = 0;
            }

            return valid;
        }

        static void IncrementSaturated(std::uint64_t& value) noexcept
        {
            if (value != std::numeric_limits<std::uint64_t>::max())
            {
                ++value;
            }
        }

        void UpdateHighWater() noexcept
        {
            const std::uint32_t occupied = _stats.reserved + _stats.active + _stats.pending;
            _stats.highWater = std::max(_stats.highWater, occupied);
        }

        void ReturnSlot(std::uint32_t index) noexcept
        {
            Slot& slot = _slots[index];
            slot.activePosition = kInvalidIndex;

            if (slot.generation >= std::numeric_limits<std::uint64_t>::max() - 1)
            {
                slot.generation = std::numeric_limits<std::uint64_t>::max();
                slot.state = SlotState::Retired;
                ++_stats.retired;
            }
            else
            {
                ++slot.generation;
                slot.state = SlotState::Free;
                _free.push_back(index);
                ++_stats.free;
            }
        }

        void RemoveActive(std::uint32_t index) noexcept
        {
            const std::uint32_t position = _slots[index].activePosition;
            const PoolHandle last = _active.back();
            _active[position] = last;
            _slots[last.index].activePosition = position;
            _active.pop_back();
            _slots[index].activePosition = kInvalidIndex;
        }

        template<typename F>
        void FlushPending(F& beforeRelease) noexcept
        {
            // LIFO drainage and swap removal do not promise stable iteration order.
            while (!_pending.empty())
            {
                const PoolHandle handle = _pending.back();
                Slot& slot = _slots[handle.index];
                std::invoke(beforeRelease, handle, slot.value);
                _pending.pop_back();
                RemoveActive(handle.index);
                --_stats.pending;
                ReturnSlot(handle.index);
                IncrementSaturated(_stats.releases);
            }
        }

        std::unique_ptr<Slot[]> _slots;
        std::uint32_t _capacity = 0;
        std::uint64_t _storageId = 0;
        std::vector<std::uint32_t> _free;
        std::vector<PoolHandle> _active;
        std::vector<PoolHandle> _pending;
        std::vector<std::uint8_t> _batchMarks;
        mutable std::vector<std::uint8_t> _checkMarks;
        PoolStats _stats;
        mutable Operation _operation = Operation::Idle;
    };
}
