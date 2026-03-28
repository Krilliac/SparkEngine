// TestAsyncComputeScheduler.cpp - Tests for async compute workload scheduling
// Validates scheduling logic, priority ordering, and statistics without D3D11

#include "TestFramework.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

    enum class ComputePriority : uint8_t
    {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };

    struct ComputeWorkItem
    {
        std::string name;
        uint32_t groupsX = 1;
        uint32_t groupsY = 1;
        uint32_t groupsZ = 1;
        ComputePriority priority = ComputePriority::Normal;
    };

    struct ComputeStats
    {
        uint32_t totalDispatches = 0;
        uint32_t pendingItems = 0;
        float totalFlushTimeMs = 0.0f;
    };

    // Simplified scheduler for testing
    class TestScheduler
    {
      public:
        uint64_t Submit(const ComputeWorkItem& item)
        {
            m_pending.push_back({m_nextId++, item});
            m_stats.pendingItems++;
            return m_pending.back().id;
        }

        void Flush()
        {
            // Sort by priority (highest first)
            std::sort(m_pending.begin(), m_pending.end(), [](const auto& a, const auto& b)
                      { return static_cast<uint8_t>(a.item.priority) > static_cast<uint8_t>(b.item.priority); });

            for (const auto& entry : m_pending)
            {
                m_stats.totalDispatches++;
                m_executionOrder.push_back(entry.item.name);
            }
            m_stats.pendingItems = 0;
            m_pending.clear();
        }

        void BeginFrame() { m_executionOrder.clear(); }

        const ComputeStats& GetStats() const { return m_stats; }
        const std::vector<std::string>& GetExecutionOrder() const { return m_executionOrder; }
        uint32_t GetPendingCount() const { return static_cast<uint32_t>(m_pending.size()); }

      private:
        struct PendingEntry
        {
            uint64_t id;
            ComputeWorkItem item;
        };

        std::vector<PendingEntry> m_pending;
        std::vector<std::string> m_executionOrder;
        ComputeStats m_stats;
        uint64_t m_nextId = 1;
    };

} // namespace

TEST(AsyncCompute_Submit_ReturnsValidHandle)
{
    TestScheduler scheduler;
    ComputeWorkItem item;
    item.name = "TestWork";

    uint64_t handle = scheduler.Submit(item);
    EXPECT_TRUE(handle > 0);
}

TEST(AsyncCompute_Submit_IncrementsPending)
{
    TestScheduler scheduler;
    EXPECT_EQ(scheduler.GetPendingCount(), 0u);

    scheduler.Submit({"Work1", 1, 1, 1, ComputePriority::Normal});
    EXPECT_EQ(scheduler.GetPendingCount(), 1u);

    scheduler.Submit({"Work2", 1, 1, 1, ComputePriority::Normal});
    EXPECT_EQ(scheduler.GetPendingCount(), 2u);
}

TEST(AsyncCompute_Flush_ClearsPending)
{
    TestScheduler scheduler;
    scheduler.Submit({"Work1", 1, 1, 1, ComputePriority::Normal});
    scheduler.Submit({"Work2", 1, 1, 1, ComputePriority::Normal});

    scheduler.Flush();
    EXPECT_EQ(scheduler.GetPendingCount(), 0u);
}

TEST(AsyncCompute_Flush_DispatchesAll)
{
    TestScheduler scheduler;
    scheduler.Submit({"Work1", 1, 1, 1, ComputePriority::Normal});
    scheduler.Submit({"Work2", 1, 1, 1, ComputePriority::Normal});
    scheduler.Submit({"Work3", 1, 1, 1, ComputePriority::Normal});

    scheduler.Flush();
    EXPECT_EQ(scheduler.GetStats().totalDispatches, 3u);
}

TEST(AsyncCompute_Priority_HighBeforeLow)
{
    TestScheduler scheduler;
    scheduler.Submit({"LowWork", 1, 1, 1, ComputePriority::Low});
    scheduler.Submit({"CriticalWork", 1, 1, 1, ComputePriority::Critical});
    scheduler.Submit({"NormalWork", 1, 1, 1, ComputePriority::Normal});

    scheduler.Flush();

    const auto& order = scheduler.GetExecutionOrder();
    EXPECT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "CriticalWork");
    EXPECT_EQ(order[2], "LowWork");
}

TEST(AsyncCompute_BeginFrame_ClearsOrder)
{
    TestScheduler scheduler;
    scheduler.Submit({"Work1", 1, 1, 1, ComputePriority::Normal});
    scheduler.Flush();

    EXPECT_EQ(scheduler.GetExecutionOrder().size(), 1u);

    scheduler.BeginFrame();
    EXPECT_EQ(scheduler.GetExecutionOrder().size(), 0u);
}

TEST(AsyncCompute_Stats_AccumulateAcrossFrames)
{
    TestScheduler scheduler;

    scheduler.Submit({"Frame1Work", 1, 1, 1, ComputePriority::Normal});
    scheduler.Flush();

    scheduler.BeginFrame();
    scheduler.Submit({"Frame2Work1", 1, 1, 1, ComputePriority::Normal});
    scheduler.Submit({"Frame2Work2", 1, 1, 1, ComputePriority::Normal});
    scheduler.Flush();

    EXPECT_EQ(scheduler.GetStats().totalDispatches, 3u);
}

TEST(AsyncCompute_UniqueHandles)
{
    TestScheduler scheduler;
    uint64_t h1 = scheduler.Submit({"Work1", 1, 1, 1, ComputePriority::Normal});
    uint64_t h2 = scheduler.Submit({"Work2", 1, 1, 1, ComputePriority::Normal});
    uint64_t h3 = scheduler.Submit({"Work3", 1, 1, 1, ComputePriority::Normal});

    EXPECT_TRUE(h1 != h2);
    EXPECT_TRUE(h2 != h3);
    EXPECT_TRUE(h1 != h3);
}

TEST(AsyncCompute_DispatchDimensions_Preserved)
{
    ComputeWorkItem item;
    item.name = "LargeCull";
    item.groupsX = 256;
    item.groupsY = 1;
    item.groupsZ = 1;

    EXPECT_EQ(item.groupsX, 256u);
    EXPECT_EQ(item.groupsY, 1u);
    EXPECT_EQ(item.groupsZ, 1u);
}
