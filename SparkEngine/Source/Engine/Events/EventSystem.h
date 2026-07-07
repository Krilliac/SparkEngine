/**
 * @file EventSystem.h
 * @brief Built-in event types and deferred event queue for engine communication
 * @author Spark Engine Team
 * @date 2025
 *
 * Re-exports the canonical EventBus from Utils/EventBus.h and defines the
 * engine's built-in event types (gameplay, lifecycle, physics, input, etc.)
 * plus a QueuedEventBus for thread-safe deferred dispatch.
 *
 * ## Usage
 * @code
 *   // Subscribe with RAII handle (auto-unsubscribes on destruction)
 *   auto handle = Spark::EventBus::Global().Subscribe<EntityDamagedEvent>(
 *       [](const EntityDamagedEvent& e) {
 *           std::cout << "Entity took " << e.damage << " damage!\n";
 *       });
 *
 *   // Publish from anywhere
 *   Spark::EventBus::Global().Publish(EntityDamagedEvent{ .entityId = 1, .damage = 50.0f });
 *
 *   // Deferred dispatch from worker threads
 *   QueuedEventBus queue;
 *   queue.QueueEvent(CollisionEvent{ entityA, entityB, 42.0f });
 *   queue.DispatchAll(mainEventBus);  // on main thread
 * @endcode
 *
 * ## Thread safety
 * The EventBus uses per-type mutexes for thread-safe subscribe/unsubscribe.
 * Publishing is safe from any thread. QueuedEventBus adds deferred dispatch.
 */

#pragma once

// Canonical EventBus implementation — single source of truth for the event bus class
#include "Utils/EventBus.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Spark
{

    // =============================================================================
    // Legacy SubscriptionID type alias
    // =============================================================================

    /**
     * @brief Legacy subscription ID type for code that uses manual unsubscription.
     *
     * New code should prefer SubscriptionHandle (RAII, auto-unsubscribes).
     * This alias is kept for backward compatibility with existing callers.
     */
    using SubscriptionID = uint64_t;

    // =============================================================================
    // Built-in Event Types — Gameplay
    // =============================================================================

    /** @brief Fired when an entity takes damage. */
    struct EntityDamagedEvent
    {
        uint32_t entityId = 0;    ///< Entity that took damage.
        float damage = 0.0f;      ///< Amount of damage dealt.
        std::string damageSource; ///< Source description (weapon name, hazard type, etc.).
    };

    /** @brief Fired when an entity is killed/destroyed. */
    struct EntityKilledEvent
    {
        uint32_t entityId = 0; ///< Entity that was killed.
        uint32_t killerId = 0; ///< Entity responsible for the kill (0 = environment).
        std::string cause;     ///< Cause of death (weapon name, "FallDamage", etc.).
    };

    /** @brief Fired when an item is picked up by an entity. */
    struct ItemPickedUpEvent
    {
        uint32_t entityId = 0;  ///< Entity that picked up the item.
        uint32_t itemDefId = 0; ///< Item definition ID from the item database.
        int count = 1;          ///< Number of items picked up (for stackable items).
    };

    /** @brief Fired when a quest is completed. */
    struct QuestCompletedEvent
    {
        uint32_t entityId = 0; ///< Entity that completed the quest.
        uint32_t questId = 0;  ///< Unique quest identifier.
        std::string questName; ///< Human-readable quest name.
    };

    /** @brief Fired when the weather changes. */
    struct WeatherChangedEvent
    {
        int previousType = 0;   ///< Previous weather type enum value.
        int newType = 0;        ///< New weather type enum value.
        float intensity = 0.0f; ///< Weather intensity [0, 1].
    };

    /** @brief Fired when the time of day changes significantly (e.g. dawn, dusk). */
    struct TimeOfDayChangedEvent
    {
        float previousHour = 0.0f; ///< Previous hour (0-24, fractional).
        float currentHour = 0.0f;  ///< New hour (0-24, fractional).
        int dayCount = 0;          ///< Number of in-game days elapsed.
    };

    /** @brief Fired when a collision occurs between two entities. */
    struct CollisionEvent
    {
        uint32_t entityA = 0;     ///< First entity involved in the collision.
        uint32_t entityB = 0;     ///< Second entity involved in the collision.
        float impactForce = 0.0f; ///< Magnitude of the collision impact (Newtons).
    };

    /** @brief Fired when a player respawns. */
    struct PlayerRespawnEvent
    {
        uint32_t entityId = 0; ///< Entity ID of the respawned player.
        float spawnX = 0.0f;   ///< Spawn position X coordinate.
        float spawnY = 0.0f;   ///< Spawn position Y coordinate.
        float spawnZ = 0.0f;   ///< Spawn position Z coordinate.
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
     * @brief Dispatch priority tier for queued events.
     *
     * Tiers determine two things: dispatch order (Critical → Normal → Low) and
     * eviction order under queue pressure (Low evicted first, Critical only
     * evicted as a last resort). Default tier is Normal, which matches the
     * pre-tier-aware behavior.
     */
    enum class EventPriority : uint8_t
    {
        Critical = 0, ///< Dispatched first; never evicted before Normal/Low
        Normal = 1,   ///< Default; dispatched after Critical
        Low = 2,      ///< Dispatched last; first to be evicted under pressure
        Count = 3
    };

    /**
     * @class QueuedEventBus
     * @brief Thread-safe event queue that batches events for deferred dispatch.
     *
     * QueuedEventBus allows worker threads to safely publish events without
     * triggering callbacks immediately. Events are stored in per-priority
     * queues and dispatched in bulk on the main thread during a specific
     * engine phase (e.g., between physics and rendering).
     *
     * ## Priority tiers
     * Each queued event carries an EventPriority (default Normal). DispatchAll
     * flushes Critical, then Normal, then Low. When the queue reaches
     * MaxQueueSize the oldest Low-tier event is evicted first; then oldest
     * Normal; Critical is only evicted if it alone fills the queue.
     *
     * ## Thread safety
     * QueueEvent() is safe to call from any thread. DispatchAll() should be
     * called from the main thread; it acquires the lock, swaps the queues,
     * then dispatches without holding the lock.
     */
    class QueuedEventBus
    {
      public:
        QueuedEventBus() = default;
        ~QueuedEventBus() = default;

        QueuedEventBus(const QueuedEventBus&) = delete;
        QueuedEventBus& operator=(const QueuedEventBus&) = delete;
        QueuedEventBus(QueuedEventBus&&) = delete;
        QueuedEventBus& operator=(QueuedEventBus&&) = delete;

        /**
         * @brief Queue an event for deferred dispatch at Normal priority. Thread-safe.
         * @tparam T     Event type.
         * @param event  Event data to queue.
         */
        template <typename T> void QueueEvent(T event) { QueueEvent(std::move(event), EventPriority::Normal); }

        /**
         * @brief Queue an event for deferred dispatch at a specific priority. Thread-safe.
         * @tparam T       Event type.
         * @param event    Event data to queue.
         * @param priority Tier controlling dispatch order and eviction policy.
         */
        template <typename T> void QueueEvent(T event, EventPriority priority)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (TotalPendingLocked() >= MaxQueueSize)
            {
                EvictOldestLocked();
            }

            QueueFor(priority).push_back([evt = std::move(event)](EventBus& bus) { bus.Publish(evt); });
        }

        /**
         * @brief Dispatch all queued events through the given EventBus.
         *
         * Events dispatch in priority order: Critical first, then Normal, then
         * Low. Queues are swapped out under the lock, so dispatch runs without
         * holding it and subscribers may themselves enqueue new events safely.
         *
         * @param bus  The EventBus to publish queued events through.
         */
        void DispatchAll(EventBus& bus)
        {
            QueueType critical;
            QueueType normal;
            QueueType low;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                critical.swap(m_critical);
                normal.swap(m_normal);
                low.swap(m_low);
                m_droppedEventCount = 0;
            }

            for (const auto& dispatch : critical)
                dispatch(bus);
            for (const auto& dispatch : normal)
                dispatch(bus);
            for (const auto& dispatch : low)
                dispatch(bus);
        }

        /** @brief Total number of events currently waiting across all priority tiers. */
        size_t GetPendingCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return TotalPendingLocked();
        }

        /** @brief Number of events currently waiting in a specific priority tier. */
        size_t GetPendingCount(EventPriority priority) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return QueueFor(priority).size();
        }

        /** @brief Discard all queued events without dispatching them. */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_critical.clear();
            m_normal.clear();
            m_low.clear();
        }

        /** @brief Number of events dropped due to queue overflow since last DispatchAll. */
        size_t GetDroppedEventCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_droppedEventCount;
        }

      private:
        static constexpr size_t MaxQueueSize = 10000;

        // std::deque (not std::vector): EvictOldestLocked() pops from the front on
        // every enqueue once the queue saturates. deque::pop_front is O(1), whereas
        // vector::erase(begin()) is O(n) and becomes O(n^2) under sustained overflow
        // while holding m_mutex. DispatchAll's swap-and-drain pattern is unaffected.
        using QueueType = std::deque<std::function<void(EventBus&)>>;

        QueueType& QueueFor(EventPriority priority)
        {
            switch (priority)
            {
            case EventPriority::Critical:
                return m_critical;
            case EventPriority::Low:
                return m_low;
            case EventPriority::Normal:
            default:
                return m_normal;
            }
        }

        const QueueType& QueueFor(EventPriority priority) const
        {
            switch (priority)
            {
            case EventPriority::Critical:
                return m_critical;
            case EventPriority::Low:
                return m_low;
            case EventPriority::Normal:
            default:
                return m_normal;
            }
        }

        size_t TotalPendingLocked() const { return m_critical.size() + m_normal.size() + m_low.size(); }

        // Evict the oldest event from the lowest-priority non-empty queue.
        // Critical is only touched when it alone fills the queue — which is
        // effectively a safety valve against critical-event floods.
        void EvictOldestLocked()
        {
            if (!m_low.empty())
            {
                m_low.pop_front();
            }
            else if (!m_normal.empty())
            {
                m_normal.pop_front();
            }
            else if (!m_critical.empty())
            {
                m_critical.pop_front();
            }
            else
            {
                return; // nothing to evict — should not happen when total >= MaxQueueSize
            }
            ++m_droppedEventCount;
        }

        mutable std::mutex m_mutex;
        QueueType m_critical;
        QueueType m_normal;
        QueueType m_low;
        size_t m_droppedEventCount = 0;
    };

} // namespace Spark
