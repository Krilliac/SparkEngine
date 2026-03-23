// TestDelegate.cpp - Tests for Delegate multicast delegate
// Tests add/remove handlers, broadcast, clear

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Simplified Delegate for testing (mirrors engine implementation)
template <typename... Args> class Delegate
{
  public:
    using HandlerID = uint64_t;

    [[nodiscard]] HandlerID operator+=(std::function<void(Args...)> handler)
    {
        HandlerID id = m_nextId++;
        m_handlers.emplace_back(id, std::move(handler));
        return id;
    }

    void operator-=(HandlerID id)
    {
        m_handlers.erase(
            std::remove_if(m_handlers.begin(), m_handlers.end(), [id](const auto& entry) { return entry.first == id; }),
            m_handlers.end());
    }

    void Broadcast(Args... args) const
    {
        for (const auto& [id, handler] : m_handlers)
            handler(args...);
    }

    void Clear() { m_handlers.clear(); }
    [[nodiscard]] size_t GetHandlerCount() const { return m_handlers.size(); }

  private:
    std::vector<std::pair<HandlerID, std::function<void(Args...)>>> m_handlers;
    HandlerID m_nextId = 1;
};

TEST(Delegate_AddAndBroadcast)
{
    Delegate<> d;
    int callCount = 0;
    (void)(d += [&] { ++callCount; });
    d.Broadcast();
    EXPECT_EQ(callCount, 1);
}

TEST(Delegate_MultipleHandlers)
{
    Delegate<> d;
    int a = 0, b = 0;
    (void)(d += [&] { ++a; });
    (void)(d += [&] { ++b; });
    d.Broadcast();
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
}

TEST(Delegate_CallOrder)
{
    Delegate<> d;
    std::vector<int> order;
    (void)(d += [&] { order.push_back(1); });
    (void)(d += [&] { order.push_back(2); });
    (void)(d += [&] { order.push_back(3); });
    d.Broadcast();
    EXPECT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(Delegate_RemoveHandler)
{
    Delegate<> d;
    int callCount = 0;
    auto id = (d += [&] { ++callCount; });
    d -= id;
    d.Broadcast();
    EXPECT_EQ(callCount, 0);
}

TEST(Delegate_RemoveOnlyTarget)
{
    Delegate<> d;
    int a = 0, b = 0;
    auto idA = (d += [&] { ++a; });
    (void)(d += [&] { ++b; });
    d -= idA;
    d.Broadcast();
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
}

TEST(Delegate_Clear)
{
    Delegate<> d;
    (void)(d += [] {});
    (void)(d += [] {});
    EXPECT_EQ(d.GetHandlerCount(), 2u);
    d.Clear();
    EXPECT_EQ(d.GetHandlerCount(), 0u);
}

TEST(Delegate_BroadcastEmpty)
{
    Delegate<> d;
    d.Broadcast(); // Should not crash
    EXPECT_TRUE(true);
}

TEST(Delegate_WithArguments)
{
    Delegate<int, float> d;
    int receivedInt = 0;
    float receivedFloat = 0.0f;
    (void)(d +=
           [&](int i, float f)
           {
               receivedInt = i;
               receivedFloat = f;
           });
    d.Broadcast(42, 3.14f);
    EXPECT_EQ(receivedInt, 42);
    EXPECT_NEAR(receivedFloat, 3.14f, 0.001f);
}

TEST(Delegate_WithStringArgument)
{
    Delegate<const std::string&> d;
    std::string received;
    (void)(d += [&](const std::string& s) { received = s; });
    std::string msg = "hello";
    d.Broadcast(msg);
    EXPECT_EQ(received, std::string("hello"));
}
