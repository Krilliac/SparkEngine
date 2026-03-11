// TestThreadSafeQueue.cpp - Tests for Spark::ThreadSafeQueue
#include "TestFramework.h"
#include "Utils/ThreadSafeQueue.h"
#include <string>
#include <thread>

TEST(ThreadSafeQueue_EmptyOnConstruction)
{
    Spark::ThreadSafeQueue<int> queue;
    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(queue.Size(), 0u);
}

TEST(ThreadSafeQueue_TryPushAndPop)
{
    Spark::ThreadSafeQueue<int> queue;
    EXPECT_TRUE(queue.TryPush(42));
    EXPECT_EQ(queue.Size(), 1u);

    int val = 0;
    EXPECT_TRUE(queue.TryPop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(queue.IsEmpty());
}

TEST(ThreadSafeQueue_TryPopEmpty)
{
    Spark::ThreadSafeQueue<int> queue;
    int val = -1;
    EXPECT_FALSE(queue.TryPop(val));
    EXPECT_EQ(val, -1);
}

TEST(ThreadSafeQueue_FIFOOrder)
{
    Spark::ThreadSafeQueue<int> queue;
    queue.TryPush(1);
    queue.TryPush(2);
    queue.TryPush(3);

    int val = 0;
    queue.TryPop(val);
    EXPECT_EQ(val, 1);
    queue.TryPop(val);
    EXPECT_EQ(val, 2);
    queue.TryPop(val);
    EXPECT_EQ(val, 3);
}

TEST(ThreadSafeQueue_BoundedCapacity)
{
    Spark::ThreadSafeQueue<int> queue(2);
    EXPECT_TRUE(queue.TryPush(1));
    EXPECT_TRUE(queue.TryPush(2));
    EXPECT_FALSE(queue.TryPush(3)); // at capacity
}

TEST(ThreadSafeQueue_ForcePushIgnoresCapacity)
{
    Spark::ThreadSafeQueue<int> queue(2);
    queue.TryPush(1);
    queue.TryPush(2);
    queue.ForcePush(3);
    EXPECT_EQ(queue.Size(), 3u);
}

TEST(ThreadSafeQueue_PopAll)
{
    Spark::ThreadSafeQueue<int> queue;
    queue.TryPush(10);
    queue.TryPush(20);
    queue.TryPush(30);

    std::vector<int> batch;
    queue.PopAll(batch);
    EXPECT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0], 10);
    EXPECT_EQ(batch[1], 20);
    EXPECT_EQ(batch[2], 30);
    EXPECT_TRUE(queue.IsEmpty());
}

TEST(ThreadSafeQueue_Clear)
{
    Spark::ThreadSafeQueue<int> queue;
    queue.TryPush(1);
    queue.TryPush(2);
    queue.Clear();
    EXPECT_TRUE(queue.IsEmpty());
}

TEST(ThreadSafeQueue_StringType)
{
    Spark::ThreadSafeQueue<std::string> queue;
    queue.TryPush("hello");
    queue.TryPush("world");

    std::string val;
    EXPECT_TRUE(queue.TryPop(val));
    EXPECT_TRUE(val == "hello");
    EXPECT_TRUE(queue.TryPop(val));
    EXPECT_TRUE(val == "world");
}

TEST(ThreadSafeQueue_ConcurrentPushPop)
{
    Spark::ThreadSafeQueue<int> queue;
    constexpr int N = 1000;

    std::thread producer(
        [&]()
        {
            for (int i = 0; i < N; ++i)
                queue.ForcePush(i);
        });

    std::thread consumer(
        [&]()
        {
            int consumed = 0;
            while (consumed < N)
            {
                int val;
                if (queue.TryPop(val))
                    ++consumed;
            }
        });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.IsEmpty());
}
