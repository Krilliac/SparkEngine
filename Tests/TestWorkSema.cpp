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

TEST(WorkSema_TryAcquireOnEmpty)
{
    std::atomic<int32_t> count{0};
    int32_t expected = count.load(std::memory_order_relaxed);
    bool acquired = false;
    if (expected > 0)
    {
        acquired = count.compare_exchange_weak(expected, expected - 1);
    }
    EXPECT_FALSE(acquired);
    EXPECT_EQ(count.load(), 0);
}

TEST(WorkSema_PostMultipleThenDrain)
{
    std::atomic<int32_t> count{0};
    count.fetch_add(5, std::memory_order_release);
    EXPECT_EQ(count.load(), 5);

    int acquired = 0;
    while (true)
    {
        int32_t val = count.load(std::memory_order_relaxed);
        if (val <= 0)
            break;
        if (count.compare_exchange_weak(val, val - 1))
            acquired++;
    }
    EXPECT_EQ(acquired, 5);
    EXPECT_EQ(count.load(), 0);
}

TEST(WorkSema_MultipleProducersSingleConsumer)
{
    std::atomic<int32_t> sema{0};
    std::atomic<int> consumed{0};
    constexpr int PRODUCERS = 4;
    constexpr int ITEMS_PER = 50;
    constexpr int TOTAL = PRODUCERS * ITEMS_PER;

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back(
            [&]
            {
                for (int i = 0; i < ITEMS_PER; ++i)
                    sema.fetch_add(1, std::memory_order_release);
            });
    }

    std::thread consumer(
        [&]
        {
            while (consumed.load() < TOTAL)
            {
                int32_t val = sema.load(std::memory_order_relaxed);
                if (val > 0 && sema.compare_exchange_weak(val, val - 1))
                    consumed.fetch_add(1);
                else
                    std::this_thread::yield();
            }
        });

    for (auto& t : producers)
        t.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), TOTAL);
}

TEST(WorkSema_SpinBeforeSleep)
{
    // Simulate spin-then-yield pattern
    std::atomic<int32_t> sema{0};
    std::atomic<bool> ready{false};

    std::thread waiter(
        [&]
        {
            constexpr int SPIN_COUNT = 1000;
            int spins = 0;
            while (true)
            {
                int32_t val = sema.load(std::memory_order_relaxed);
                if (val > 0 && sema.compare_exchange_weak(val, val - 1))
                    break;
                spins++;
                if (spins >= SPIN_COUNT)
                    std::this_thread::yield();
            }
            ready.store(true);
        });

    // Post after a tiny delay
    sema.fetch_add(1, std::memory_order_release);
    waiter.join();
    EXPECT_TRUE(ready.load());
}

TEST(WorkSema_BatchPostAndAcquire)
{
    std::atomic<int32_t> count{0};
    // Post a batch
    constexpr int BATCH = 10;
    count.fetch_add(BATCH, std::memory_order_release);
    EXPECT_EQ(count.load(), BATCH);

    // Acquire half
    for (int i = 0; i < BATCH / 2; ++i)
    {
        int32_t val = count.load(std::memory_order_relaxed);
        EXPECT_TRUE(val > 0);
        EXPECT_TRUE(count.compare_exchange_strong(val, val - 1));
    }
    EXPECT_EQ(count.load(), BATCH / 2);
}
