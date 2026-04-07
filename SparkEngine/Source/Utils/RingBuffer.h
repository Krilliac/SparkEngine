/**
 * @file RingBuffer.h
 * @brief Generic fixed-size and dynamic ring buffer templates
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides circular buffer data structures for efficient FIFO storage
 * with constant-time push and indexed access. Useful for frame history,
 * input buffering, audio sample windows, and event queues.
 *
 * ## Usage
 * @code
 *   // Fixed-size ring buffer (stack-allocated)
 *   Spark::RingBuffer<float, 60> fpsHistory;
 *   fpsHistory.Push(60.0f);
 *   fpsHistory.Push(59.5f);
 *   float latest = fpsHistory[0];     // 59.5 (most recent)
 *   float previous = fpsHistory[1];   // 60.0
 *
 *   // Dynamic ring buffer (heap-allocated)
 *   Spark::DynamicRingBuffer<int> events(256);
 *   events.Push(42);
 * @endcode
 */

#pragma once

#include "Core/Contracts.h"

#include <cstddef>
#include <vector>
#include <stdexcept>

namespace Spark
{

    // =============================================================================
    // Fixed-Size Ring Buffer
    // =============================================================================

    /**
 * @brief Fixed-capacity ring buffer with compile-time size
 * @tparam T Element type
 * @tparam N Maximum number of elements
 */
    template <typename T, size_t N> class RingBuffer
    {
        static_assert(N > 0, "RingBuffer capacity must be > 0");

      public:
        RingBuffer() = default;

        /// Push a new element, overwriting oldest if full
        void Push(const T& value)
        {
            m_data[m_head] = value;
            m_head = (m_head + 1) % N;
            if (m_count < N)
                m_count++;
        }

        /// Access by reverse index: 0 = most recent, 1 = second most recent, etc.
        const T& operator[](size_t index) const
        {
            SPARK_EXPECTS(index < N);
            size_t i = (m_head + N - 1 - index) % N;
            return m_data[i];
        }

        T& operator[](size_t index)
        {
            SPARK_EXPECTS(index < N);
            size_t i = (m_head + N - 1 - index) % N;
            return m_data[i];
        }

        /// Most recently pushed element
        const T& Front() const { return (*this)[0]; }

        /// Oldest element in the buffer
        const T& Back() const { return (*this)[m_count - 1]; }

        size_t Size() const { return m_count; }
        size_t Capacity() const { return N; }
        bool IsEmpty() const { return m_count == 0; }
        bool IsFull() const { return m_count == N; }

        void Clear()
        {
            m_head = 0;
            m_count = 0;
        }

        // ---- Iterator support (oldest to newest) ----

        class Iterator
        {
          public:
            Iterator(const RingBuffer* buf, size_t pos) : m_buf(buf), m_pos(pos) {}
            const T& operator*() const { return (*m_buf)[m_buf->m_count - 1 - m_pos]; }
            Iterator& operator++()
            {
                ++m_pos;
                return *this;
            }
            bool operator!=(const Iterator& other) const { return m_pos != other.m_pos; }

          private:
            const RingBuffer* m_buf;
            size_t m_pos;
        };

        Iterator begin() const { return Iterator(this, 0); }
        Iterator end() const { return Iterator(this, m_count); }

      private:
        T m_data[N] = {};
        size_t m_head = 0;  ///< Next write position
        size_t m_count = 0; ///< Current number of elements
    };

    // =============================================================================
    // Dynamic Ring Buffer
    // =============================================================================

    /**
 * @brief Runtime-sized ring buffer backed by std::vector
 * @tparam T Element type
 */
    template <typename T> class DynamicRingBuffer
    {
      public:
        explicit DynamicRingBuffer(size_t capacity = 64) : m_data(capacity), m_capacity(capacity) {}

        void Push(const T& value)
        {
            if (m_capacity == 0)
                return;
            m_data[m_head] = value;
            m_head = (m_head + 1) % m_capacity;
            if (m_count < m_capacity)
                m_count++;
        }

        const T& operator[](size_t index) const
        {
            SPARK_EXPECTS(index < m_capacity);
            size_t i = (m_head + m_capacity - 1 - index) % m_capacity;
            return m_data[i];
        }

        T& operator[](size_t index)
        {
            SPARK_EXPECTS(index < m_capacity);
            size_t i = (m_head + m_capacity - 1 - index) % m_capacity;
            return m_data[i];
        }

        const T& Front() const { return (*this)[0]; }
        const T& Back() const { return (*this)[m_count - 1]; }

        size_t Size() const { return m_count; }
        size_t Capacity() const { return m_capacity; }
        bool IsEmpty() const { return m_count == 0; }
        bool IsFull() const { return m_count == m_capacity; }

        void Clear()
        {
            m_head = 0;
            m_count = 0;
        }

        void Resize(size_t newCapacity)
        {
            m_data.resize(newCapacity);
            m_capacity = newCapacity;
            m_head = 0;
            m_count = 0;
        }

      private:
        std::vector<T> m_data;
        size_t m_capacity;
        size_t m_head = 0;
        size_t m_count = 0;
    };

} // namespace Spark
