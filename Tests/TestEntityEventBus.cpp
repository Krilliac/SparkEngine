// TestEntityEventBus.cpp - Tests for per-entity event dispatch

#include "TestFramework.h"
#include "Utils/EntityEventBus.h"

using namespace Spark;

// Test event types
struct DamageEvent
{
    float amount = 0.0f;
};

struct HealEvent
{
    float amount = 0.0f;
};

TEST(EntityEventBus_PublishToSpecificEntity)
{
    EntityEventBus bus;
    float received = 0.0f;
    EventEntityID entity1 = 100;
    EventEntityID entity2 = 200;

    auto h1 = bus.Subscribe<DamageEvent>(entity1, [&](const DamageEvent& e) { received = e.amount; });

    // Publish to entity1 — handler fires
    bus.Publish<DamageEvent>(entity1, {50.0f});
    EXPECT_EQ(received, 50.0f);

    // Publish to entity2 — handler does NOT fire
    received = 0.0f;
    bus.Publish<DamageEvent>(entity2, {99.0f});
    EXPECT_EQ(received, 0.0f);
}

TEST(EntityEventBus_MultipleHandlersPerEntity)
{
    EntityEventBus bus;
    int count = 0;
    EventEntityID entity = 42;

    auto h1 = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent&) { count++; });
    auto h2 = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent&) { count++; });

    bus.Publish<DamageEvent>(entity, {10.0f});
    EXPECT_EQ(count, 2);
}

TEST(EntityEventBus_BroadcastToAllEntities)
{
    EntityEventBus bus;
    int total = 0;

    auto h1 = bus.Subscribe<DamageEvent>(1, [&](const DamageEvent& e) { total += static_cast<int>(e.amount); });
    auto h2 = bus.Subscribe<DamageEvent>(2, [&](const DamageEvent& e) { total += static_cast<int>(e.amount); });
    auto h3 = bus.Subscribe<DamageEvent>(3, [&](const DamageEvent& e) { total += static_cast<int>(e.amount); });

    bus.Broadcast<DamageEvent>({10.0f});
    EXPECT_EQ(total, 30); // 10 * 3 entities
}

TEST(EntityEventBus_UnsubscribeViaHandle)
{
    EntityEventBus bus;
    int count = 0;
    EventEntityID entity = 1;

    auto handle = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent&) { count++; });

    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1);

    handle.Unsubscribe();

    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1); // No change
}

TEST(EntityEventBus_HandleRAIIUnsubscribe)
{
    EntityEventBus bus;
    int count = 0;
    EventEntityID entity = 1;

    {
        auto handle = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent&) { count++; });
        bus.Publish<DamageEvent>(entity, {});
        EXPECT_EQ(count, 1);
    } // handle destroyed here

    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1); // No change — auto-unsubscribed
}

TEST(EntityEventBus_RemoveEntityClearsHandlers)
{
    EntityEventBus bus;
    int count = 0;
    EventEntityID entity = 42;

    auto h = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent&) { count++; });

    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1);

    bus.RemoveEntity(entity);

    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1); // Handler was removed
}

TEST(EntityEventBus_DifferentEventTypes)
{
    EntityEventBus bus;
    float damage = 0.0f;
    float heal = 0.0f;
    EventEntityID entity = 1;

    auto h1 = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent& e) { damage = e.amount; });
    auto h2 = bus.Subscribe<HealEvent>(entity, [&](const HealEvent& e) { heal = e.amount; });

    bus.Publish<DamageEvent>(entity, {25.0f});
    EXPECT_EQ(damage, 25.0f);
    EXPECT_EQ(heal, 0.0f);

    bus.Publish<HealEvent>(entity, {10.0f});
    EXPECT_EQ(heal, 10.0f);
}

TEST(EntityEventBus_EntityCount)
{
    EntityEventBus bus;

    auto h1 = bus.Subscribe<DamageEvent>(1, [](const DamageEvent&) {});
    auto h2 = bus.Subscribe<DamageEvent>(2, [](const DamageEvent&) {});
    auto h3 = bus.Subscribe<DamageEvent>(3, [](const DamageEvent&) {});

    EXPECT_EQ(bus.EntityCount<DamageEvent>(), 3u);
    EXPECT_EQ(bus.EntityCount<HealEvent>(), 0u);
}

TEST(EntityEventBus_HandlerCount)
{
    EntityEventBus bus;
    EventEntityID entity = 1;

    auto h1 = bus.Subscribe<DamageEvent>(entity, [](const DamageEvent&) {});
    auto h2 = bus.Subscribe<DamageEvent>(entity, [](const DamageEvent&) {});

    EXPECT_EQ(bus.HandlerCount<DamageEvent>(entity), 2u);
    EXPECT_EQ(bus.HandlerCount<DamageEvent>(999), 0u);
}

TEST(EntityEventBus_ClearAll)
{
    EntityEventBus bus;
    int count = 0;

    auto h = bus.Subscribe<DamageEvent>(1, [&](const DamageEvent&) { count++; });

    bus.ClearAll();

    bus.Publish<DamageEvent>(1, {});
    EXPECT_EQ(count, 0); // All handlers cleared
}

TEST(EntityEventBus_HandleMoveSemantics)
{
    EntityEventBus bus;
    int count = 0;
    EventEntityID entity = 1;

    EntitySubscriptionHandle handle;
    {
        auto tmp = bus.Subscribe<DamageEvent>(entity, [&](const DamageEvent&) { count++; });
        handle = std::move(tmp);
    }
    // tmp destroyed, but subscription moved to handle — still active
    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1);

    handle.Unsubscribe();
    bus.Publish<DamageEvent>(entity, {});
    EXPECT_EQ(count, 1);
}
