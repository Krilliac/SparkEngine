# SparkEngine Event System — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Events/` (EventBus, built-in event types)
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Events/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Event subsystem provides a type-safe publish/subscribe event bus using C++ RTTI (`std::type_index`) for event routing. The `EventBus` class supports `Subscribe<T>()`, `Unsubscribe<T>()`, `Publish<T>()`, and `ClearSubscriptions<T>()`. Eight built-in event types are defined (`EntityDamagedEvent`, `EntityKilledEvent`, `ItemPickedUpEvent`, `QuestCompletedEvent`, `WeatherChangedEvent`, `TimeOfDayChangedEvent`, `CollisionEvent`, `PlayerRespawnEvent`). The EventBus is registered in `EngineContext`. However, there are critical thread-safety issues, no evidence of actual subsystem integration, and the system lacks queued dispatch.

---

## Critical Gaps

### GAP-EV01 — Potential Deadlock in Publish()

**Files**:
- `Events/EventSystem.h` (lines 124–137, `Publish<T>()`)

**Impact**: `Publish()` acquires `m_mutex` (non-recursive `std::mutex`) and then invokes subscriber callbacks while still holding the lock. If any callback calls `Subscribe()`, `Unsubscribe()`, or `Publish()` on the **same** `EventBus` instance, the thread will attempt to re-acquire the same non-recursive mutex, causing a **deadlock**. This is a realistic scenario: an `EntityKilledEvent` handler might publish a `PlayerRespawnEvent`, or an event handler might unsubscribe itself.

**Evidence**: Line 126: `std::lock_guard<std::mutex> lock(m_mutex)` — acquired before callback invocation. Line 133: `sub.callback(&event)` — executed while lock is held. `m_mutex` is `std::mutex` (line 179), not `std::recursive_mutex`. The subscriber list is copied at line 132 (`auto subs = it->second`), which prevents iterator invalidation but does **not** prevent mutex re-acquisition deadlock.

**What is needed**: Either use `std::recursive_mutex` (simple fix but masks design issues), or release the lock before invoking callbacks (copy the subscriber list, unlock, then iterate). The preferred fix is to copy-and-unlock:

```cpp
template <typename T> void Publish(const T& event) {
    std::vector<Subscription> subs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(std::type_index(typeid(T)));
        if (it == m_subscribers.end()) return;
        subs = it->second;
    }
    for (const auto& sub : subs)
        sub.callback(&event);
}
```

---

## Major Gaps

### GAP-EV02 — No Subsystem Integration (Dead Event Types)

**Files**:
- `Events/EventSystem.h` (lines 188–253, built-in event types)
- All other engine subsystems

**Impact**: Eight built-in event types are defined but there is no evidence that any engine subsystem publishes or subscribes to them. `CollisionEvent` should be published by the physics system, `WeatherChangedEvent` by the weather system, `TimeOfDayChangedEvent` by `DayNightCycle`, and `EntityDamagedEvent`/`EntityKilledEvent` by the health/combat system. Without actual publishers and subscribers, the event system is infrastructure without content.

**Evidence**: Grep for `Publish<EntityDamagedEvent>`, `Publish<CollisionEvent>`, `Subscribe<WeatherChangedEvent>`, etc. across the codebase shows no usage outside the `EventSystem.h` definitions. `DayNightCycle.h` includes `EventSystem.h` but it is unclear if it publishes `TimeOfDayChangedEvent`.

**What is needed**: Wire up event publishing in the relevant subsystems. At minimum: `PhysicsSystem` should publish `CollisionEvent`, `DayNightCycle` should publish `TimeOfDayChangedEvent`, the health component system should publish `EntityDamagedEvent`/`EntityKilledEvent`.

---

### GAP-EV03 — No Deferred/Queued Event Dispatch

**Files**:
- `Events/EventSystem.h` (lines 124–137, `Publish<T>()`)

**Impact**: All events are dispatched synchronously on the calling thread. There is no option to queue events for processing at a specific point in the frame (e.g., after physics but before rendering). This creates ordering dependencies: if system A publishes an event during its update, and system B subscribes to it, the callback runs during system A's update rather than during system B's update.

**Evidence**: `Publish()` immediately invokes all callbacks. No `QueueEvent()`, `FlushEvents()`, or `ProcessDeferredEvents()` method exists.

**What is needed**: Add a deferred dispatch mode: `Queue<T>(event)` stores events in a type-erased queue, and `FlushQueue()` dispatches all queued events in order. Allow both immediate and deferred dispatch. This is especially important for events that trigger entity creation/destruction, which should not happen mid-system-update.

---

### GAP-EV04 — RTTI Dependency

**Files**:
- `Events/EventSystem.h` (lines 38, 88, 103, 127, 146, 164 — `std::type_index`, `typeid`)

**Impact**: The EventBus relies on C++ RTTI (`std::type_index(typeid(T))`) for event type routing. If RTTI is disabled (`-fno-rtti` on GCC/Clang), the entire event system fails to compile. Some game projects disable RTTI for performance or binary size reasons.

**Evidence**: Every public template method uses `std::type_index(typeid(T))` as the map key.

**What is needed**: Provide a compile-time type ID alternative (e.g., `template<typename T> constexpr size_t TypeID()` using `__COUNTER__` or a manual registration macro) that works when RTTI is disabled. Keep RTTI as the default but allow opt-out.

---

## Moderate Gaps

### GAP-EV05 — No Event Priority or Ordering Control

**Files**:
- `Events/EventSystem.h` (lines 84–91, `Subscribe<T>()`)

**Impact**: Subscribers are invoked in registration order with no way to control priority. If two systems both subscribe to `EntityKilledEvent`, there is no guarantee which runs first. Systems that need to run before others (e.g., score tracking before respawn) cannot express ordering requirements.

**What is needed**: Add an optional `priority` parameter to `Subscribe()` (default 0). Sort subscribers by priority (highest first) when dispatching. This is a common pattern in event systems.

---

### GAP-EV06 — No Event Filtering

**Files**:
- `Events/EventSystem.h` (full `Subscribe<T>()` template)

**Impact**: Subscribers receive all events of a given type. There is no way to filter by event content (e.g., "only `EntityDamagedEvent` where `entityId == myId`"). Every subscriber must check event fields and discard irrelevant events, adding overhead when many entities subscribe to the same event type.

**What is needed**: Add a filtered subscribe overload: `Subscribe<T>(predicate, callback)` where `predicate` is `std::function<bool(const T&)>`. The predicate is evaluated before the callback is invoked.

---

### GAP-EV07 — Subscriber List Copy on Every Publish

**Files**:
- `Events/EventSystem.h` (line 132, `auto subs = it->second`)

**Impact**: Every `Publish()` call copies the entire subscriber vector for the event type. This is done to allow safe iteration if a callback modifies the subscriber list, which is a reasonable safety measure. However, for frequently published events (e.g., `CollisionEvent` fired every frame for every contact), the vector copy adds per-frame allocation overhead.

**What is needed**: Use a "dirty flag" pattern — only copy when a modification has occurred since the last publish. Alternatively, use a stable subscriber list with tombstones for removed entries, avoiding copies entirely.

---

## Minor Gaps

### GAP-EV08 — No Event History or Replay for Debugging

**Files**: All event files

**Impact**: No facility to record published events for debugging or replay. When debugging gameplay issues, knowing what events were fired and in what order is critical. The console integration does not include event logging.

**What is needed**: Add an optional event history ring buffer that stores the last N events (type name, timestamp, serialized data). Expose via console command (e.g., `event history 20`).

---

### GAP-EV09 — No Wildcard or Catch-All Subscription

**Files**:
- `Events/EventSystem.h` (full file)

**Impact**: There is no way to subscribe to all event types at once. A debugging or logging system that wants to observe all engine events must subscribe to each type individually.

**What is needed**: Add a `SubscribeAll(std::function<void(const std::type_index&, const void*)>)` method for type-erased catch-all subscriptions.

---

### GAP-EV10 — No Unsubscribe-by-Type Without Knowing SubscriptionID

**Files**:
- `Events/EventSystem.h` (lines 100–113, `Unsubscribe<T>(id)`)

**Impact**: `Unsubscribe` requires both the event type `T` and the `SubscriptionID`. If a subscriber loses its ID (e.g., a system that subscribes during initialization but doesn't store the ID), it cannot unsubscribe. `ClearSubscriptions<T>()` removes *all* subscribers for a type, which is too broad.

**What is needed**: Add `UnsubscribeAll(SubscriptionID id)` that searches all event types for the given ID. Alternatively, return an RAII handle from `Subscribe()` that auto-unsubscribes on destruction.

---
