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

        QueuedEventBus(const QueuedEventBus&) = delete;
        QueuedEventBus& operator=(const QueuedEventBus&) = delete;
        QueuedEventBus(QueuedEventBus&&) = default;
        QueuedEventBus& operator=(QueuedEventBus&&) = default;

        /**
         * @brief Queue an event for deferred dispatch. Thread-safe.
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

        /** @brief Get the number of events currently waiting in the queue. */
        size_t GetPendingCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_pendingEvents.size();
        }

        /** @brief Discard all queued events without dispatching them. */
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
