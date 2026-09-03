/**
 * @file TestFrameAllocator.cpp
 * @brief Tests for Spark::FrameAllocator
 */

#include "TestFramework.h"
#include "Utils/FrameAllocator.h"

TEST(FrameAllocator_BasicAlloc)
{
    Spark::FrameAllocator alloc(1024);
    int* p = alloc.Alloc<int>(1);
    ASSERT_TRUE(p != nullptr);
    *p = 42;
    EXPECT_EQ(*p, 42);
}

TEST(FrameAllocator_MultipleAllocs)
{
    Spark::FrameAllocator alloc(4096);
    float* a = alloc.Alloc<float>(10);
    double* b = alloc.Alloc<double>(5);
    char* c = alloc.Alloc<char>(100);
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(c != nullptr);

    // Should all be different addresses
    EXPECT_TRUE(reinterpret_cast<uintptr_t>(b) > reinterpret_cast<uintptr_t>(a));
    EXPECT_TRUE(reinterpret_cast<uintptr_t>(c) > reinterpret_cast<uintptr_t>(b));
}

TEST(FrameAllocator_Reset)
{
    Spark::FrameAllocator alloc(256);
    alloc.Alloc<int>(10); // 40 bytes
    EXPECT_TRUE(alloc.Used() >= 40);

    alloc.Reset();
    EXPECT_EQ(alloc.Used(), static_cast<size_t>(0));
}

TEST(FrameAllocator_Capacity)
{
    Spark::FrameAllocator alloc(512);
    EXPECT_EQ(alloc.Capacity(), static_cast<size_t>(512));
    EXPECT_EQ(alloc.Used(), static_cast<size_t>(0));
    EXPECT_EQ(alloc.Remaining(), static_cast<size_t>(512));
}

TEST(FrameAllocator_OutOfSpace)
{
    Spark::FrameAllocator alloc(32);
    void* p = alloc.AllocRaw(64); // Larger than capacity
    EXPECT_TRUE(p == nullptr);
}

TEST(FrameAllocator_New)
{
    Spark::FrameAllocator alloc(1024);

    struct Point
    {
        float x, y;
    };
    Point* p = alloc.New<Point>();
    EXPECT_TRUE(p != nullptr);
}

TEST(FrameAllocator_PeakTracking)
{
    Spark::FrameAllocator alloc(1024);
    alloc.Alloc<int>(10);
    alloc.UpdatePeak();
    size_t peak1 = alloc.PeakUsed();

    alloc.Reset();
    alloc.Alloc<int>(5);
    alloc.UpdatePeak();

    // Peak should still be from the larger allocation
    EXPECT_TRUE(alloc.PeakUsed() >= peak1);
}

TEST(FrameAllocator_MoveConstruct)
{
    Spark::FrameAllocator alloc(512);
    alloc.Alloc<int>(10);

    Spark::FrameAllocator moved(std::move(alloc));
    EXPECT_TRUE(moved.Capacity() == 512);
    EXPECT_EQ(alloc.Capacity(), static_cast<size_t>(0));
}
