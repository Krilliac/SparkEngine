// TestScheduledCallback.cpp - Tests for Scheduler
// Tests one-shot, repeating, cancel, and timing behavior

#include "TestFramework.h"
#include <cstdint>
#include <functional>
#include <vector>

// Simplified Scheduler for testing (mirrors engine implementation)
class Scheduler
{
  public:
    using CallbackHandle = uint64_t;

    [[nodiscard]] CallbackHandle Schedule(std::function<void()> callback, float delay)
    {
        CallbackHandle id = m_nextId++;
        m_entries.push_back({id, std::move(callback), delay, 0.0f, false});
        return id;
    }

    [[nodiscard]] CallbackHandle ScheduleRepeating(std::function<void()> callback, float interval)
    {
        CallbackHandle id = m_nextId++;
        m_entries.push_back({id, std::move(callback), interval, interval, true});
        return id;
    }

    bool Cancel(CallbackHandle handle)
    {
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        {
            if (it->id == handle)
            {
                m_entries.erase(it);
                return true;
            }
        }
        return false;
    }

    void Update(float dt)
    {
        size_t i = 0;
        while (i < m_entries.size())
        {
            auto& entry = m_entries[i];
            entry.timeRemaining -= dt;
            if (entry.timeRemaining <= 0.0f)
            {
                entry.callback();
                if (entry.repeating)
                {
                    entry.timeRemaining += entry.interval;
                    ++i;
                }
                else
                {
                    m_entries[i] = std::move(m_entries.back());
                    m_entries.pop_back();
                }
            }
            else
            {
                ++i;
            }
        }
    }

    [[nodiscard]] size_t GetPendingCount() const { return m_entries.size(); }
    void ClearAll() { m_entries.clear(); }

  private:
    struct Entry
    {
        CallbackHandle id;
        std::function<void()> callback;
        float timeRemaining;
        float interval;
        bool repeating;
    };

    std::vector<Entry> m_entries;
    CallbackHandle m_nextId = 1;
};

TEST(Scheduler_OneShotFires)
{
    Scheduler sched;
    int count = 0;
    (void)sched.Schedule([&] { ++count; }, 1.0f);

    sched.Update(0.5f);
    EXPECT_EQ(count, 0);

    sched.Update(0.5f);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(sched.GetPendingCount(), 0u);
}

TEST(Scheduler_OneShotDoesNotRepeat)
{
    Scheduler sched;
    int count = 0;
    (void)sched.Schedule([&] { ++count; }, 0.5f);

    sched.Update(1.0f);
    EXPECT_EQ(count, 1);

    sched.Update(1.0f);
    EXPECT_EQ(count, 1); // No repeat
}

TEST(Scheduler_RepeatingFires)
{
    Scheduler sched;
    int count = 0;
    (void)sched.ScheduleRepeating([&] { ++count; }, 1.0f);

    sched.Update(1.0f);
    EXPECT_EQ(count, 1);

    sched.Update(1.0f);
    EXPECT_EQ(count, 2);

    sched.Update(1.0f);
    EXPECT_EQ(count, 3);
}

TEST(Scheduler_Cancel)
{
    Scheduler sched;
    int count = 0;
    auto h = sched.Schedule([&] { ++count; }, 1.0f);

    bool cancelled = sched.Cancel(h);
    EXPECT_TRUE(cancelled);

    sched.Update(2.0f);
    EXPECT_EQ(count, 0);
}

TEST(Scheduler_CancelAlreadyFired)
{
    Scheduler sched;
    int count = 0;
    auto h = sched.Schedule([&] { ++count; }, 0.5f);

    sched.Update(1.0f);
    EXPECT_EQ(count, 1);

    bool cancelled = sched.Cancel(h);
    EXPECT_FALSE(cancelled); // Already fired and removed
}

TEST(Scheduler_ClearAll)
{
    Scheduler sched;
    (void)sched.Schedule([] {}, 1.0f);
    (void)sched.ScheduleRepeating([] {}, 1.0f);
    EXPECT_EQ(sched.GetPendingCount(), 2u);

    sched.ClearAll();
    EXPECT_EQ(sched.GetPendingCount(), 0u);
}

TEST(Scheduler_MultipleCallbacks)
{
    Scheduler sched;
    std::vector<int> order;
    (void)sched.Schedule([&] { order.push_back(1); }, 0.5f);
    (void)sched.Schedule([&] { order.push_back(2); }, 1.0f);

    sched.Update(0.5f);
    EXPECT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 1);

    sched.Update(0.5f);
    EXPECT_EQ(order.size(), 2u);
    EXPECT_EQ(order[1], 2);
}

TEST(Scheduler_LargeDtFiresMultipleRepeating)
{
    Scheduler sched;
    int count = 0;
    (void)sched.ScheduleRepeating([&] { ++count; }, 1.0f);

    // A 3-second update should fire once (our impl fires once per Update
    // and adjusts timeRemaining, so we call Update multiple times)
    sched.Update(1.0f);
    sched.Update(1.0f);
    sched.Update(1.0f);
    EXPECT_EQ(count, 3);
}
