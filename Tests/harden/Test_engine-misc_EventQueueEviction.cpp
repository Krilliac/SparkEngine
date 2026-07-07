/**
 * @file Test_engine-misc_EventQueueEviction.cpp
 * @brief Regression tests for QueuedEventBus overflow eviction (EventSystem.h).
 *
 * Guards the O(1) deque-backed eviction path: once the queue saturates at
 * MaxQueueSize, each further QueueEvent evicts the oldest event from the
 * lowest-priority non-empty tier (Low, then Normal, then Critical) and
 * increments the dropped-event count, keeping total pending capped.
 */

#include "../TestFramework.h"
#include "Engine/Events/EventSystem.h"

namespace
{
    struct TestEvt
    {
        int id = 0;
    };

    constexpr size_t kMaxQueueSize = 10000;
} // namespace

TEST(QueuedEventBus_EvictsWhenFull)
{
    Spark::QueuedEventBus bus;
    for (size_t i = 0; i < kMaxQueueSize; ++i)
    {
        bus.QueueEvent(TestEvt{static_cast<int>(i)}, Spark::EventPriority::Low);
    }
    EXPECT_EQ(bus.GetPendingCount(), kMaxQueueSize);
    EXPECT_EQ(bus.GetDroppedEventCount(), static_cast<size_t>(0));

    // One more event past capacity: oldest Low is evicted, total stays capped.
    bus.QueueEvent(TestEvt{}, Spark::EventPriority::Low);
    EXPECT_EQ(bus.GetPendingCount(), kMaxQueueSize);
    EXPECT_EQ(bus.GetDroppedEventCount(), static_cast<size_t>(1));
    EXPECT_EQ(bus.GetPendingCount(Spark::EventPriority::Low), kMaxQueueSize);
}

TEST(QueuedEventBus_EvictsLowBeforeCritical)
{
    Spark::QueuedEventBus bus;
    for (size_t i = 0; i < 5; ++i)
    {
        bus.QueueEvent(TestEvt{}, Spark::EventPriority::Critical);
    }
    for (size_t i = 0; i < kMaxQueueSize - 5; ++i)
    {
        bus.QueueEvent(TestEvt{}, Spark::EventPriority::Low);
    }
    EXPECT_EQ(bus.GetPendingCount(), kMaxQueueSize);

    // Overflow must always evict from Low first; Critical stays untouched.
    for (size_t i = 0; i < 10; ++i)
    {
        bus.QueueEvent(TestEvt{}, Spark::EventPriority::Low);
    }
    EXPECT_EQ(bus.GetPendingCount(Spark::EventPriority::Critical), static_cast<size_t>(5));
    EXPECT_EQ(bus.GetPendingCount(), kMaxQueueSize);
    EXPECT_EQ(bus.GetDroppedEventCount(), static_cast<size_t>(10));
}

TEST(QueuedEventBus_ClearResetsPending)
{
    Spark::QueuedEventBus bus;
    for (size_t i = 0; i < 100; ++i)
    {
        bus.QueueEvent(TestEvt{static_cast<int>(i)});
    }
    EXPECT_EQ(bus.GetPendingCount(), static_cast<size_t>(100));

    bus.Clear();
    EXPECT_EQ(bus.GetPendingCount(), static_cast<size_t>(0));
}
