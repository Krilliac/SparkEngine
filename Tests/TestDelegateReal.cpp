/**
 * @file TestDelegateReal.cpp
 * @brief Real-class tests for Spark::Delegate<Args...>
 */

#include "TestFramework.h"
#include "Utils/Delegate.h"

TEST(DelegateReal_DefaultEmpty)
{
    Spark::Delegate<> d;
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(0));
}

TEST(DelegateReal_AddAndBroadcast)
{
    Spark::Delegate<int> d;
    int received = 0;
    auto id = d += [&](int v) { received = v; };
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(1));

    d.Broadcast(42);
    EXPECT_EQ(received, 42);
    (void)id;
}

TEST(DelegateReal_MultipleHandlersAllCalled)
{
    Spark::Delegate<> d;
    int count = 0;
    auto id1 = d += [&]() { ++count; };
    auto id2 = d += [&]() { ++count; };
    auto id3 = d += [&]() { ++count; };

    d.Broadcast();
    EXPECT_EQ(count, 3);
    (void)id1;
    (void)id2;
    (void)id3;
}

TEST(DelegateReal_RemoveByID)
{
    Spark::Delegate<int> d;
    int a = 0, b = 0;
    auto idA = d += [&](int v) { a = v; };
    auto idB = d += [&](int v) { b = v; };

    d.Broadcast(10);
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 10);

    d -= idA;
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(1));

    d.Broadcast(20);
    EXPECT_EQ(a, 10); // Unchanged
    EXPECT_EQ(b, 20);
}

TEST(DelegateReal_Clear)
{
    Spark::Delegate<> d;
    (void)(d += []() {});
    (void)(d += []() {});
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(2));

    d.Clear();
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(0));
}

TEST(DelegateReal_MultiArgBroadcast)
{
    Spark::Delegate<int, float, const char*> d;
    int receivedInt = 0;
    float receivedFloat = 0.0f;
    const char* receivedStr = nullptr;

    auto id = d += [&](int i, float f, const char* s)
    {
        receivedInt = i;
        receivedFloat = f;
        receivedStr = s;
    };

    d.Broadcast(42, 3.14f, "hello");
    EXPECT_EQ(receivedInt, 42);
    EXPECT_NEAR(receivedFloat, 3.14f, 0.001f);
    EXPECT_TRUE(receivedStr != nullptr);
    (void)id;
}

TEST(DelegateReal_RemoveUnknownIdIsNoOp)
{
    Spark::Delegate<> d;
    auto id = d += []() {};
    d -= 99999; // Unknown ID.
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(1));
    (void)id;
}

TEST(DelegateReal_EmptyBroadcastIsSafe)
{
    Spark::Delegate<int> d;
    d.Broadcast(42); // No handlers — should not crash.
    EXPECT_EQ(d.GetHandlerCount(), static_cast<size_t>(0));
}
