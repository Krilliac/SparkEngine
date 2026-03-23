/**
 * @file VersionedHandle.h
 * @brief Generational index handle for detecting stale references
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a compact handle (32-bit index + 32-bit generation) and an allocator
 * that recycles slots with generation bumps. Callers can detect use-after-free
 * via O(1) `IsValid()` instead of crashing.
 *
 * Separate concept from OpaqueHandle (which wraps a void* pointer).
 *
 * ## Usage
 * @code
 *   Spark::HandleAllocator allocator(1024);
 *
 *   auto handle = allocator.Allocate();
 *   ASSERT(allocator.IsValid(handle));    // true
 *
 *   allocator.Free(handle);
 *   ASSERT(!allocator.IsValid(handle));   // false — generation mismatch
 *
 *   auto reused = allocator.Allocate();   // Same index, higher generation
 * @endcode
 */

#pragma once

#include <cstdint>
#include <vector>

namespace Spark
{

    /**
     * @struct VersionedHandle
     * @brief Compact handle with index and generation for stale reference detection.
     */
    struct VersionedHandle
    {
        uint32_t index = 0;
        uint32_t generation = 0;

        [[nodiscard]] constexpr bool operator==(const VersionedHandle&) const = default;

        /**
         * @brief Check if this handle represents the null/invalid sentinel.
         */
        [[nodiscard]] constexpr bool IsNull() const noexcept { return index == UINT32_MAX && generation == 0; }

        /**
         * @brief Return the null sentinel handle.
         */
        [[nodiscard]] static constexpr VersionedHandle Null() noexcept { return {UINT32_MAX, 0}; }
    };

    /**
     * @class HandleAllocator
     * @brief Manages allocation and validation of versioned handles.
     *
     * Fixed capacity, O(1) allocate/free/validate. Not thread-safe.
     */
    class HandleAllocator
    {
      public:
        /**
         * @brief Create an allocator with the given slot capacity.
         * @param capacity Maximum number of simultaneously live handles.
         */
        explicit HandleAllocator(uint32_t capacity) : m_generations(capacity, 0), m_activeCount(0)
        {
            m_freeList.reserve(capacity);
            // Push in reverse so index 0 is allocated first
            for (uint32_t i = capacity; i > 0; --i)
                m_freeList.push_back(i - 1);
        }

        /**
         * @brief Allocate a new handle. Returns Null if capacity is exhausted.
         */
        [[nodiscard]] VersionedHandle Allocate()
        {
            if (m_freeList.empty())
                return VersionedHandle::Null();

            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            ++m_activeCount;
            return {idx, m_generations[idx]};
        }

        /**
         * @brief Free a handle, making its slot available for reuse.
         *
         * Bumps the generation so that stale handles pointing to this slot
         * will fail IsValid(). Double-free is a no-op (generation mismatch).
         */
        void Free(VersionedHandle handle)
        {
            if (handle.IsNull())
                return;
            if (handle.index >= m_generations.size())
                return;
            if (m_generations[handle.index] != handle.generation)
                return; // Already freed (generation mismatch)

            ++m_generations[handle.index];
            m_freeList.push_back(handle.index);
            --m_activeCount;
        }

        /**
         * @brief O(1) validity check: index in range AND generation matches.
         */
        [[nodiscard]] bool IsValid(VersionedHandle handle) const
        {
            if (handle.IsNull())
                return false;
            if (handle.index >= m_generations.size())
                return false;
            return m_generations[handle.index] == handle.generation;
        }

        /**
         * @brief Number of currently live (allocated, not freed) handles.
         */
        [[nodiscard]] uint32_t GetActiveCount() const { return m_activeCount; }

        /**
         * @brief Total slot capacity.
         */
        [[nodiscard]] uint32_t GetCapacity() const { return static_cast<uint32_t>(m_generations.size()); }

      private:
        std::vector<uint32_t> m_generations;
        std::vector<uint32_t> m_freeList;
        uint32_t m_activeCount;
    };

} // namespace Spark
