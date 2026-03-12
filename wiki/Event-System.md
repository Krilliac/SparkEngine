# Event System

SparkEngine provides a type-safe publish/subscribe event bus for decoupled communication between engine subsystems.

**Source:** `SparkEngine/Source/Engine/Events/EventSystem.h`

## Overview

The `EventBus` class uses C++ RTTI (`std::type_index`) to route events by type. Each event type can have multiple subscribers, invoked synchronously in registration order.

## Basic Usage

```cpp
EventBus bus;

// Subscribe to an event type
auto id = bus.Subscribe<EntityDamagedEvent>([](const EntityDamagedEvent& e) {
    std::cout << "Entity " << e.entityId << " took " << e.damage << " damage!\n";
});

// Publish an event (notifies all subscribers)
bus.Publish(EntityDamagedEvent{ entityId, 50.0f, "Explosion" });

// Unsubscribe when done
bus.Unsubscribe<EntityDamagedEvent>(id);
```

## API Reference

### Subscribe

```cpp
template<typename T>
SubscriptionID Subscribe(std::function<void(const T&)> callback);
```

Returns a `SubscriptionID` for later unsubscription.

### Unsubscribe

```cpp
template<typename T>
bool Unsubscribe(SubscriptionID id);
```

Returns `true` if the subscription was found and removed.

### Publish

```cpp
template<typename T>
void Publish(const T& event);
```

Invokes all subscribers synchronously on the calling thread, in registration order.

### Utility Methods

```cpp
template<typename T>
void ClearSubscriptions();       // Remove all subscribers for type T

void ClearAll();                 // Remove all subscribers for all types

template<typename T>
size_t GetSubscriberCount() const; // Count subscribers for type T
```

## Built-in Event Types

### [Gameplay](Gameplay-Systems) Events

```cpp
struct EntityDamagedEvent {
    uint32_t entityId;
    float damage;
    std::string damageSource;
};

struct EntityKilledEvent {
    uint32_t entityId;
    uint32_t killerId;
    std::string cause;
};

struct ItemPickedUpEvent {
    uint32_t entityId;
    uint32_t itemDefId;
    int count;
};

struct QuestCompletedEvent {
    uint32_t entityId;
    uint32_t questId;
    std::string questName;
};

struct PlayerRespawnEvent {
    uint32_t entityId;
    float spawnX, spawnY, spawnZ;
};
```

### [World](Day-Night-Cycle-and-Weather) Events

```cpp
struct WeatherChangedEvent {
    int previousType;
    int newType;
    float intensity;
};

struct TimeOfDayChangedEvent {
    float previousHour;
    float currentHour;
    int dayCount;
};
```

### [Physics](Physics) Events

```cpp
struct CollisionEvent {
    uint32_t entityA;
    uint32_t entityB;
    float impactForce;
};
```

## Custom Events

Define your own event types as plain structs:

```cpp
struct PlayerScoredEvent {
    uint32_t playerId;
    int points;
    std::string reason;
};

// Subscribe
bus.Subscribe<PlayerScoredEvent>([](const PlayerScoredEvent& e) {
    UpdateScoreboard(e.playerId, e.points);
});

// Publish
bus.Publish(PlayerScoredEvent{ playerId, 100, "Headshot" });
```

## Accessing the Event Bus

In a game module, get the event bus from the engine context:

```cpp
bool OnLoad(Spark::IEngineContext* context) override {
    Spark::EventBus* bus = context->GetEventBus();

    bus->Subscribe<Spark::CollisionEvent>([this](const Spark::CollisionEvent& e) {
        HandleCollision(e);
    });

    return true;
}
```

## Queued Events (Thread-Safe)

Use `QueuedEventBus` when events are produced on background threads and must be dispatched on the main thread:

```cpp
QueuedEventBus queue;

// On a background thread (e.g., networking, file I/O):
queue.QueueEvent(SceneLoadedEvent{ "Level02" });
queue.QueueEvent(CollisionEvent{ entityA, entityB, 150.0f });

// On the main thread, dispatch all queued events through the bus:
EventBus bus;
bus.Subscribe<SceneLoadedEvent>([](const SceneLoadedEvent& e) {
    LOG("Scene loaded: " + e.sceneName);
});

// Each frame on the main thread:
queue.DispatchAll(bus);  // Invokes subscribers for all queued events
size_t pending = queue.GetPendingCount();
```

## Practical Example: Gameplay Event Chain

```cpp
EventBus& bus = *context->GetEventBus();

// HUD subscribes to damage events to flash the screen
bus.Subscribe<EntityDamagedEvent>([&](const EntityDamagedEvent& e) {
    if (e.entityId == localPlayerId) {
        hud.FlashDamageIndicator(e.damage, e.damageSource);
    }
});

// Audio subscribes to kills for sound feedback
bus.Subscribe<EntityKilledEvent>([&](const EntityKilledEvent& e) {
    if (e.killerId == localPlayerId) {
        audio.PlaySound("kill_confirmed");
    }
});

// Quest system listens for item pickups
bus.Subscribe<ItemPickedUpEvent>([&](const ItemPickedUpEvent& e) {
    questSystem.CheckObjective("collect", e.itemDefId, e.count);
});

// Achievement system listens for quest completion
bus.Subscribe<QuestCompletedEvent>([&](const QuestCompletedEvent& e) {
    achievements.OnQuestCompleted(e.questId, e.questName);
});
```

## Thread Safety

- `Subscribe()` and `Unsubscribe()` are thread-safe (mutex protected)
- `Publish()` is safe from any thread, but callbacks execute on the publishing thread
- The subscriber list is copied before iteration, allowing callbacks to safely modify subscriptions

## Design Notes

- The event bus is **non-copyable** but **movable**
- Events are passed by `const&` to subscribers
- Subscribers are invoked synchronously — long-running callbacks will block the publisher
- Consider keeping callbacks lightweight; defer heavy work to the next frame

---

## See Also

- [Architecture Overview](Architecture-Overview) — How subsystems communicate
- [Entity Component System](Entity-Component-System) — Entity lifecycle events
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) — Weather and time-of-day events
- [Physics](Physics) — Collision events
- [Gameplay Systems](Gameplay-Systems) — Gameplay events (damage, kills, quests)
- [Networking](Networking) — Network event replication
- [Audio](Audio) — Audio event triggers
