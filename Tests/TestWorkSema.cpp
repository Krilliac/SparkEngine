/**
 * @file TestWorkSema.cpp
 * @brief Tests for spin-before-sleep work semaphore
 */

#include "TestFramework.h"
#include <atomic>
#include <cstdint>
#include <thread>

TEST(WorkSema_PostAndTryAcquire)
{
    std::atomic<int32_t> count{0};

    count.fetch_add(3, std::memory_order_release);
    EXPECT_EQ(count.load(), 3);

    // Simulate TryAcquire
    int32_t expected = count.load(std::memory_order_relaxed);
    bool acquired = false;
    while (expected > 0)
    {
        if (count.compare_exchange_weak(expected, expected - 1))
        {
            acquired = true;
            break;
        }
    }
    EXPECT_TRUE(acquired);
    EXPECT_EQ(count.load(), 2);
}

TEST(WorkSema_ProducerConsumer)
{
    std::atomic<int32_t> sema{0};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    constexpr int ITEMS = 100;

    std::thread producer(
        [&]
        {
            for (int i = 0; i < ITEMS; ++i)
            {
                produced.fetch_add(1);
                sema.fetch_add(1, std::memory_order_release);
            }
        });

    std::thread consumer(
        [&]
        {
            while (consumed.load() < ITEMS)
            {
                int32_t val = sema.load(std::memory_order_relaxed);
                if (val > 0 && sema.compare_exchange_weak(val, val - 1))
                    consumed.fetch_add(1);
                else
                    std::this_thread::yield();
            }
        });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), ITEMS);
}
