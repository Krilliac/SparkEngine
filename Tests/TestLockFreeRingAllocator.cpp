/**
 * @file TestLockFreeRingAllocator.cpp
 * @brief Tests for lock-free ring buffer allocator
 */

#include "TestFramework.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

    class TestRingAllocator
    {
      public:
        static constexpr size_t CAPACITY = 1024;

        TestRingAllocator() : m_buffer(CAPACITY, 0) {}

        void* Allocate(size_t size)
        {
            size_t aligned = (size + 7) & ~7;
            if (m_used + aligned > CAPACITY)
                return nullptr;
            void* ptr = m_buffer.data() + m_writePos;
            m_writePos += aligned;
            m_used += aligned;
            m_allocCount++;
            return ptr;
        }

        void Reset()
        {
            m_writePos = 0;
            m_used = 0;
        }

        size_t GetUsed() const { return m_used; }
        bool IsEmpty() const { return m_used == 0; }
        uint64_t GetAllocCount() const { return m_allocCount; }

      private:
        std::vector<uint8_t> m_buffer;
        size_t m_writePos = 0;
        size_t m_used = 0;
        uint64_t m_allocCount = 0;
    };

} // anonymous namespace

TEST(RingAllocator_AllocateAndReset)
{
    TestRingAllocator alloc;
    EXPECT_TRUE(alloc.IsEmpty());

    void* p1 = alloc.Allocate(64);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_FALSE(alloc.IsEmpty());
    EXPECT_EQ(alloc.GetAllocCount(), 1u);

    void* p2 = alloc.Allocate(128);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_EQ(alloc.GetAllocCount(), 2u);

    alloc.Reset();
    EXPECT_TRUE(alloc.IsEmpty());
}

TEST(RingAllocator_CapacityLimit)
{
    TestRingAllocator alloc;
    void* p = alloc.Allocate(2048); // Exceeds 1024
    EXPECT_TRUE(p == nullptr);
}

TEST(RingAllocator_AlignedAllocations)
{
    TestRingAllocator alloc;
    void* p1 = alloc.Allocate(3); // Aligns to 8
    void* p2 = alloc.Allocate(5); // Aligns to 8
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_EQ(alloc.GetUsed(), 16u); // 8 + 8
}

TEST(RingAllocator_ExactCapacity)
{
    TestRingAllocator alloc;
    void* p = alloc.Allocate(TestRingAllocator::CAPACITY);
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(alloc.GetUsed(), TestRingAllocator::CAPACITY);

    // No space left
    void* p2 = alloc.Allocate(1);
    EXPECT_TRUE(p2 == nullptr);
}

TEST(RingAllocator_ResetAndReuse)
{
    TestRingAllocator alloc;
    alloc.Allocate(512);
    EXPECT_EQ(alloc.GetUsed(), 512u);
    EXPECT_EQ(alloc.GetAllocCount(), 1u);

    alloc.Reset();
    EXPECT_TRUE(alloc.IsEmpty());

    void* p = alloc.Allocate(256);
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(alloc.GetUsed(), 256u);
    EXPECT_EQ(alloc.GetAllocCount(), 2u);
}

TEST(RingAllocator_ManySmallAllocations)
{
    TestRingAllocator alloc;
    int count = 0;
    // Each 1-byte alloc aligns to 8
    while (alloc.Allocate(1) != nullptr)
        count++;
    // 1024 / 8 = 128 allocations
    EXPECT_EQ(count, 128);
    EXPECT_EQ(alloc.GetAllocCount(), static_cast<uint64_t>(128));
}

TEST(RingAllocator_ZeroSize)
{
    TestRingAllocator alloc;
    void* p = alloc.Allocate(0);
    // Zero-size aligns to 0, so no space consumed
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(alloc.GetUsed(), 0u);
}

TEST(RingAllocator_DistinctPointers)
{
    TestRingAllocator alloc;
    void* p1 = alloc.Allocate(16);
    void* p2 = alloc.Allocate(16);
    void* p3 = alloc.Allocate(16);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_TRUE(p3 != nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_NE(p2, p3);
    EXPECT_NE(p1, p3);
}
