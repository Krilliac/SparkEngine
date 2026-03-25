/**
 * @file TestUtilsStress.cpp
 * @brief Stress tests for core utility data structures
 *
 * These tests push RingBuffer, ThreadSafeQueue, StringUtils, and UUID
 * well beyond normal usage to expose crashes, data corruption, and
 * race conditions.
 */

#include "TestFramework.h"
#include "Utils/RingBuffer.h"
#include "Utils/ThreadSafeQueue.h"
#include "Utils/EventBus.h"
#include "Utils/StringUtils.h"
#include "Utils/UUID.h"

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// =============================================================================
// RingBuffer stress tests
// =============================================================================

TEST(UtilsStress_RingBufferOverflow)
{
    // Push 10000 items into a capacity-64 buffer.
    // Oldest items must be silently overwritten; no crash allowed.
    Spark::RingBuffer<int, 64> ring;

    for (int i = 0; i < 10000; ++i)
    {
        ring.Push(i);
    }

    EXPECT_EQ(ring.Size(), static_cast<size_t>(64));
    EXPECT_TRUE(ring.IsFull());

    // Most recent item should be 9999
    EXPECT_EQ(ring.Front(), 9999);

    // Oldest item should be 10000 - 64 = 9936
    EXPECT_EQ(ring.Back(), 9936);
}

TEST(UtilsStress_RingBufferEmptyPop)
{
    // Access an empty buffer. operator[] on an empty buffer accesses
    // default-constructed storage, so it should return T{} without crashing.
    Spark::RingBuffer<int, 8> ring;

    EXPECT_TRUE(ring.IsEmpty());
    EXPECT_EQ(ring.Size(), static_cast<size_t>(0));

    // Accessing index 0 on empty buffer reads default-initialized storage
    // This should not crash even though Size() is 0
    EXPECT_NO_THROW((void)ring[0]);
}

TEST(UtilsStress_RingBufferRapidPushPop)
{
    // Alternate push/pop (via Clear + re-push) 50000 times to stress
    // the head/count arithmetic for wrap-around correctness.
    Spark::RingBuffer<int, 16> ring;

    for (int i = 0; i < 50000; ++i)
    {
        ring.Push(i);

        // Every 16 pushes, verify front is correct and clear
        if ((i + 1) % 16 == 0)
        {
            EXPECT_EQ(ring.Front(), i);
            EXPECT_TRUE(ring.IsFull());
            ring.Clear();
            EXPECT_TRUE(ring.IsEmpty());
        }
    }

    // Final state: after the last batch of pushes since last clear
    EXPECT_TRUE(ring.Size() <= 16);
}

TEST(UtilsStress_RingBufferSingleCapacity)
{
    // Capacity=1 is the tightest edge case: every push overwrites the only slot.
    Spark::RingBuffer<int, 1> ring;

    EXPECT_TRUE(ring.IsEmpty());

    ring.Push(42);
    EXPECT_EQ(ring.Size(), static_cast<size_t>(1));
    EXPECT_TRUE(ring.IsFull());
    EXPECT_EQ(ring.Front(), 42);
    EXPECT_EQ(ring.Back(), 42);

    ring.Push(99);
    EXPECT_EQ(ring.Size(), static_cast<size_t>(1));
    EXPECT_EQ(ring.Front(), 99);

    ring.Clear();
    EXPECT_TRUE(ring.IsEmpty());

    // Push/read cycle
    for (int i = 0; i < 1000; ++i)
    {
        ring.Push(i);
        EXPECT_EQ(ring.Front(), i);
    }
}

// =============================================================================
// ThreadSafeQueue stress tests
// =============================================================================

TEST(UtilsStress_ThreadSafeQueueConcurrentPushPop)
{
    // 4 producers push 10000 items each, 4 consumers pop.
    // Total items pushed = 40000; all must be delivered exactly once.
    constexpr int kItemsPerProducer = 10000;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kTotalItems = kItemsPerProducer * kProducers;

    Spark::ThreadSafeQueue<int> queue;
    std::atomic<int> totalPopped{0};
    std::atomic<bool> producersDone{false};

    // Producer threads
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back(
            [&queue, p]
            {
                for (int i = 0; i < kItemsPerProducer; ++i)
                {
                    queue.ForcePush(p * kItemsPerProducer + i);
                }
            });
    }

    // Consumer threads
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c)
    {
        consumers.emplace_back(
            [&queue, &totalPopped, &producersDone]
            {
                int localCount = 0;
                while (true)
                {
                    int item = 0;
                    if (queue.TryPop(item))
                    {
                        ++localCount;
                    }
                    else if (producersDone.load(std::memory_order_acquire) && queue.IsEmpty())
                    {
                        break;
                    }
                }
                totalPopped.fetch_add(localCount, std::memory_order_relaxed);
            });
    }

    for (auto& t : producers)
    {
        t.join();
    }
    producersDone.store(true, std::memory_order_release);

    for (auto& t : consumers)
    {
        t.join();
    }

    EXPECT_EQ(totalPopped.load(), kTotalItems);
}

TEST(UtilsStress_ThreadSafeQueueBoundedCapacity)
{
    // Queue with maxSize=100. TryPush should fail when full.
    Spark::ThreadSafeQueue<int> queue(100);

    int accepted = 0;
    for (int i = 0; i < 200; ++i)
    {
        if (queue.TryPush(i))
        {
            ++accepted;
        }
    }

    // Exactly 100 should have been accepted
    EXPECT_EQ(accepted, 100);
    EXPECT_EQ(queue.Size(), static_cast<size_t>(100));
}

TEST(UtilsStress_ThreadSafeQueueEmptyPoll)
{
    // TryPop on an empty queue should return false without crashing.
    Spark::ThreadSafeQueue<int> queue;

    int value = -1;
    for (int i = 0; i < 1000; ++i)
    {
        bool result = queue.TryPop(value);
        EXPECT_FALSE(result);
    }

    // Value should remain untouched on failed pop
    EXPECT_EQ(value, -1);
}

TEST(UtilsStress_ThreadSafeQueueRapidClear)
{
    // Push items, clear, push again. Verify clean state after clear.
    Spark::ThreadSafeQueue<int> queue;

    for (int cycle = 0; cycle < 10; ++cycle)
    {
        // Push 1000 items
        for (int i = 0; i < 1000; ++i)
        {
            queue.ForcePush(cycle * 1000 + i);
        }
        EXPECT_EQ(queue.Size(), static_cast<size_t>(1000));

        queue.Clear();
        EXPECT_TRUE(queue.IsEmpty());
        EXPECT_EQ(queue.Size(), static_cast<size_t>(0));

        // TryPop should fail on cleared queue
        int val = 0;
        EXPECT_FALSE(queue.TryPop(val));
    }

    // Final push after all clears to verify queue is still functional
    queue.ForcePush(12345);
    int result = 0;
    EXPECT_TRUE(queue.TryPop(result));
    EXPECT_EQ(result, 12345);
}

// =============================================================================
// StringUtils stress tests
// =============================================================================

TEST(UtilsStress_StringUtilsEmptyString)
{
    // All operations on empty string must not crash.
    using namespace Spark::StringUtils;

    std::string empty;

    EXPECT_NO_THROW((void)ToLower(empty));
    EXPECT_NO_THROW((void)ToUpper(empty));
    EXPECT_NO_THROW((void)Trim(empty));
    EXPECT_NO_THROW((void)TrimLeft(empty));
    EXPECT_NO_THROW((void)TrimRight(empty));
    EXPECT_NO_THROW((void)Split(empty, ','));
    EXPECT_NO_THROW((void)Split(empty, std::string(",")));
    EXPECT_NO_THROW((void)Replace(empty, "a", "b"));
    EXPECT_NO_THROW((void)ReplaceAll(empty, "a", "b"));
    EXPECT_NO_THROW((void)StartsWith(empty, "x"));
    EXPECT_NO_THROW((void)EndsWith(empty, "x"));
    EXPECT_NO_THROW((void)Contains(empty, "x"));

    EXPECT_EQ(ToLower(empty), std::string(""));
    EXPECT_EQ(Trim(empty), std::string(""));

    auto parts = Split(empty, ',');
    // Engine's Split returns 0 parts for empty string (no content to split)
    EXPECT_TRUE(parts.size() <= 1);
}

TEST(UtilsStress_StringUtilsMassiveString)
{
    // Build a 1MB string and run operations on it. Must not crash or hang.
    using namespace Spark::StringUtils;

    constexpr size_t kSize = 1024 * 1024; // 1MB
    std::string massive(kSize, 'A');

    // Insert some delimiters
    for (size_t i = 0; i < kSize; i += 1024)
    {
        massive[i] = ',';
    }

    EXPECT_NO_THROW((void)ToLower(massive));
    EXPECT_NO_THROW((void)Trim(massive));

    auto parts = Split(massive, ',');
    EXPECT_GT(parts.size(), static_cast<size_t>(1));

    EXPECT_NO_THROW((void)Replace(massive, "AAA", "BBB"));
    EXPECT_NO_THROW((void)ReplaceAll(massive, "AA", "CC"));
}

TEST(UtilsStress_StringUtilsNullTerminator)
{
    // String with embedded null characters. std::string handles these,
    // but some C-style operations might not. Verify no crash.
    using namespace Spark::StringUtils;

    std::string withNull = "hello";
    withNull.push_back('\0');
    withNull += "world";

    // String should be 11 chars: "hello\0world"
    EXPECT_EQ(withNull.size(), static_cast<size_t>(11));

    EXPECT_NO_THROW((void)ToLower(withNull));
    EXPECT_NO_THROW((void)ToUpper(withNull));
    EXPECT_NO_THROW((void)Trim(withNull));
    EXPECT_NO_THROW((void)Split(withNull, ','));
    EXPECT_NO_THROW((void)Replace(withNull, "hello", "HI"));
    EXPECT_NO_THROW((void)Contains(withNull, "world"));
}

// =============================================================================
// UUID stress tests
// =============================================================================

TEST(UtilsStress_UUIDMassiveGeneration)
{
    // Generate 10000 UUIDs. All must be valid and unique.
    constexpr int kCount = 10000;
    std::unordered_set<uint64_t> seen;
    seen.reserve(kCount);

    for (int i = 0; i < kCount; ++i)
    {
        Spark::UUID id = Spark::UUID::Generate();
        EXPECT_TRUE(id.IsValid());
        seen.insert(id.GetValue());
    }

    EXPECT_EQ(seen.size(), static_cast<size_t>(kCount));
}

TEST(UtilsStress_UUIDCollisionCheck)
{
    // Generate 50000 UUIDs and verify zero collisions.
    constexpr int kCount = 50000;
    std::unordered_set<uint64_t> seen;
    seen.reserve(kCount);

    int collisions = 0;
    for (int i = 0; i < kCount; ++i)
    {
        Spark::UUID id = Spark::UUID::Generate();
        auto [it, inserted] = seen.insert(id.GetValue());
        if (!inserted)
        {
            ++collisions;
        }
    }

    EXPECT_EQ(collisions, 0);
    EXPECT_EQ(seen.size(), static_cast<size_t>(kCount));
}
