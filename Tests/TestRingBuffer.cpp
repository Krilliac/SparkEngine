// TestRingBuffer.cpp - Tests for generic ring buffer templates
// Uses the actual RingBuffer.h header

#include "TestFramework.h"
#include "Utils/RingBuffer.h"

// =============================================================================
// Tests - Fixed RingBuffer
// =============================================================================

TEST(RingBuffer_Empty) {
    Spark::RingBuffer<int, 4> buf;
    EXPECT_TRUE(buf.IsEmpty());
    EXPECT_FALSE(buf.IsFull());
    EXPECT_EQ(buf.Size(), (size_t)0);
    EXPECT_EQ(buf.Capacity(), (size_t)4);
}

TEST(RingBuffer_PushAndAccess) {
    Spark::RingBuffer<int, 4> buf;
    buf.Push(10);
    buf.Push(20);
    buf.Push(30);

    EXPECT_EQ(buf.Size(), (size_t)3);
    EXPECT_EQ(buf[0], 30);  // Most recent
    EXPECT_EQ(buf[1], 20);
    EXPECT_EQ(buf[2], 10);  // Oldest
}

TEST(RingBuffer_FrontBack) {
    Spark::RingBuffer<int, 4> buf;
    buf.Push(1);
    buf.Push(2);
    buf.Push(3);

    EXPECT_EQ(buf.Front(), 3);  // Most recent
    EXPECT_EQ(buf.Back(), 1);   // Oldest
}

TEST(RingBuffer_Overflow) {
    Spark::RingBuffer<int, 3> buf;
    buf.Push(1);
    buf.Push(2);
    buf.Push(3);
    EXPECT_TRUE(buf.IsFull());
    EXPECT_EQ(buf.Size(), (size_t)3);

    buf.Push(4);  // Overwrites oldest (1)
    EXPECT_EQ(buf.Size(), (size_t)3);  // Still 3
    EXPECT_EQ(buf[0], 4);  // Most recent
    EXPECT_EQ(buf[1], 3);
    EXPECT_EQ(buf[2], 2);  // Oldest now (1 was overwritten)
}

TEST(RingBuffer_Clear) {
    Spark::RingBuffer<int, 4> buf;
    buf.Push(1);
    buf.Push(2);
    EXPECT_EQ(buf.Size(), (size_t)2);

    buf.Clear();
    EXPECT_TRUE(buf.IsEmpty());
    EXPECT_EQ(buf.Size(), (size_t)0);

    // Should work after clear
    buf.Push(10);
    EXPECT_EQ(buf[0], 10);
}

TEST(RingBuffer_SingleElement) {
    Spark::RingBuffer<float, 1> buf;
    buf.Push(1.0f);
    EXPECT_TRUE(buf.IsFull());
    EXPECT_NEAR(buf[0], 1.0f, 0.001f);

    buf.Push(2.0f);
    EXPECT_NEAR(buf[0], 2.0f, 0.001f);
    EXPECT_EQ(buf.Size(), (size_t)1);
}

TEST(RingBuffer_Iterator) {
    Spark::RingBuffer<int, 8> buf;
    buf.Push(10);
    buf.Push(20);
    buf.Push(30);

    // Iterator goes oldest to newest
    std::vector<int> values;
    for (int v : buf) values.push_back(v);

    EXPECT_EQ((int)values.size(), 3);
    EXPECT_EQ(values[0], 10);  // Oldest
    EXPECT_EQ(values[1], 20);
    EXPECT_EQ(values[2], 30);  // Newest
}

TEST(RingBuffer_StringType) {
    Spark::RingBuffer<std::string, 3> buf;
    buf.Push("hello");
    buf.Push("world");
    EXPECT_EQ(buf[0], std::string("world"));
    EXPECT_EQ(buf[1], std::string("hello"));
}

TEST(RingBuffer_MutableAccess) {
    Spark::RingBuffer<int, 4> buf;
    buf.Push(10);
    buf[0] = 99;
    EXPECT_EQ(buf[0], 99);
}

// =============================================================================
// Tests - Dynamic RingBuffer
// =============================================================================

TEST(DynamicRingBuffer_Basic) {
    Spark::DynamicRingBuffer<int> buf(4);
    EXPECT_TRUE(buf.IsEmpty());
    EXPECT_EQ(buf.Capacity(), (size_t)4);

    buf.Push(1);
    buf.Push(2);
    buf.Push(3);
    EXPECT_EQ(buf.Size(), (size_t)3);
    EXPECT_EQ(buf[0], 3);
    EXPECT_EQ(buf[2], 1);
}

TEST(DynamicRingBuffer_Overflow) {
    Spark::DynamicRingBuffer<int> buf(3);
    buf.Push(1);
    buf.Push(2);
    buf.Push(3);
    buf.Push(4);

    EXPECT_EQ(buf.Size(), (size_t)3);
    EXPECT_EQ(buf[0], 4);
    EXPECT_EQ(buf[2], 2);
}

TEST(DynamicRingBuffer_Clear) {
    Spark::DynamicRingBuffer<int> buf(4);
    buf.Push(1);
    buf.Push(2);
    buf.Clear();
    EXPECT_TRUE(buf.IsEmpty());
}

TEST(DynamicRingBuffer_Resize) {
    Spark::DynamicRingBuffer<int> buf(4);
    buf.Push(1);
    buf.Push(2);

    buf.Resize(8);
    EXPECT_EQ(buf.Capacity(), (size_t)8);
    EXPECT_TRUE(buf.IsEmpty());  // Resize clears
}

TEST(DynamicRingBuffer_FrontBack) {
    Spark::DynamicRingBuffer<int> buf(8);
    buf.Push(10);
    buf.Push(20);
    buf.Push(30);

    EXPECT_EQ(buf.Front(), 30);
    EXPECT_EQ(buf.Back(), 10);
}
