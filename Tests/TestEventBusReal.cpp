/**
 * @file TestEventBusReal.cpp
 * @brief Real-class tests for Spark::EventBus and SubscriptionHandle
 */

#include "TestFramework.h"
#include "Utils/EventBus.h"

namespace
{
    struct PhaseLL_PingEvent
    {
        int value = 0;
    };

    struct PhaseLL_PongEvent
    {
        std::string message;
    };
} // namespace

TEST(EventBusReal_SingletonGlobal)
{
    auto& a = Spark::EventBus::Global();
    auto& b = Spark::EventBus::Global();
    EXPECT_TRUE(&a == &b);
}

TEST(EventBusReal_SubscribePublishRoundTrip)
{
    Spark::EventBus bus;
    int received = 0;
    auto handle = bus.Subscribe<PhaseLL_PingEvent>([&](const PhaseLL_PingEvent& e) { received = e.value; });
    bus.Publish<PhaseLL_PingEvent>({42});
    EXPECT_EQ(received, 42);
}

TEST(EventBusReal_MultipleSubscribersReceive)
{
    Spark::EventBus bus;
    int count1 = 0;
    int count2 = 0;
    auto h1 = bus.Subscribe<PhaseLL_PingEvent>([&](const PhaseLL_PingEvent&) { ++count1; });
    auto h2 = bus.Subscribe<PhaseLL_PingEvent>([&](const PhaseLL_PingEvent&) { ++count2; });
    bus.Publish<PhaseLL_PingEvent>({1});
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
}

TEST(EventBusReal_UnsubscribeStopsHandler)
{
    Spark::EventBus bus;
    int received = 0;
    auto handle = bus.Subscribe<PhaseLL_PingEvent>([&](const PhaseLL_PingEvent&) { ++received; });
    bus.Publish<PhaseLL_PingEvent>({1});
    EXPECT_EQ(received, 1);

    handle.Unsubscribe();
    bus.Publish<PhaseLL_PingEvent>({2});
    EXPECT_EQ(received, 1); // No increment after unsubscribe.
}

TEST(EventBusReal_HandleDestructionUnsubscribes)
{
    Spark::EventBus bus;
    int received = 0;
    {
        auto handle = bus.Subscribe<PhaseLL_PingEvent>([&](const PhaseLL_PingEvent&) { ++received; });
        bus.Publish<PhaseLL_PingEvent>({1});
        EXPECT_EQ(received, 1);
    }
    // Handle out of scope — should be unsubscribed.
    bus.Publish<PhaseLL_PingEvent>({2});
    EXPECT_EQ(received, 1);
}

TEST(EventBusReal_PublishWithNoSubscribersIsSafe)
{
    Spark::EventBus bus;
    bus.Publish<PhaseLL_PongEvent>({"hello"});
    // No crash.
    EXPECT_TRUE(true);
}

TEST(EventBusReal_TypeSegregation)
{
    // Subscribing to PingEvent should not receive PongEvent.
    Spark::EventBus bus;
    int pings = 0;
    int pongs = 0;
    auto h1 = bus.Subscribe<PhaseLL_PingEvent>([&](const PhaseLL_PingEvent&) { ++pings; });
    auto h2 = bus.Subscribe<PhaseLL_PongEvent>([&](const PhaseLL_PongEvent&) { ++pongs; });

    bus.Publish<PhaseLL_PingEvent>({1});
    bus.Publish<PhaseLL_PingEvent>({2});
    bus.Publish<PhaseLL_PongEvent>({"x"});

    EXPECT_EQ(pings, 2);
    EXPECT_EQ(pongs, 1);
}
