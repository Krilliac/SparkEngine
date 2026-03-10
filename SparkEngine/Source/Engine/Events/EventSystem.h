/**
 * @file EventSystem.h
 * @brief Type-safe publish/subscribe event bus for decoupled system communication
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a lightweight, type-safe event bus that allows engine systems to
 * communicate without direct dependencies. Systems publish events and
 * subscribers receive them through registered callbacks.
 *
 * ## Usage
 * @code
 *   EventBus bus;
 *
 *   // Subscribe to an event
 *   auto id = bus.Subscribe<EntityDamagedEvent>([](const EntityDamagedEvent& e) {
 *       std::cout << "Entity took " << e.damage << " damage!\n";
 *   });
 *
 *   // Publish an event
 *   bus.Publish(EntityDamagedEvent{ entityId, 50.0f, DamageType::Fire });
 *
 *   // Unsubscribe when done
 *   bus.Unsubscribe<EntityDamagedEvent>(id);
 * @endcode
 *
 * ## Thread safety
 * The event bus uses a mutex for thread-safe subscribe/unsubscribe. Publishing
 * events is safe from any thread, but callbacks execute on the publishing thread.
 */

#pragma once

#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <typeindex>
#include <mutex>
#include <cstdint>
#include <algorithm>

namespace Spark
{

    // =============================================================================
    // Subscription handle
    // =============================================================================

    /** @brief Unique identifier for an event subscription, used for unsubscription. */
    using SubscriptionID = uint64_t;

    // =============================================================================
    // Event Bus
    // =============================================================================

    /**
 * @class EventBus
 * @brief Central publish/subscribe message broker for engine events.
 *
 * The EventBus uses C++ RTTI (std::type_index) to route events by type.
 * Each event type T can have multiple subscribers. Subscribers are invoked
 * synchronously in registration order when an event is published.
 */
    class EventBus
    {
      public:
        EventBus() = default;
        ~EventBus() = default;

        // Non-copyable, movable
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;
        EventBus(EventBus&&) = default;
        EventBus& operator=(EventBus&&) = default;

        /**
     * @brief Subscribe to events of type T.
     *
     * @tparam T      Event type to subscribe to.
     * @param callback  Function called when an event of type T is published.
     * @return          SubscriptionID that can be used to unsubscribe later.
     */
        template <typename T> SubscriptionID Subscribe(std::function<void(const T&)> callback)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            SubscriptionID id = m_nextId++;
            auto& subs = m_subscribers[std::type_index(typeid(T))];
            subs.push_back({id, [cb = std::move(callback)](const void* data) { cb(*static_cast<const T*>(data)); }});
            return id;
        }

        /**
     * @brief Unsubscribe a previously registered callback.
     *
     * @tparam T  Event type the subscription was for.
     * @param id  The SubscriptionID returned by Subscribe().
     * @return    true if the subscription was found and removed.
     */
        template <typename T> bool Unsubscribe(SubscriptionID id)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_subscribers.find(std::type_index(typeid(T)));
            if (it == m_subscribers.end())
                return false;

            auto& subs = it->second;
            auto sub = std::remove_if(subs.begin(), subs.end(), [id](const Subscription& s) { return s.id == id; });
            if (sub == subs.end())
                return false;
            subs.erase(sub, subs.end());
            return true;
        }

        /**
     * @brief Publish an event to all subscribers of type T.
     *
     * Callbacks are invoked synchronously in registration order on the
     * calling thread. The event object is passed by const reference.
     *
     * @tparam T     Event type to publish.
     * @param event  The event data to broadcast.
     */
        template <typename T> void Publish(const T& event)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_subscribers.find(std::type_index(typeid(T)));
            if (it == m_subscribers.end())
                return;

            // Copy subscriber list to allow safe iteration if callbacks modify subscriptions
            auto subs = it->second;
            for (const auto& sub : subs)
            {
                sub.callback(&event);
            }
        }

        /**
     * @brief Remove all subscriptions for a given event type.
     * @tparam T  Event type to clear.
     */
        template <typename T> void ClearSubscriptions()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_subscribers.erase(std::type_index(typeid(T)));
        }

        /**
     * @brief Remove all subscriptions for all event types.
     */
        void ClearAll()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_subscribers.clear();
        }

        /**
     * @brief Get the number of subscribers for a given event type.
     * @tparam T  Event type to query.
     * @return    Number of active subscriptions.
     */
        template <typename T> size_t GetSubscriberCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_subscribers.find(std::type_index(typeid(T)));
            if (it == m_subscribers.end())
                return 0;
            return it->second.size();
        }

      private:
        struct Subscription
        {
            SubscriptionID id;
            std::function<void(const void*)> callback;
        };

        mutable std::mutex m_mutex;
        std::unordered_map<std::type_index, std::vector<Subscription>> m_subscribers;
        SubscriptionID m_nextId = 1;
    };

    // =============================================================================
    // Built-in Event Types
    // =============================================================================

    /** @brief Fired when an entity takes damage. */
    struct EntityDamagedEvent
    {
        uint32_t entityId = 0;
        float damage = 0.0f;
        std::string damageSource;
    };

    /** @brief Fired when an entity is killed/destroyed. */
    struct EntityKilledEvent
    {
        uint32_t entityId = 0;
        uint32_t killerId = 0;
        std::string cause;
    };

    /** @brief Fired when an item is picked up by an entity. */
    struct ItemPickedUpEvent
    {
        uint32_t entityId = 0;
        uint32_t itemDefId = 0;
        int count = 1;
    };

    /** @brief Fired when a quest is completed. */
    struct QuestCompletedEvent
    {
        uint32_t entityId = 0;
        uint32_t questId = 0;
        std::string questName;
    };

    /** @brief Fired when the weather changes. */
    struct WeatherChangedEvent
    {
        int previousType = 0;
        int newType = 0;
        float intensity = 0.0f;
    };

    /** @brief Fired when the time of day changes significantly (e.g. dawn, dusk). */
    struct TimeOfDayChangedEvent
    {
        float previousHour = 0.0f;
        float currentHour = 0.0f;
        int dayCount = 0;
    };

    /** @brief Fired when a collision occurs between two entities. */
    struct CollisionEvent
    {
        uint32_t entityA = 0;
        uint32_t entityB = 0;
        float impactForce = 0.0f;
    };

    /** @brief Fired when a player respawns. */
    struct PlayerRespawnEvent
    {
        uint32_t entityId = 0;
        float spawnX = 0.0f;
        float spawnY = 0.0f;
        float spawnZ = 0.0f;
    };

    // =============================================================================
    // Engine Lifecycle Events
    // =============================================================================

    /** @brief Fired when the engine finishes initialization and is ready to run. */
    struct EngineStartEvent
    {
    };

    /** @brief Fired when the engine begins its shutdown sequence. */
    struct EngineShutdownEvent
    {
    };

    /** @brief Fired at the beginning of each frame before any systems update. */
    struct FrameBeginEvent
    {
        float deltaTime = 0.0f; ///< Time elapsed since the previous frame (seconds)
    };

    /** @brief Fired at the end of each frame after all systems have updated. */
    struct FrameEndEvent
    {
        float deltaTime = 0.0f; ///< Time elapsed during this frame (seconds)
    };

    // =============================================================================
    // Scene Events
    // =============================================================================

    /** @brief Fired when a scene has been fully loaded and is ready for gameplay. */
    struct SceneLoadedEvent
    {
        std::string sceneName; ///< Name or path of the loaded scene
    };

    /** @brief Fired when a scene has been unloaded and its resources released. */
    struct SceneUnloadedEvent
    {
        std::string sceneName; ///< Name or path of the unloaded scene
    };

    // =============================================================================
    // Entity Events
    // =============================================================================

    /** @brief Fired when a new entity is created in the ECS. */
    struct EntityCreatedEvent
    {
        uint32_t entityId = 0; ///< ID of the newly created entity
    };

    /** @brief Fired when an entity is destroyed and removed from the ECS. */
    struct EntityDestroyedEvent
    {
        uint32_t entityId = 0; ///< ID of the destroyed entity
    };

    /** @brief Fired when an entity's health reaches zero. */
    struct EntityDeathEvent
    {
        uint32_t entityId = 0; ///< ID of the dead entity
    };

    // =============================================================================
    // Physics Events
    // =============================================================================

    /** @brief Fired when an entity enters a trigger volume. */
    struct TriggerEnterEvent
    {
        uint32_t entityId = 0;  ///< ID of the entity that entered the trigger
        uint32_t triggerId = 0; ///< ID of the trigger volume entity
    };

    /** @brief Fired when an entity exits a trigger volume. */
    struct TriggerExitEvent
    {
        uint32_t entityId = 0;  ///< ID of the entity that exited the trigger
        uint32_t triggerId = 0; ///< ID of the trigger volume entity
    };

    // =============================================================================
    // Input Events
    // =============================================================================

    /** @brief Fired when a named input action is pressed or released. */
    struct InputActionEvent
    {
        std::string actionName; ///< Name of the action binding
        bool pressed = false;   ///< true if pressed, false if released
    };

    /** @brief Fired when the mouse moves. */
    struct MouseMoveEvent
    {
        float deltaX = 0.0f; ///< Horizontal movement in pixels since last frame
        float deltaY = 0.0f; ///< Vertical movement in pixels since last frame
    };

    // =============================================================================
    // Graphics Events
    // =============================================================================

    /** @brief Fired when the window is resized. */
    struct WindowResizeEvent
    {
        uint32_t width = 0;  ///< New window width in pixels
        uint32_t height = 0; ///< New window height in pixels
    };

    /** @brief Fired when the graphics quality preset changes. */
    struct QualityChangedEvent
    {
        std::string preset; ///< Name of the new quality preset (e.g., "Low", "Ultra")
    };

    // =============================================================================
    // Audio Events
    // =============================================================================

    /** @brief Fired when a sound begins playing. */
    struct SoundPlayedEvent
    {
        std::string soundName; ///< Name or path of the sound asset
    };

    // =============================================================================
    // Memory Events
    // =============================================================================

    /** @brief Fired when system memory usage exceeds a threshold. */
    struct MemoryPressureEvent
    {
        float usagePercent = 0.0f; ///< Current memory usage as a percentage [0, 100]
        size_t availableBytes = 0; ///< Remaining available memory in bytes
    };

    // =============================================================================
    // Queued Event Bus
    // =============================================================================

    /**
     * @class QueuedEventBus
     * @brief Thread-safe event queue that batches events for deferred dispatch.
     *
     * QueuedEventBus allows worker threads to safely publish events without
     * triggering callbacks immediately. Events are stored in a queue and
     * dispatched in bulk on the main thread during a specific engine phase
     * (e.g., between physics and rendering).
     *
     * ## Usage
     * @code
     *   QueuedEventBus queue;
     *
     *   // Worker thread: queue events safely
     *   queue.QueueEvent(CollisionEvent{ entityA, entityB, 42.0f });
     *
     *   // Main thread: dispatch all queued events through the EventBus
     *   queue.DispatchAll(mainEventBus);
     * @endcode
     *
     * ## Thread safety
     * QueueEvent() is safe to call from any thread. DispatchAll() should be
     * called from the main thread; it acquires the lock, swaps the queue, then
     * dispatches without holding the lock.
     */
    class QueuedEventBus
    {
      public:
        QueuedEventBus() = default;
        ~QueuedEventBus() = default;

        // Non-copyable, movable
        QueuedEventBus(const QueuedEventBus&) = delete;
        QueuedEventBus& operator=(const QueuedEventBus&) = delete;
        QueuedEventBus(QueuedEventBus&&) = default;
        QueuedEventBus& operator=(QueuedEventBus&&) = default;

        /**
         * @brief Queue an event for deferred dispatch. Thread-safe.
         *
         * The event is type-erased and stored internally. It will be published
         * to an EventBus when DispatchAll() is called.
         *
         * @tparam T     Event type.
         * @param event  Event data to queue.
         */
        template <typename T> void QueueEvent(T event)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingEvents.push_back([evt = std::move(event)](EventBus& bus) { bus.Publish(evt); });
        }

        /**
         * @brief Dispatch all queued events through the given EventBus.
         *
         * Swaps the internal queue under lock, then dispatches each event
         * without holding the lock. This minimises contention with producer
         * threads that may be calling QueueEvent() concurrently.
         *
         * @param bus  The EventBus to publish queued events through.
         */
        void DispatchAll(EventBus& bus)
        {
            std::vector<std::function<void(EventBus&)>> events;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                events.swap(m_pendingEvents);
            }

            for (const auto& dispatch : events)
            {
                dispatch(bus);
            }
        }

        /**
         * @brief Get the number of events currently waiting in the queue.
         * @return  Count of pending events.
         */
        size_t GetPendingCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_pendingEvents.size();
        }

        /**
         * @brief Discard all queued events without dispatching them.
         */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingEvents.clear();
        }

      private:
        mutable std::mutex m_mutex;
        std::vector<std::function<void(EventBus&)>> m_pendingEvents;
    };

} // namespace Spark
