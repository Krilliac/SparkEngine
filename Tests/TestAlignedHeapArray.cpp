/**
 * @file TestAlignedHeapArray.cpp
 * @brief Tests for SIMD-aligned heap memory arrays
 */

#include "TestFramework.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

    template <typename T, size_t SIZE, size_t ALIGNMENT = 32> class TestAlignedArray
    {
      public:
        TestAlignedArray()
        {
#ifdef _MSC_VER
            m_data = static_cast<T*>(_aligned_malloc(SIZE * sizeof(T), ALIGNMENT));
#else
            void* ptr = nullptr;
            if (posix_memalign(&ptr, ALIGNMENT, SIZE * sizeof(T)) != 0)
                ptr = nullptr;
            m_data = static_cast<T*>(ptr);
#endif
            if (m_data)
                std::memset(m_data, 0, SIZE * sizeof(T));
        }

        ~TestAlignedArray()
        {
#ifdef _MSC_VER
            _aligned_free(m_data);
#else
            free(m_data);
#endif
        }

        T* data() { return m_data; }
        constexpr size_t size() const { return SIZE; }
        T& operator[](size_t i) { return m_data[i]; }

        bool IsAligned() const { return (reinterpret_cast<uintptr_t>(m_data) % ALIGNMENT) == 0; }

      private:
        T* m_data = nullptr;
    };

    template <typename T, size_t ALIGNMENT = 32> class TestDynamicArray
    {
      public:
        TestDynamicArray() = default;

        explicit TestDynamicArray(size_t count) : m_size(count)
        {
            if (count > 0)
            {
#ifdef _MSC_VER
                m_data = static_cast<T*>(_aligned_malloc(count * sizeof(T), ALIGNMENT));
#else
                void* ptr = nullptr;
                if (posix_memalign(&ptr, ALIGNMENT, count * sizeof(T)) != 0)
                    ptr = nullptr;
                m_data = static_cast<T*>(ptr);
#endif
                if (m_data)
                    std::memset(m_data, 0, count * sizeof(T));
            }
        }

        ~TestDynamicArray()
        {
#ifdef _MSC_VER
            _aligned_free(m_data);
#else
            free(m_data);
#endif
        }

        // Move
        TestDynamicArray(TestDynamicArray&& other) noexcept : m_data(other.m_data), m_size(other.m_size)
        {
            other.m_data = nullptr;
            other.m_size = 0;
        }

        T* data() { return m_data; }
        size_t size() const { return m_size; }
        bool empty() const { return m_size == 0; }
        T& operator[](size_t i) { return m_data[i]; }

      private:
        T* m_data = nullptr;
        size_t m_size = 0;
    };

} // anonymous namespace

TEST(AlignedHeapArray_Alignment)
{
    TestAlignedArray<float, 256, 32> arr;
    EXPECT_TRUE(arr.IsAligned());
    EXPECT_EQ(arr.size(), 256u);
}

TEST(AlignedHeapArray_ReadWrite)
{
    TestAlignedArray<int32_t, 64, 16> arr;
    for (size_t i = 0; i < arr.size(); ++i)
        arr[i] = static_cast<int32_t>(i * 2);

    EXPECT_EQ(arr[0], 0);
    EXPECT_EQ(arr[1], 2);
    EXPECT_EQ(arr[63], 126);
}

TEST(AlignedHeapArray_ZeroInitialized)
{
    TestAlignedArray<uint8_t, 128, 32> arr;
    bool allZero = true;
    for (size_t i = 0; i < arr.size(); ++i)
    {
        if (arr[i] != 0)
        {
            allZero = false;
            break;
        }
    }
    EXPECT_TRUE(allZero);
}

TEST(DynamicHeapArray_Create)
{
    TestDynamicArray<float> arr(128);
    EXPECT_EQ(arr.size(), 128u);
    EXPECT_FALSE(arr.empty());
    EXPECT_TRUE(arr.data() != nullptr);
}

TEST(DynamicHeapArray_MoveSemantics)
{
    TestDynamicArray<int32_t> arr(64);
    arr[0] = 42;
    arr[63] = 99;

    TestDynamicArray<int32_t> moved(std::move(arr));
    EXPECT_EQ(moved.size(), 64u);
    EXPECT_EQ(moved[0], 42);
    EXPECT_EQ(moved[63], 99);
    EXPECT_TRUE(arr.empty());
}

TEST(DynamicHeapArray_Empty)
{
    TestDynamicArray<float> arr;
    EXPECT_TRUE(arr.empty());
    EXPECT_EQ(arr.size(), 0u);
}
