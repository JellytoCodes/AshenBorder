#include "Entity/PoolHandle.h"

#include <atomic>
#include <limits>

namespace Engine::Detail
{
    std::uint64_t AllocatePoolStorageId() noexcept
    {
        static std::atomic<std::uint64_t> lastIssued = 0;
        std::uint64_t current = lastIssued.load(std::memory_order_relaxed);

        while (current != std::numeric_limits<std::uint64_t>::max())
        {
            const std::uint64_t next = current + 1;

            if (lastIssued.compare_exchange_weak(current, next,
                std::memory_order_relaxed, std::memory_order_relaxed))
            {
                return next;
            }
        }

        return 0;
    }
}
