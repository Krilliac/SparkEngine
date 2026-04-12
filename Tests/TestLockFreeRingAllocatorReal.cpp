/**
 * @file TestLockFreeRingAllocatorReal.cpp
 * @brief Real-class tests for Spark::LockFreeRingAllocator
 */

#include "TestFramework.h"
#include "Utils/LockFreeRingAllocator.h"

#include <cstring>

// Use a small 1KB ring for easier testing
using SmallRing = Spark::LockFreeRingAllocator<1024>;

// ============================================================================
// Construction and initial state
// ============================================================================

TEST(LockFreeRingAllocatorReal_InitialState)
{
    SmallRing ring;
    EXPECT_TRUE(ring.IsEmpty());
    EXPECT_EQ(ring.GetUsedBytes(), 0u);
    EXPECT_EQ(ring.GetFreeBytes(), 1024u);
}

TEST(LockFreeRingAllocatorReal_MetricsInitial)
{
    SmallRing ring;
    auto m = ring.GetMetrics();
    EXPECT_EQ(m.totalAllocations, 0u);
    EXPECT_EQ(m.totalFrees, 0u);
    EXPECT_EQ(m.totalBytesAllocated, 0u);
    EXPECT_EQ(m.currentUsed, 0u);
}

// ============================================================================
// Allocate and free
// ============================================================================

TEST(LockFreeRingAllocatorReal_SingleAllocate)
{
    SmallRing ring;
    void* ptr = ring.Allocate(64);
    EXPECT_TRUE(ptr != nullptr);
    EXPECT_FALSE(ring.IsEmpty());
    EXPECT_GT(ring.GetUsedBytes(), 0u);
}

TEST(LockFreeRingAllocatorReal_WriteAndRead)
{
    SmallRing ring;
    void* ptr = ring.Allocate(16);
    EXPECT_TRUE(ptr != nullptr);

    // Write data into the allocation
    std::memset(ptr, 0xAB, 16);
    auto* bytes = static_cast<uint8_t*>(ptr);
    EXPECT_EQ(bytes[0], 0xAB);
    EXPECT_EQ(bytes[15], 0xAB);
}

TEST(LockFreeRingAllocatorReal_AllocateAndFree)
{
    SmallRing ring;
    ring.Allocate(32);
    EXPECT_FALSE(ring.IsEmpty());

    ring.Free();
    EXPECT_TRUE(ring.IsEmpty());

    auto m = ring.GetMetrics();
    EXPECT_EQ(m.totalAllocations, 1u);
    EXPECT_EQ(m.totalFrees, 1u);
}

TEST(LockFreeRingAllocatorReal_MultipleAllocations)
{
    SmallRing ring;
    void* p1 = ring.Allocate(32);
    void* p2 = ring.Allocate(64);
    void* p3 = ring.Allocate(16);

    EXPECT_TRUE(p1 != nullptr);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_TRUE(p3 != nullptr);

    auto m = ring.GetMetrics();
    EXPECT_EQ(m.totalAllocations, 3u);
    EXPECT_EQ(m.totalBytesAllocated, 112u);
}

TEST(LockFreeRingAllocatorReal_AllocationFailsWhenFull)
{
    SmallRing ring;
    // Try to allocate more than capacity
    void* ptr = ring.Allocate(2000);
    EXPECT_TRUE(ptr == nullptr);
}

// ============================================================================
// Reset
// ============================================================================

TEST(LockFreeRingAllocatorReal_Reset)
{
    SmallRing ring;
    ring.Allocate(128);
    ring.Allocate(128);
    EXPECT_FALSE(ring.IsEmpty());

    ring.Reset();
    EXPECT_TRUE(ring.IsEmpty());
    EXPECT_EQ(ring.GetUsedBytes(), 0u);
}

// ============================================================================
// FIFO free order
// ============================================================================

TEST(LockFreeRingAllocatorReal_FIFOFreeOrder)
{
    SmallRing ring;
    ring.Allocate(32);
    ring.Allocate(64);

    ring.Free(); // free first
    ring.Free(); // free second
    EXPECT_TRUE(ring.IsEmpty());
}

TEST(LockFreeRingAllocatorReal_CapacityConstant)
{
    EXPECT_EQ(SmallRing::CAPACITY, 1024u);
    using BigRing = Spark::LockFreeRingAllocator<4096>;
    EXPECT_EQ(BigRing::CAPACITY, 4096u);
}
