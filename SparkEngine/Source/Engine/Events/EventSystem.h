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

} // namespace Spark
