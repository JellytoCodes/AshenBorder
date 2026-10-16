#pragma once

#include <cstdint>

namespace Engine
{
    struct PoolHandle
    {
        std::uint64_t storageId = 0;
        std::uint32_t index = 0;
        std::uint64_t generation = 0;

        bool operator==(const PoolHandle&) const noexcept = default;
    };

    enum class SlotState : std::uint8_t
    {
        Free,
        Reserved,
        Active,
        PendingRelease,
        Retired
    };

    struct PoolStats
    {
        std::uint32_t capacity = 0;
        std::uint32_t free = 0;
        std::uint32_t reserved = 0;
        std::uint32_t active = 0;
        std::uint32_t pending = 0;
        std::uint32_t retired = 0;
        std::uint32_t highWater = 0;
        std::uint64_t reserveFailures = 0;
        std::uint64_t releases = 0;
    };

    namespace Detail
    {
        std::uint64_t AllocatePoolStorageId() noexcept;
    }
}
