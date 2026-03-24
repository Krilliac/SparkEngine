// TestDeferredQueue.cpp - Tests for DeferredQueue<T> general-purpose deferred queue
// Tests mark-for-deletion, flush, re-entrancy safety

#include "TestFramework.h"
#include <functional>
#include <string>
#include <vector>

// Simplified DeferredQueue for testing (mirrors engine implementation)
template <typename T> class DeferredQueue
{
  public:
    void MarkForDeletion(T item) { m_pending.push_back(std::move(item)); }

    void Flush(std::function<void(T&)> callback)
    {
        std::vector<T> local = std::move(m_pending);
        m_pending.clear();
        for (auto& item : local)
            callback(item);
    }

    void FlushAll() { m_pending.clear(); }
    [[nodiscard]] size_t GetPendingCount() const { return m_pending.size(); }
    [[nodiscard]] bool IsEmpty() const { return m_pending.empty(); }

  private:
    std::vector<T> m_pending;
};

TEST(DeferredQueue_InitialEmpty)
{
    DeferredQueue<int> q;
    EXPECT_TRUE(q.IsEmpty());
    EXPECT_EQ(q.GetPendingCount(), 0u);
}

TEST(DeferredQueue_MarkAndFlush)
{
    DeferredQueue<int> q;
    q.MarkForDeletion(1);
    q.MarkForDeletion(2);
    q.MarkForDeletion(3);
    EXPECT_EQ(q.GetPendingCount(), 3u);

    std::vector<int> flushed;
    q.Flush([&](int& val) { flushed.push_back(val); });

    EXPECT_EQ(flushed.size(), 3u);
    EXPECT_EQ(flushed[0], 1);
    EXPECT_EQ(flushed[1], 2);
    EXPECT_EQ(flushed[2], 3);
    EXPECT_TRUE(q.IsEmpty());
}

TEST(DeferredQueue_FlushAll)
{
    DeferredQueue<std::string> q;
    q.MarkForDeletion("a");
    q.MarkForDeletion("b");
    EXPECT_EQ(q.GetPendingCount(), 2u);

    q.FlushAll();
    EXPECT_TRUE(q.IsEmpty());
}

TEST(DeferredQueue_Reentrancy)
{
    // Adding items during Flush should not process them in the same Flush
    DeferredQueue<int> q;
    q.MarkForDeletion(1);

    std::vector<int> flushed;
    q.Flush(
        [&](int& val)
        {
            flushed.push_back(val);
            if (val == 1)
                q.MarkForDeletion(99);
        });

    EXPECT_EQ(flushed.size(), 1u);
    EXPECT_EQ(flushed[0], 1);
    EXPECT_EQ(q.GetPendingCount(), 1u);
}

TEST(DeferredQueue_FlushEmptyQueue)
{
    DeferredQueue<int> q;
    int callCount = 0;
    q.Flush([&](int&) { ++callCount; });
    EXPECT_EQ(callCount, 0);
}

TEST(DeferredQueue_MultipleCycles)
{
    DeferredQueue<int> q;

    q.MarkForDeletion(10);
    q.Flush([](int&) {});
    EXPECT_TRUE(q.IsEmpty());

    q.MarkForDeletion(20);
    q.MarkForDeletion(30);

    int sum = 0;
    q.Flush([&](int& val) { sum += val; });
    EXPECT_EQ(sum, 50);
    EXPECT_TRUE(q.IsEmpty());
}
