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

### Gameplay Events

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

### World Events

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

### Physics Events

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

## Thread Safety

- `Subscribe()` and `Unsubscribe()` are thread-safe (mutex protected)
- `Publish()` is safe from any thread, but callbacks execute on the publishing thread
- The subscriber list is copied before iteration, allowing callbacks to safely modify subscriptions

## Design Notes

- The event bus is **non-copyable** but **movable**
- Events are passed by `const&` to subscribers
- Subscribers are invoked synchronously — long-running callbacks will block the publisher
- Consider keeping callbacks lightweight; defer heavy work to the next frame

## See Also

- [[Architecture Overview]] — How subsystems communicate
- [[Entity Component System]] — Entity lifecycle events
