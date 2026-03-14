// TestEventBus.cpp - Tests for Spark::EventBus and SubscriptionHandle (new Utils/EventBus.h)
// Note: test names use "UtilEventBus_" prefix to avoid collisions with TestEventSystem.cpp
#include "TestFramework.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

// Event types used in tests
struct EBCounterEvent
{
    int value = 0;
};

struct EBStringEvent
{
    std::string message;
};

struct EBEmptyEvent
{
};

// ============================================================================
// Basic publish/subscribe
// ============================================================================

TEST(UtilEventBus_Subscribe_And_Publish)
{
    Spark::EventBus bus;
    int received = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    bus.Publish<EBCounterEvent>({5});

    EXPECT_EQ(received, 5);
}

TEST(UtilEventBus_MultipleSubscribers)
{
    Spark::EventBus bus;
    int a = 0, b = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { a += e.value; });
    auto h2 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { b += e.value; });
    bus.Publish<EBCounterEvent>({3});

    EXPECT_EQ(a, 3);
    EXPECT_EQ(b, 3);
}

TEST(UtilEventBus_MultiplePublish)
{
    Spark::EventBus bus;
    int total = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { total += e.value; });
    bus.Publish<EBCounterEvent>({1});
    bus.Publish<EBCounterEvent>({2});
    bus.Publish<EBCounterEvent>({3});

    EXPECT_EQ(total, 6);
}

TEST(UtilEventBus_DifferentEventTypes)
{
    Spark::EventBus bus;
    int counterReceived = 0;
    std::string stringReceived;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { counterReceived = e.value; });
    auto h2 = bus.Subscribe<EBStringEvent>([&](const EBStringEvent& e) { stringReceived = e.message; });

    bus.Publish<EBCounterEvent>({42});
    bus.Publish<EBStringEvent>({"hello"});

    EXPECT_EQ(counterReceived, 42);
    EXPECT_TRUE(stringReceived == "hello");
}

TEST(UtilEventBus_PublishWithNoSubscribers_NoCrash)
{
    Spark::EventBus bus;
    EXPECT_NO_THROW(bus.Publish<EBEmptyEvent>({}));
}

// ============================================================================
// SubscriptionHandle lifetime
// ============================================================================

TEST(UtilEventBus_HandleAutoUnsubscribes)
{
    Spark::EventBus bus;
    int received = 0;

    {
        auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
        bus.Publish<EBCounterEvent>({1});
        EXPECT_EQ(received, 1);
        // handle goes out of scope — unsubscribes automatically
    }

    bus.Publish<EBCounterEvent>({5});
    EXPECT_EQ(received, 1); // Should not receive after handle destroyed
}

TEST(UtilEventBus_ExplicitUnsubscribe)
{
    Spark::EventBus bus;
    int received = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    bus.Publish<EBCounterEvent>({10});
    EXPECT_EQ(received, 10);

    handle.Unsubscribe();
    bus.Publish<EBCounterEvent>({99});
    EXPECT_EQ(received, 10); // No additional delivery
}

TEST(UtilEventBus_Handle_IsActive)
{
    Spark::EventBus bus;
    auto handle = bus.Subscribe<EBEmptyEvent>([](const EBEmptyEvent&) {});
    EXPECT_TRUE(handle.IsActive());
    handle.Unsubscribe();
    EXPECT_FALSE(handle.IsActive());
}

TEST(UtilEventBus_Handle_MultipleUnsubscribe_Safe)
{
    Spark::EventBus bus;
    auto handle = bus.Subscribe<EBEmptyEvent>([](const EBEmptyEvent&) {});
    EXPECT_NO_THROW(handle.Unsubscribe());
    EXPECT_NO_THROW(handle.Unsubscribe()); // Second call should be a no-op
}

TEST(UtilEventBus_Handle_MoveConstruct)
{
    Spark::EventBus bus;
    int received = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    auto h2 = std::move(h1);

    EXPECT_FALSE(h1.IsActive());
    EXPECT_TRUE(h2.IsActive());

    bus.Publish<EBCounterEvent>({7});
    EXPECT_EQ(received, 7);
}

TEST(UtilEventBus_Handle_MoveAssign)
{
    Spark::EventBus bus;
    int received = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    Spark::SubscriptionHandle h2;
    h2 = std::move(h1);

    EXPECT_FALSE(h1.IsActive());
    EXPECT_TRUE(h2.IsActive());

    bus.Publish<EBCounterEvent>({9});
    EXPECT_EQ(received, 9);
}

// ============================================================================
// SubscriberCount and ClearSubscribers
// ============================================================================

TEST(UtilEventBus_SubscriberCount)
{
    Spark::EventBus bus;
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 0u);

    auto h1 = bus.Subscribe<EBCounterEvent>([](const EBCounterEvent&) {});
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 1u);

    auto h2 = bus.Subscribe<EBCounterEvent>([](const EBCounterEvent&) {});
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 2u);

    h1.Unsubscribe();
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 1u);
}

TEST(UtilEventBus_ClearSubscribers)
{
    Spark::EventBus bus;
    int received = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    bus.ClearSubscribers<EBCounterEvent>();

    bus.Publish<EBCounterEvent>({99});
    EXPECT_EQ(received, 0);
}

TEST(UtilEventBus_ClearAll)
{
    Spark::EventBus bus;
    int a = 0, b = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent&) { ++a; });
    auto h2 = bus.Subscribe<EBStringEvent>([&](const EBStringEvent&) { ++b; });

    bus.ClearAll();

    bus.Publish<EBCounterEvent>({1});
    bus.Publish<EBStringEvent>({"x"});

    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
}

// ============================================================================
// Global bus
// ============================================================================

TEST(UtilEventBus_Global_IsSingleton)
{
    auto& a = Spark::EventBus::Global();
    auto& b = Spark::EventBus::Global();
    EXPECT_TRUE(&a == &b);
}
