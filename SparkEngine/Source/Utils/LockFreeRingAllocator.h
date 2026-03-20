/**
 * @file LockFreeRingAllocator.h
 * @brief Lock-free ring buffer allocator for producer-consumer patterns
 *
 * Optimized for single-writer/multi-reader
 * patterns using relaxed atomics. Allocations include a size prefix for
 * O(1) deallocation. Ideal for per-frame render command data.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace Spark
{

    /**
     * @brief Fixed-size ring buffer allocator with lock-free operation.
     *
     * Designed for producer-consumer threading: one thread allocates/writes,
     * other threads read. Uses memory_order_relaxed for minimal overhead.
     *
     * @tparam CapacityBytes Total ring buffer size in bytes (must be power of 2).
     */
    template <size_t CapacityBytes = (1 << 20)> // 1MB default
    class LockFreeRingAllocator
    {
      public:
        static_assert((CapacityBytes & (CapacityBytes - 1)) == 0, "Capacity must be power of 2");
        static constexpr size_t CAPACITY = CapacityBytes;
        static constexpr size_t MASK = CAPACITY - 1;
        static constexpr size_t HEADER_SIZE = sizeof(uint32_t); // Size prefix

        LockFreeRingAllocator() { m_buffer = new uint8_t[CAPACITY]; }

        ~LockFreeRingAllocator() { delete[] m_buffer; }

        LockFreeRingAllocator(const LockFreeRingAllocator&) = delete;
        LockFreeRingAllocator& operator=(const LockFreeRingAllocator&) = delete;

        /**
         * @brief Allocate a block of memory from the ring.
         *
         * @param size Requested allocation size in bytes.
         * @return Pointer to allocated memory, or nullptr if ring is full.
         */
        void* Allocate(size_t size)
        {
            size_t totalSize = size + HEADER_SIZE;
            size_t aligned = (totalSize + 7) & ~7; // 8-byte alignment

            size_t writePos = m_writePos.load(std::memory_order_relaxed);
            size_t readPos = m_readPos.load(std::memory_order_acquire);

            size_t used = writePos - readPos;
            if (used + aligned > CAPACITY)
                return nullptr;

            size_t offset = writePos & MASK;

            // Handle wrap-around: if allocation would split, skip to start
            if (offset + aligned > CAPACITY)
            {
                size_t wastedSpace = CAPACITY - offset;
                if (used + wastedSpace + aligned > CAPACITY)
                    return nullptr;
                // Mark wasted space with zero-size header
                auto* skipHeader = reinterpret_cast<uint32_t*>(m_buffer + offset);
                *skipHeader = 0;
                writePos += wastedSpace;
                offset = 0;
            }

            // Write size prefix
            auto* header = reinterpret_cast<uint32_t*>(m_buffer + offset);
            *header = static_cast<uint32_t>(size);

            m_writePos.store(writePos + aligned, std::memory_order_release);
            m_totalAllocations++;
            m_totalBytesAllocated += size;

            return m_buffer + offset + HEADER_SIZE;
        }

        /**
         * @brief Advance the read position past one allocation.
         *
         * Must be called in allocation order (FIFO).
         */
        void Free()
        {
            size_t readPos = m_readPos.load(std::memory_order_relaxed);
            size_t offset = readPos & MASK;

            auto* header = reinterpret_cast<const uint32_t*>(m_buffer + offset);
            uint32_t size = *header;

            if (size == 0)
            {
                // Skip marker — advance past wasted wrap-around space
                size_t wastedSpace = CAPACITY - offset;
                readPos += wastedSpace;
                offset = 0;
                header = reinterpret_cast<const uint32_t*>(m_buffer + offset);
                size = *header;
            }

            size_t totalSize = size + HEADER_SIZE;
            size_t aligned = (totalSize + 7) & ~7;
            m_readPos.store(readPos + aligned, std::memory_order_release);
            m_totalFrees++;
        }

        /**
         * @brief Reset the ring buffer (not thread-safe — call when idle).
         */
        void Reset()
        {
            m_writePos.store(0, std::memory_order_relaxed);
            m_readPos.store(0, std::memory_order_relaxed);
        }

        size_t GetUsedBytes() const
        {
            return m_writePos.load(std::memory_order_acquire) - m_readPos.load(std::memory_order_acquire);
        }

        size_t GetFreeBytes() const { return CAPACITY - GetUsedBytes(); }
        bool IsEmpty() const { return GetUsedBytes() == 0; }

        struct Metrics
        {
            uint64_t totalAllocations = 0;
            uint64_t totalFrees = 0;
            uint64_t totalBytesAllocated = 0;
            size_t currentUsed = 0;
        };

        Metrics GetMetrics() const { return {m_totalAllocations, m_totalFrees, m_totalBytesAllocated, GetUsedBytes()}; }

      private:
        uint8_t* m_buffer = nullptr;
        alignas(64) std::atomic<size_t> m_writePos{0};
        alignas(64) std::atomic<size_t> m_readPos{0};
        uint64_t m_totalAllocations = 0;
        uint64_t m_totalFrees = 0;
        uint64_t m_totalBytesAllocated = 0;
    };

} // namespace Spark
