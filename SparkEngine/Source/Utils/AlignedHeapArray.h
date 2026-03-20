/**
 * @file AlignedHeapArray.h
 * @brief SIMD-aligned heap arrays with move semantics
 *
 * Inspired by PCSX2's HeapArray.h. Provides FixedHeapArray and
 * DynamicHeapArray with guaranteed alignment for SIMD operations.
 * Cleaner than manual aligned allocation scattered across the codebase.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

namespace Spark
{

    namespace Detail
    {
        inline void* AlignedAlloc(size_t size, size_t alignment)
        {
#ifdef _MSC_VER
            return _aligned_malloc(size, alignment);
#else
            void* ptr = nullptr;
            if (posix_memalign(&ptr, alignment, size) != 0)
                return nullptr;
            return ptr;
#endif
        }

        inline void AlignedFree(void* ptr)
        {
#ifdef _MSC_VER
            _aligned_free(ptr);
#else
            free(ptr);
#endif
        }
    } // namespace Detail

    /**
     * @brief Fixed-size, SIMD-aligned heap array.
     *
     * @tparam T Element type (must be trivially copyable).
     * @tparam SIZE Number of elements.
     * @tparam ALIGNMENT Memory alignment in bytes (default: 32 for AVX).
     */
    template <typename T, size_t SIZE, size_t ALIGNMENT = 32> class FixedHeapArray
    {
        static_assert(std::is_trivially_copyable_v<T>, "FixedHeapArray requires trivially copyable type");
        static_assert((ALIGNMENT & (ALIGNMENT - 1)) == 0, "Alignment must be power of 2");
        static_assert(ALIGNMENT >= alignof(T), "Alignment must be >= alignof(T)");

      public:
        FixedHeapArray()
        {
            m_data = static_cast<T*>(Detail::AlignedAlloc(SIZE * sizeof(T), ALIGNMENT));
            if (m_data)
                std::memset(m_data, 0, SIZE * sizeof(T));
        }

        ~FixedHeapArray() { Detail::AlignedFree(m_data); }

        // Move semantics
        FixedHeapArray(FixedHeapArray&& other) noexcept : m_data(other.m_data) { other.m_data = nullptr; }

        FixedHeapArray& operator=(FixedHeapArray&& other) noexcept
        {
            if (this != &other)
            {
                Detail::AlignedFree(m_data);
                m_data = other.m_data;
                other.m_data = nullptr;
            }
            return *this;
        }

        // No copy
        FixedHeapArray(const FixedHeapArray&) = delete;
        FixedHeapArray& operator=(const FixedHeapArray&) = delete;

        T* data() { return m_data; }
        const T* data() const { return m_data; }
        T* begin() { return m_data; }
        T* end() { return m_data + SIZE; }
        const T* begin() const { return m_data; }
        const T* end() const { return m_data + SIZE; }
        constexpr size_t size() const { return SIZE; }
        constexpr size_t byte_size() const { return SIZE * sizeof(T); }
        T& operator[](size_t index) { return m_data[index]; }
        const T& operator[](size_t index) const { return m_data[index]; }

        void Fill(const T& value)
        {
            for (size_t i = 0; i < SIZE; ++i)
                m_data[i] = value;
        }

        void Zero() { std::memset(m_data, 0, SIZE * sizeof(T)); }

      private:
        T* m_data = nullptr;
    };

    /**
     * @brief Dynamically-sized, SIMD-aligned heap array.
     *
     * @tparam T Element type (must be trivially copyable).
     * @tparam ALIGNMENT Memory alignment in bytes (default: 32 for AVX).
     */
    template <typename T, size_t ALIGNMENT = 32> class DynamicHeapArray
    {
        static_assert(std::is_trivially_copyable_v<T>, "DynamicHeapArray requires trivially copyable type");
        static_assert((ALIGNMENT & (ALIGNMENT - 1)) == 0, "Alignment must be power of 2");

      public:
        DynamicHeapArray() = default;

        explicit DynamicHeapArray(size_t count) : m_size(count)
        {
            if (count > 0)
            {
                m_data = static_cast<T*>(Detail::AlignedAlloc(count * sizeof(T), ALIGNMENT));
                if (m_data)
                    std::memset(m_data, 0, count * sizeof(T));
            }
        }

        ~DynamicHeapArray() { Detail::AlignedFree(m_data); }

        // Move semantics
        DynamicHeapArray(DynamicHeapArray&& other) noexcept : m_data(other.m_data), m_size(other.m_size)
        {
            other.m_data = nullptr;
            other.m_size = 0;
        }

        DynamicHeapArray& operator=(DynamicHeapArray&& other) noexcept
        {
            if (this != &other)
            {
                Detail::AlignedFree(m_data);
                m_data = other.m_data;
                m_size = other.m_size;
                other.m_data = nullptr;
                other.m_size = 0;
            }
            return *this;
        }

        // No copy
        DynamicHeapArray(const DynamicHeapArray&) = delete;
        DynamicHeapArray& operator=(const DynamicHeapArray&) = delete;

        void Resize(size_t newSize)
        {
            if (newSize == m_size)
                return;
            Detail::AlignedFree(m_data);
            m_size = newSize;
            if (newSize > 0)
            {
                m_data = static_cast<T*>(Detail::AlignedAlloc(newSize * sizeof(T), ALIGNMENT));
                if (m_data)
                    std::memset(m_data, 0, newSize * sizeof(T));
            }
            else
            {
                m_data = nullptr;
            }
        }

        T* data() { return m_data; }
        const T* data() const { return m_data; }
        T* begin() { return m_data; }
        T* end() { return m_data + m_size; }
        const T* begin() const { return m_data; }
        const T* end() const { return m_data + m_size; }
        size_t size() const { return m_size; }
        size_t byte_size() const { return m_size * sizeof(T); }
        bool empty() const { return m_size == 0; }
        T& operator[](size_t index) { return m_data[index]; }
        const T& operator[](size_t index) const { return m_data[index]; }

        void Fill(const T& value)
        {
            for (size_t i = 0; i < m_size; ++i)
                m_data[i] = value;
        }

        void Zero()
        {
            if (m_data)
                std::memset(m_data, 0, m_size * sizeof(T));
        }

      private:
        T* m_data = nullptr;
        size_t m_size = 0;
    };

} // namespace Spark
