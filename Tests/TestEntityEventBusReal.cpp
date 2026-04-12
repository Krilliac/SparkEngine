/**
 * @file TestEntityEventBusReal.cpp
 * @brief Real-class tests for Spark::EntityEventBus
 */

#include "TestFramework.h"
#include "Utils/EntityEventBus.h"

namespace
{
    struct PhaseMM_DamageEvent
    {
        float amount = 0.0f;
    };

    struct PhaseMM_PickupEvent
    {
        int itemId = 0;
    };
} // namespace

TEST(EntityEventBusReal_SingletonGlobal)
{
    auto& a = Spark::EntityEventBus::Global();
    auto& b = Spark::EntityEventBus::Global();
    EXPECT_TRUE(&a == &b);
}

TEST(EntityEventBusReal_PerEntityDelivery)
{
    Spark::EntityEventBus bus;
    float received = 0.0f;
    Spark::EventEntityID targetId = 42;

    auto handle =
        bus.Subscribe<PhaseMM_DamageEvent>(targetId, [&](const PhaseMM_DamageEvent& e) { received = e.amount; });

    bus.Publish<PhaseMM_DamageEvent>(targetId, {100.0f});
    EXPECT_NEAR(received, 100.0f, 0.001f);
}

TEST(EntityEventBusReal_EntityIsolation)
{
    Spark::EntityEventBus bus;
    int entity1Count = 0;
    int entity2Count = 0;

    auto h1 = bus.Subscribe<PhaseMM_PickupEvent>(1, [&](const PhaseMM_PickupEvent&) { ++entity1Count; });
    auto h2 = bus.Subscribe<PhaseMM_PickupEvent>(2, [&](const PhaseMM_PickupEvent&) { ++entity2Count; });

    // Publish to entity 1 only — entity 2's handler should not fire.
    bus.Publish<PhaseMM_PickupEvent>(1, {100});
    EXPECT_EQ(entity1Count, 1);
    EXPECT_EQ(entity2Count, 0);

    // Publish to entity 2 only.
    bus.Publish<PhaseMM_PickupEvent>(2, {200});
    EXPECT_EQ(entity1Count, 1);
    EXPECT_EQ(entity2Count, 1);
}

TEST(EntityEventBusReal_UnsubscribeStopsHandler)
{
    Spark::EntityEventBus bus;
    int received = 0;
    auto handle = bus.Subscribe<PhaseMM_PickupEvent>(10, [&](const PhaseMM_PickupEvent&) { ++received; });
    bus.Publish<PhaseMM_PickupEvent>(10, {1});
    EXPECT_EQ(received, 1);

    handle.Unsubscribe();
    bus.Publish<PhaseMM_PickupEvent>(10, {2});
    EXPECT_EQ(received, 1);
}

TEST(EntityEventBusReal_HandleDestructorUnsubscribes)
{
    Spark::EntityEventBus bus;
    int received = 0;
    {
        auto handle = bus.Subscribe<PhaseMM_PickupEvent>(5, [&](const PhaseMM_PickupEvent&) { ++received; });
        bus.Publish<PhaseMM_PickupEvent>(5, {1});
        EXPECT_EQ(received, 1);
    }
    // Handle out of scope — should be unsubscribed.
    bus.Publish<PhaseMM_PickupEvent>(5, {2});
    EXPECT_EQ(received, 1);
}

TEST(EntityEventBusReal_PublishToEntityWithoutSubscriberIsSafe)
{
    Spark::EntityEventBus bus;
    bus.Publish<PhaseMM_DamageEvent>(999, {50.0f});
    EXPECT_TRUE(true);
}
