#pragma once

#include "Scene/Geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace Engine
{
    struct SpatialGridTestAccess;

    // XZ broad phase. Keys are values; exact geometry and key lifetime belong to the caller.
    // The grid and its const queries are single-threaded. No key hash/order is required.
    template<typename Key>
    class SpatialGrid
    {
        static_assert(std::is_nothrow_copy_constructible_v<Key>,
            "SpatialGrid output requires noexcept-copyable value keys");

    public:
        SpatialGrid() = default;
        SpatialGrid(const SpatialGrid&) = delete;
        SpatialGrid& operator=(const SpatialGrid&) = delete;
        SpatialGrid(SpatialGrid&&) = delete;
        SpatialGrid& operator=(SpatialGrid&&) = delete;

        bool Initialize(BoundsXZ bounds, float cellSize, std::uint32_t maxEntries) noexcept
        {
            if (!Finite(bounds.min) || !Finite(bounds.max) || bounds.min.x >= bounds.max.x
                || bounds.min.y >= bounds.max.y || !std::isfinite(cellSize) || cellSize <= 0
                || maxEntries == 0 || maxEntries > kMaximumMemberships)
            {
                return false;
            }

            const double columns = std::ceil((static_cast<double>(bounds.max.x) - bounds.min.x) / cellSize);
            const double rows = std::ceil((static_cast<double>(bounds.max.y) - bounds.min.y) / cellSize);

            if (!std::isfinite(columns) || !std::isfinite(rows) || columns < 1 || rows < 1
                || columns > kMaximumMemberships || rows > kMaximumMemberships)
            {
                return false;
            }

            const auto columnCount = static_cast<std::uint32_t>(columns);
            const auto rowCount = static_cast<std::uint32_t>(rows);
            const std::uint64_t cellCount = static_cast<std::uint64_t>(columnCount) * rowCount;

            if (cellCount > kMaximumMemberships / maxEntries)
            {
                return false;
            }

            try
            {
                auto entries = std::make_unique<Entry[]>(maxEntries);
                std::vector<std::uint32_t> cellCounts(static_cast<std::size_t>(cellCount), 0);
                std::vector<std::uint32_t> members(static_cast<std::size_t>(cellCount * maxEntries), kInvalidEntry);
                std::vector<std::uint8_t> queryMarks(maxEntries, 0);

                _entries.swap(entries);
                _cellCounts.swap(cellCounts);
                _members.swap(members);
                _queryMarks.swap(queryMarks);
                _bounds = bounds;
                _cellSize = cellSize;
                _columns = columnCount;
                _rows = rowCount;
                _maxEntries = maxEntries;
                _entryCount = 0;
            }
            catch (...)
            {
                return false;
            }

            return true;
        }

        void Clear() noexcept
        {
            for (std::uint32_t index = 0; index < _entryCount; ++index)
            {
                _entries[index].key.reset();
            }

            _entryCount = 0;
            std::fill(_cellCounts.begin(), _cellCounts.end(), std::uint32_t{0});
            std::fill(_members.begin(), _members.end(), kInvalidEntry);
            std::fill(_queryMarks.begin(), _queryMarks.end(), std::uint8_t{0});
        }

        bool Insert(const Key& key, Vector3 position, float radius) noexcept
        {
            if (_maxEntries == 0 || _entryCount == _maxEntries || !Finite(position)
                || !std::isfinite(radius) || radius < 0 || !Contains(position))
            {
                return false;
            }

            // Equality may throw. Finish every comparison before changing the grid.
            try
            {
                for (std::uint32_t index = 0; index < _entryCount; ++index)
                {
                    if (*_entries[index].key == key)
                    {
                        return false;
                    }
                }
            }
            catch (...)
            {
                return false;
            }

            CellRange range;

            if (!GetRange(static_cast<double>(position.x) - radius, static_cast<double>(position.z) - radius,
                static_cast<double>(position.x) + radius, static_cast<double>(position.z) + radius, range))
            {
                return false;
            }

            for (std::uint32_t row = range.minRow; row <= range.maxRow; ++row)
            {
                for (std::uint32_t column = range.minColumn; column <= range.maxColumn; ++column)
                {
                    if (_cellCounts[CellIndex(column, row)] == _maxEntries)
                    {
                        return false;
                    }
                }
            }

            Entry& entry = _entries[_entryCount];
            entry.key.emplace(key);
            entry.position = position;
            entry.radius = radius;
            entry.range = range;

            for (std::uint32_t row = range.minRow; row <= range.maxRow; ++row)
            {
                for (std::uint32_t column = range.minColumn; column <= range.maxColumn; ++column)
                {
                    const std::size_t cell = CellIndex(column, row);
                    _members[cell * _maxEntries + _cellCounts[cell]] = _entryCount;
                    ++_cellCounts[cell];
                }
            }

            ++_entryCount;
            return true;
        }

        // out must already have capacity >= EntryCount(), including queries outside bounds.
        // Successful queries replace out with unique broad-phase candidates; false preserves it.
        bool QueryCircle(Vector3 center, float radius, std::vector<Key>& out) const noexcept
        {
            if (!Finite(center) || !std::isfinite(radius) || radius < 0)
            {
                return false;
            }

            return QueryRange(static_cast<double>(center.x) - radius, static_cast<double>(center.z) - radius,
                static_cast<double>(center.x) + radius, static_cast<double>(center.z) + radius, out);
        }

        bool QuerySegment(const Segment& segment, float radius, std::vector<Key>& out) const noexcept
        {
            if (!Finite(segment.start) || !Finite(segment.end) || !std::isfinite(radius) || radius < 0)
            {
                return false;
            }

            return QueryRange(static_cast<double>(std::min(segment.start.x, segment.end.x)) - radius,
                static_cast<double>(std::min(segment.start.z, segment.end.z)) - radius,
                static_cast<double>(std::max(segment.start.x, segment.end.x)) + radius,
                static_cast<double>(std::max(segment.start.z, segment.end.z)) + radius, out);
        }

        std::uint32_t EntryCount() const noexcept
        {
            return _entryCount;
        }

        bool CheckInvariants() const noexcept
        {
            if (_maxEntries == 0)
            {
                return !_entries && _entryCount == 0 && _columns == 0 && _rows == 0
                    && _cellCounts.empty() && _members.empty() && _queryMarks.empty();
            }

            const std::uint64_t cellCount = static_cast<std::uint64_t>(_columns) * _rows;

            if (!_entries || _entryCount > _maxEntries || _columns == 0 || _rows == 0
                || !Finite(_bounds.min) || !Finite(_bounds.max) || _bounds.min.x >= _bounds.max.x
                || _bounds.min.y >= _bounds.max.y || !std::isfinite(_cellSize) || _cellSize <= 0
                || cellCount > kMaximumMemberships / _maxEntries
                || _cellCounts.size() != cellCount || _members.size() != cellCount * _maxEntries
                || _queryMarks.size() != _maxEntries)
            {
                return false;
            }

            for (std::uint32_t index = 0; index < _maxEntries; ++index)
            {
                const Entry& entry = _entries[index];

                if (entry.key.has_value() != (index < _entryCount))
                {
                    return false;
                }

                if (index >= _entryCount)
                {
                    continue;
                }

                CellRange expected;

                if (!Finite(entry.position) || !Contains(entry.position) || !std::isfinite(entry.radius)
                    || entry.radius < 0 || !GetRange(static_cast<double>(entry.position.x) - entry.radius,
                        static_cast<double>(entry.position.z) - entry.radius,
                        static_cast<double>(entry.position.x) + entry.radius,
                        static_cast<double>(entry.position.z) + entry.radius, expected)
                    || expected != entry.range)
                {
                    return false;
                }
            }

            try
            {
                for (std::uint32_t index = 0; index < _entryCount; ++index)
                {
                    for (std::uint32_t prior = 0; prior < index; ++prior)
                    {
                        if (*_entries[index].key == *_entries[prior].key)
                        {
                            return false;
                        }
                    }
                }
            }
            catch (...)
            {
                return false;
            }

            for (std::uint32_t row = 0; row < _rows; ++row)
            {
                for (std::uint32_t column = 0; column < _columns; ++column)
                {
                    const std::size_t cell = CellIndex(column, row);
                    const std::uint32_t count = _cellCounts[cell];

                    if (count > _entryCount)
                    {
                        return false;
                    }

                    std::fill(_queryMarks.begin(), _queryMarks.end(), std::uint8_t{0});

                    for (std::uint32_t member = 0; member < _maxEntries; ++member)
                    {
                        const std::uint32_t entry = _members[cell * _maxEntries + member];

                        if (member >= count)
                        {
                            if (entry != kInvalidEntry)
                            {
                                return false;
                            }
                        }
                        else
                        {
                            if (entry >= _entryCount || _queryMarks[entry] != 0)
                            {
                                return false;
                            }

                            _queryMarks[entry] = 1;
                        }
                    }

                    for (std::uint32_t index = 0; index < _entryCount; ++index)
                    {
                        const CellRange& range = _entries[index].range;
                        const bool expected = column >= range.minColumn && column <= range.maxColumn
                            && row >= range.minRow && row <= range.maxRow;

                        if ((_queryMarks[index] != 0) != expected)
                        {
                            return false;
                        }
                    }
                }
            }

            return true;
        }

    private:
        friend struct SpatialGridTestAccess;

        static constexpr std::uint32_t kMaximumMemberships = 2097152;
        static constexpr std::uint32_t kInvalidEntry = std::numeric_limits<std::uint32_t>::max();

        struct CellRange
        {
            std::uint32_t minColumn = 0;
            std::uint32_t maxColumn = 0;
            std::uint32_t minRow = 0;
            std::uint32_t maxRow = 0;

            bool operator==(const CellRange&) const noexcept = default;
        };

        struct Entry
        {
            std::optional<Key> key;
            Vector3 position = {};
            float radius = 0;
            CellRange range;
        };

        static bool Finite(Vector2 value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        static bool Finite(Vector3 value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool Contains(Vector3 position) const noexcept
        {
            return position.x >= _bounds.min.x && position.x <= _bounds.max.x
                && position.z >= _bounds.min.y && position.z <= _bounds.max.y;
        }

        std::size_t CellIndex(std::uint32_t column, std::uint32_t row) const noexcept
        {
            return static_cast<std::size_t>(row) * _columns + column;
        }

        bool GetRange(double minX, double minZ, double maxX, double maxZ, CellRange& out) const noexcept
        {
            if (maxX < _bounds.min.x || minX > _bounds.max.x
                || maxZ < _bounds.min.y || minZ > _bounds.max.y)
            {
                return false;
            }

            const auto coordinate = [this](double value, double origin, std::uint32_t count)
            {
                const double relative = std::floor(std::max(0.0, (value - origin) / _cellSize));
                return static_cast<std::uint32_t>(std::min(relative, static_cast<double>(count - 1)));
            };

            out.minColumn = coordinate(std::max(minX, static_cast<double>(_bounds.min.x)), _bounds.min.x, _columns);
            out.maxColumn = coordinate(std::min(maxX, static_cast<double>(_bounds.max.x)), _bounds.min.x, _columns);
            out.minRow = coordinate(std::max(minZ, static_cast<double>(_bounds.min.y)), _bounds.min.y, _rows);
            out.maxRow = coordinate(std::min(maxZ, static_cast<double>(_bounds.max.y)), _bounds.min.y, _rows);
            return true;
        }

        bool QueryRange(double minX, double minZ, double maxX, double maxZ, std::vector<Key>& out) const noexcept
        {
            if (_maxEntries == 0 || out.capacity() < _entryCount)
            {
                return false;
            }

            std::fill(_queryMarks.begin(), _queryMarks.end(), std::uint8_t{0});
            CellRange range;

            if (GetRange(minX, minZ, maxX, maxZ, range))
            {
                for (std::uint32_t row = range.minRow; row <= range.maxRow; ++row)
                {
                    for (std::uint32_t column = range.minColumn; column <= range.maxColumn; ++column)
                    {
                        const std::size_t cell = CellIndex(column, row);

                        for (std::uint32_t member = 0; member < _cellCounts[cell]; ++member)
                        {
                            _queryMarks[_members[cell * _maxEntries + member]] = 1;
                        }
                    }
                }
            }

            // Bounds restrict entry centers, not shapes. An outside query can hit the
            // part of an entry radius that is not represented by a clamped grid cell.
            const bool outside = minX < _bounds.min.x || maxX > _bounds.max.x
                || minZ < _bounds.min.y || maxZ > _bounds.max.y;
            out.clear();

            for (std::uint32_t index = 0; index < _entryCount; ++index)
            {
                const Entry& entry = _entries[index];
                const bool outsideCandidate = outside
                    && static_cast<double>(entry.position.x) - entry.radius <= maxX
                    && static_cast<double>(entry.position.x) + entry.radius >= minX
                    && static_cast<double>(entry.position.z) - entry.radius <= maxZ
                    && static_cast<double>(entry.position.z) + entry.radius >= minZ;

                if (_queryMarks[index] != 0 || outsideCandidate)
                {
                    out.push_back(*entry.key);
                }
            }

            return true;
        }

        BoundsXZ _bounds = {};
        float _cellSize = 0;
        std::uint32_t _columns = 0;
        std::uint32_t _rows = 0;
        std::uint32_t _maxEntries = 0;
        std::uint32_t _entryCount = 0;
        std::unique_ptr<Entry[]> _entries;
        std::vector<std::uint32_t> _cellCounts;
        std::vector<std::uint32_t> _members;
        mutable std::vector<std::uint8_t> _queryMarks;
    };
}
