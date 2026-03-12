# Event System

SparkEngine provides a type-safe publish/subscribe event bus for decoupled communication between engine subsystems.

**Source:** `SparkEngine/Source/Engine/Events/EventSystem.h`

## Overview

The `EventBus` class uses C++ RTTI (`std::type_index`) to route events by type. Each event type can have multiple subscribers, invoked synchronously in registration order. The event bus is the primary mechanism for systems to communicate without direct dependencies.

```
┌──────────────┐    Publish(event)     ┌──────────────┐
│  Publisher    │ ──────────────────── │   EventBus    │
│ (any system) │                       │               │
└──────────────┘                       │  type_index   │
                                       │  → callbacks  │
┌──────────────┐    Subscribe<T>()     │               │
│  Subscriber  │ ──────────────────── │               │
│ (any system) │ ◄── callback(event)   └──────────────┘
└──────────────┘
```

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

### EventBus Class

The `EventBus` is non-copyable but movable. It uses an internal mutex for thread-safe subscribe/unsubscribe.

#### Subscribe

```cpp
template<typename T>
SubscriptionID Subscribe(std::function<void(const T&)> callback);
```

Registers a callback for events of type `T`. Returns a `SubscriptionID` (a `uint64_t`) for later unsubscription. IDs are monotonically increasing and never reused.

#### Unsubscribe

```cpp
template<typename T>
bool Unsubscribe(SubscriptionID id);
```

Removes a previously registered callback. Returns `true` if the subscription was found and removed, `false` if not found.

#### Publish

```cpp
template<typename T>
void Publish(const T& event);
```

Invokes all subscribers synchronously on the calling thread, in registration order. The event is passed by `const&`. The subscriber list is copied before iteration, allowing callbacks to safely modify subscriptions during dispatch.

#### ClearSubscriptions

```cpp
template<typename T>
void ClearSubscriptions();       // Remove all subscribers for type T
```

#### ClearAll

```cpp
void ClearAll();                 // Remove all subscribers for all types
```

#### GetSubscriberCount

```cpp
template<typename T>
size_t GetSubscriberCount() const; // Count subscribers for type T
```

### SubscriptionID

```cpp
using SubscriptionID = uint64_t;
```

Unique identifier returned by `Subscribe()`. Store this to later call `Unsubscribe()`.

## Built-in Event Types

### Gameplay Events

```cpp
struct EntityDamagedEvent {
    uint32_t entityId = 0;       // Entity that took damage
    float damage = 0.0f;         // Damage amount
    std::string damageSource;    // Source description ("Explosion", "Weapon")
};

struct EntityKilledEvent {
    uint32_t entityId = 0;       // Entity that was killed
    uint32_t killerId = 0;       // Entity that did the killing
    std::string cause;           // Cause of death
};

struct ItemPickedUpEvent {
    uint32_t entityId = 0;       // Entity that picked up the item
    uint32_t itemDefId = 0;      // Item definition ID
    int count = 1;               // Number of items picked up
};

struct QuestCompletedEvent {
    uint32_t entityId = 0;       // Entity that completed the quest
    uint32_t questId = 0;        // Quest ID
    std::string questName;       // Quest display name
};

struct PlayerRespawnEvent {
    uint32_t entityId = 0;       // Respawning player entity
    float spawnX = 0.0f;         // Spawn position X
    float spawnY = 0.0f;         // Spawn position Y
    float spawnZ = 0.0f;         // Spawn position Z
};
```

### Engine Lifecycle Events

```cpp
struct EngineStartEvent {};       // Engine finished initialization

struct EngineShutdownEvent {};    // Engine beginning shutdown

struct FrameBeginEvent {
    float deltaTime = 0.0f;      // Time since previous frame (seconds)
};

struct FrameEndEvent {
    float deltaTime = 0.0f;      // Time during this frame (seconds)
};
```

### Scene Events

```cpp
struct SceneLoadedEvent {
    std::string sceneName;        // Name or path of the loaded scene
};

struct SceneUnloadedEvent {
    std::string sceneName;        // Name or path of the unloaded scene
};
```

### Entity Events

```cpp
struct EntityCreatedEvent {
    uint32_t entityId = 0;        // ID of the newly created entity
};

struct EntityDestroyedEvent {
    uint32_t entityId = 0;        // ID of the destroyed entity
};

struct EntityDeathEvent {
    uint32_t entityId = 0;        // ID of the dead entity
};
```

### World Events

```cpp
struct WeatherChangedEvent {
    int previousType = 0;         // Previous weather type enum
    int newType = 0;              // New weather type enum
    float intensity = 0.0f;       // Weather intensity [0,1]
};

struct TimeOfDayChangedEvent {
    float previousHour = 0.0f;    // Previous hour [0,24)
    float currentHour = 0.0f;     // Current hour [0,24)
    int dayCount = 0;             // Total days elapsed
};
```

### Physics Events

```cpp
struct CollisionEvent {
    uint32_t entityA = 0;         // First entity in the collision
    uint32_t entityB = 0;         // Second entity in the collision
    float impactForce = 0.0f;     // Force of the impact
};

struct TriggerEnterEvent {
    uint32_t entityId = 0;        // Entity that entered the trigger
    uint32_t triggerId = 0;       // Trigger volume entity
};

struct TriggerExitEvent {
    uint32_t entityId = 0;        // Entity that exited the trigger
    uint32_t triggerId = 0;       // Trigger volume entity
};
```

### Input Events

```cpp
struct InputActionEvent {
    std::string actionName;       // Named action binding
    bool pressed = false;         // true = pressed, false = released
};

struct MouseMoveEvent {
    float deltaX = 0.0f;         // Horizontal pixels since last frame
    float deltaY = 0.0f;         // Vertical pixels since last frame
};
```

### Graphics Events

```cpp
struct WindowResizeEvent {
    uint32_t width = 0;           // New width in pixels
    uint32_t height = 0;          // New height in pixels
};

struct QualityChangedEvent {
    std::string preset;           // Quality preset name ("Low", "Ultra")
};
```

### Audio Events

```cpp
struct SoundPlayedEvent {
    std::string soundName;        // Sound asset name or path
};
```

### Memory Events

```cpp
struct MemoryPressureEvent {
    float usagePercent = 0.0f;    // Memory usage [0, 100]
    size_t availableBytes = 0;    // Remaining available memory
};
```

## Complete Event Type Summary

| Category | Event | Key Fields |
|----------|-------|------------|
| Gameplay | `EntityDamagedEvent` | entityId, damage, damageSource |
| Gameplay | `EntityKilledEvent` | entityId, killerId, cause |
| Gameplay | `ItemPickedUpEvent` | entityId, itemDefId, count |
| Gameplay | `QuestCompletedEvent` | entityId, questId, questName |
| Gameplay | `PlayerRespawnEvent` | entityId, spawnX/Y/Z |
| Lifecycle | `EngineStartEvent` | (empty) |
| Lifecycle | `EngineShutdownEvent` | (empty) |
| Lifecycle | `FrameBeginEvent` | deltaTime |
| Lifecycle | `FrameEndEvent` | deltaTime |
| Scene | `SceneLoadedEvent` | sceneName |
| Scene | `SceneUnloadedEvent` | sceneName |
| Entity | `EntityCreatedEvent` | entityId |
| Entity | `EntityDestroyedEvent` | entityId |
| Entity | `EntityDeathEvent` | entityId |
| World | `WeatherChangedEvent` | previousType, newType, intensity |
| World | `TimeOfDayChangedEvent` | previousHour, currentHour, dayCount |
| Physics | `CollisionEvent` | entityA, entityB, impactForce |
| Physics | `TriggerEnterEvent` | entityId, triggerId |
| Physics | `TriggerExitEvent` | entityId, triggerId |
| Input | `InputActionEvent` | actionName, pressed |
| Input | `MouseMoveEvent` | deltaX, deltaY |
| Graphics | `WindowResizeEvent` | width, height |
| Graphics | `QualityChangedEvent` | preset |
| Audio | `SoundPlayedEvent` | soundName |
| Memory | `MemoryPressureEvent` | usagePercent, availableBytes |

## Custom Events

Define your own event types as plain structs. No registration or base class is needed -- the event bus uses `std::type_index` to match types automatically:

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

### Custom Event Best Practices

1. **Use POD structs** -- Keep events as simple data containers with no behavior
2. **Default-initialize fields** -- Provide sensible defaults for all members
3. **Const reference semantics** -- Events are passed by `const&`; do not store pointers to them
4. **Name with Event suffix** -- Use descriptive names ending in `Event` (e.g., `DoorOpenedEvent`)
5. **Keep events small** -- Avoid large payloads; use entity IDs to look up data instead

## Accessing the Event Bus

### From a Game Module

```cpp
bool OnLoad(Spark::IEngineContext* context) override {
    Spark::EventBus* bus = context->GetEventBus();

    bus->Subscribe<Spark::CollisionEvent>([this](const Spark::CollisionEvent& e) {
        HandleCollision(e);
    });

    bus->Subscribe<Spark::SceneLoadedEvent>([this](const Spark::SceneLoadedEvent& e) {
        OnSceneLoaded(e.sceneName);
    });

    return true;
}
```

### From an Engine Subsystem

```cpp
// Via the EngineContext singleton
auto* bus = EngineContext::Get()->GetEventBus();
bus->Publish(EngineStartEvent{});
```

## Queued Events (Thread-Safe Deferred Dispatch)

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

### QueuedEventBus API

| Method | Thread Safety | Description |
|--------|--------------|-------------|
| `QueueEvent(T event)` | Any thread | Queue an event for later dispatch |
| `DispatchAll(EventBus& bus)` | Main thread | Publish all queued events, then clear the queue |
| `GetPendingCount() const` | Any thread | Number of events waiting in the queue |
| `Clear()` | Any thread | Discard all queued events without dispatching |

### QueuedEventBus Internals

`QueueEvent()` type-erases the event into a `std::function<void(EventBus&)>` and stores it in a mutex-protected vector. `DispatchAll()` acquires the lock, swaps the queue, releases the lock, then dispatches events without holding the lock. This minimizes contention with producer threads.

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

// Scene manager publishes load events
bus.Subscribe<SceneLoadedEvent>([&](const SceneLoadedEvent& e) {
    LOG("Scene loaded: " + e.sceneName);
    physics.SetGravity(sceneManager.GetMetadata().gravityY);
});

// Frame timing for profiling
bus.Subscribe<FrameBeginEvent>([&](const FrameBeginEvent& e) {
    profiler.BeginFrame(e.deltaTime);
});
bus.Subscribe<FrameEndEvent>([&](const FrameEndEvent& e) {
    profiler.EndFrame(e.deltaTime);
});
```

## Practical Example: Subscription Lifetime Management

```cpp
class MyGameSystem
{
public:
    void Initialize(EventBus* bus)
    {
        m_bus = bus;

        // Store subscription IDs for cleanup
        m_subDamage = bus->Subscribe<EntityDamagedEvent>(
            [this](const EntityDamagedEvent& e) { OnDamage(e); });

        m_subKill = bus->Subscribe<EntityKilledEvent>(
            [this](const EntityKilledEvent& e) { OnKill(e); });
    }

    void Shutdown()
    {
        // Clean up subscriptions
        m_bus->Unsubscribe<EntityDamagedEvent>(m_subDamage);
        m_bus->Unsubscribe<EntityKilledEvent>(m_subKill);
    }

private:
    void OnDamage(const EntityDamagedEvent& e) { /* ... */ }
    void OnKill(const EntityKilledEvent& e) { /* ... */ }

    EventBus* m_bus = nullptr;
    SubscriptionID m_subDamage = 0;
    SubscriptionID m_subKill = 0;
};
```

## Thread Safety

| Operation | Thread Safety | Notes |
|-----------|--------------|-------|
| `Subscribe()` | Thread-safe | Mutex protected |
| `Unsubscribe()` | Thread-safe | Mutex protected |
| `Publish()` | Thread-safe | Callbacks execute on the publishing thread |
| `ClearSubscriptions<T>()` | Thread-safe | Mutex protected |
| `ClearAll()` | Thread-safe | Mutex protected |
| `GetSubscriberCount<T>()` | Thread-safe | Mutex protected |

**Important:** The subscriber list is copied before iteration during `Publish()`. This means callbacks can safely call `Subscribe()`, `Unsubscribe()`, or even `Publish()` for other event types without causing iterator invalidation.

## Design Notes

- The event bus is **non-copyable** but **movable**
- Events are passed by `const&` to subscribers -- do not modify them
- Subscribers are invoked **synchronously** -- long-running callbacks will block the publisher
- Consider keeping callbacks lightweight; defer heavy work to the next frame
- `SubscriptionID` values are monotonically increasing `uint64_t` values starting from 1
- The internal storage uses `std::unordered_map<std::type_index, vector<Subscription>>`

### Performance Considerations

- `Publish()` has O(N) cost where N is the number of subscribers for that type
- `Subscribe()` and `Unsubscribe()` acquire a mutex -- avoid calling in hot loops
- For high-frequency events (e.g., per-frame), consider direct function calls instead
- The `QueuedEventBus` adds one allocation per queued event (the type-erased lambda)

---

## See Also

- [Architecture Overview](Architecture-Overview) -- How subsystems communicate
- [Entity Component System](Entity-Component-System) -- Entity lifecycle events
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) -- Weather and time-of-day events
- [Physics](Physics) -- Collision events
- [Gameplay Systems](Gameplay-Systems) -- Gameplay events (damage, kills, quests)
- [Networking](Networking) -- Network event replication
- [Audio](Audio) -- Audio event triggers
