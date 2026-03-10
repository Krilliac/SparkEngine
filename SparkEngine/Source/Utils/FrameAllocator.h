/**
 * @file FrameAllocator.h
 * @brief Per-frame linear (bump) allocator
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a fast linear allocator for temporary per-frame allocations
 * that are automatically freed at the end of each frame. Eliminates
 * heap fragmentation and malloc/free overhead for short-lived objects.
 *
 * ## Usage
 * @code
 *   // At engine startup
 *   Spark::FrameAllocator allocator(1024 * 1024); // 1 MB
 *
 *   // During frame
 *   auto* data = allocator.Alloc<RenderCommand>(16);
 *   // ... use data ...
 *
 *   // At end of frame
 *   allocator.Reset();
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace Spark
{

    class FrameAllocator
    {
      public:
        /// Create a frame allocator with the given capacity in bytes
        explicit FrameAllocator(size_t capacityBytes = 1024 * 1024) : m_capacity(capacityBytes), m_offset(0)
        {
            m_buffer = static_cast<uint8_t*>(::operator new(capacityBytes, std::nothrow));
        }

        ~FrameAllocator() { ::operator delete(m_buffer); }

        // Non-copyable
        FrameAllocator(const FrameAllocator&) = delete;
        FrameAllocator& operator=(const FrameAllocator&) = delete;

        // Movable
        FrameAllocator(FrameAllocator&& other) noexcept
            : m_buffer(other.m_buffer), m_capacity(other.m_capacity), m_offset(other.m_offset)
        {
            other.m_buffer = nullptr;
            other.m_capacity = 0;
            other.m_offset = 0;
        }

        FrameAllocator& operator=(FrameAllocator&& other) noexcept
        {
            if (this != &other)
            {
                ::operator delete(m_buffer);
                m_buffer = other.m_buffer;
                m_capacity = other.m_capacity;
                m_offset = other.m_offset;
                other.m_buffer = nullptr;
                other.m_capacity = 0;
                other.m_offset = 0;
            }
            return *this;
        }

        /**
     * @brief Allocate raw bytes with given alignment
     * @return Pointer to allocated memory, or nullptr if out of space
     */
        void* AllocRaw(size_t bytes, size_t alignment = alignof(std::max_align_t))
        {
            if (!m_buffer)
                return nullptr;

            // Align the current offset
            size_t aligned = (m_offset + alignment - 1) & ~(alignment - 1);
            if (aligned + bytes > m_capacity)
            {
                return nullptr; // Out of space
            }

            void* ptr = m_buffer + aligned;
            m_offset = aligned + bytes;
            return ptr;
        }

        /**
     * @brief Allocate raw bytes with specified alignment (convenience alias)
     *
     * Matches the standard interface:
     *   void* Allocate(size_t size, size_t alignment = 16);
     *
     * @return Pointer to allocated memory, or nullptr if out of space
     */
        void* Allocate(size_t size, size_t alignment = 16)
        {
            return AllocRaw(size, alignment);
        }

        /**
     * @brief Allocate space for N objects of type T (uninitialized)
     * @return Pointer to uninitialized memory for count T objects
     */
        template <typename T> T* Alloc(size_t count = 1)
        {
            return static_cast<T*>(AllocRaw(sizeof(T) * count, alignof(T)));
        }

        /**
     * @brief Allocate and construct a single object of type T
     */
        template <typename T, typename... Args> T* New(Args&&... args)
        {
            void* mem = AllocRaw(sizeof(T), alignof(T));
            if (!mem)
                return nullptr;
            return new (mem) T(std::forward<Args>(args)...);
        }

        /**
     * @brief Reset the allocator for the next frame
     *
     * This is O(1) — just resets the offset pointer.
     * Does NOT call destructors — only use for trivially destructible
     * types, or types whose destructors are no-ops.
     */
        void Reset() { m_offset = 0; }

        /// Current bytes used this frame
        size_t Used() const { return m_offset; }

        /// Convenience alias matching the standard interface
        size_t GetUsed() const { return m_offset; }

        /// Total capacity in bytes
        size_t Capacity() const { return m_capacity; }

        /// Convenience alias matching the standard interface
        size_t GetCapacity() const { return m_capacity; }

        /// Remaining bytes available
        size_t Remaining() const { return m_capacity > m_offset ? m_capacity - m_offset : 0; }

        /// Peak usage (high water mark) — call before Reset()
        size_t PeakUsed() const { return m_peakOffset; }

        /// Update peak tracking — call before Reset()
        void UpdatePeak()
        {
            if (m_offset > m_peakOffset)
                m_peakOffset = m_offset;
        }

      private:
        uint8_t* m_buffer = nullptr;
        size_t m_capacity = 0;
        size_t m_offset = 0;
        size_t m_peakOffset = 0;
    };

} // namespace Spark
