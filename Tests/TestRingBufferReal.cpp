/**
 * @file TestRingBufferReal.cpp
 * @brief Real-class tests for Spark::RingBuffer<T, N>
 */

#include "TestFramework.h"
#include "Utils/RingBuffer.h"

TEST(RingBufferReal_DefaultEmpty)
{
    Spark::RingBuffer<int, 4> rb;
    EXPECT_EQ(rb.Size(), static_cast<size_t>(0));
    EXPECT_EQ(rb.Capacity(), static_cast<size_t>(4));
    EXPECT_TRUE(rb.IsEmpty());
    EXPECT_FALSE(rb.IsFull());
}

TEST(RingBufferReal_PushIncreasesSize)
{
    Spark::RingBuffer<int, 4> rb;
    rb.Push(1);
    rb.Push(2);
    rb.Push(3);
    EXPECT_EQ(rb.Size(), static_cast<size_t>(3));
    EXPECT_FALSE(rb.IsEmpty());
    EXPECT_FALSE(rb.IsFull());
}

TEST(RingBufferReal_FillToCapacity)
{
    Spark::RingBuffer<int, 4> rb;
    rb.Push(1);
    rb.Push(2);
    rb.Push(3);
    rb.Push(4);
    EXPECT_TRUE(rb.IsFull());
    EXPECT_EQ(rb.Size(), static_cast<size_t>(4));
}

TEST(RingBufferReal_OverwriteOldest)
{
    Spark::RingBuffer<int, 3> rb;
    rb.Push(1);
    rb.Push(2);
    rb.Push(3);
    rb.Push(4); // Overwrites 1
    rb.Push(5); // Overwrites 2

    EXPECT_EQ(rb.Size(), static_cast<size_t>(3));
    EXPECT_EQ(rb.Front(), 5); // Most recent
    EXPECT_EQ(rb.Back(), 3);  // Oldest
}

TEST(RingBufferReal_IndexAccess)
{
    Spark::RingBuffer<int, 5> rb;
    rb.Push(10);
    rb.Push(20);
    rb.Push(30);
    EXPECT_EQ(rb[0], 30); // Most recent
    EXPECT_EQ(rb[1], 20);
    EXPECT_EQ(rb[2], 10);
}

TEST(RingBufferReal_Clear)
{
    Spark::RingBuffer<int, 4> rb;
    rb.Push(1);
    rb.Push(2);
    rb.Push(3);
    EXPECT_EQ(rb.Size(), static_cast<size_t>(3));

    rb.Clear();
    EXPECT_EQ(rb.Size(), static_cast<size_t>(0));
    EXPECT_TRUE(rb.IsEmpty());
    EXPECT_FALSE(rb.IsFull());
}

TEST(RingBufferReal_FrontEqualsBackForSingleElement)
{
    Spark::RingBuffer<int, 5> rb;
    rb.Push(99);
    EXPECT_EQ(rb.Front(), 99);
    EXPECT_EQ(rb.Back(), 99);
    EXPECT_EQ(rb.Size(), static_cast<size_t>(1));
}
