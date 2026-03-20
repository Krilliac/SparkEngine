/**
 * @file EntityEventBus.h
 * @brief Per-entity event dispatch (O3DE ById-inspired)
 *
 * Extends the global EventBus pattern with entity-scoped event addressing.
 * Instead of broadcasting to all subscribers, events can be published to a
 * specific entity ID and only handlers registered for that entity will fire.
 *
 * This avoids the O(N) cost of every handler checking "is this my entity?"
 * on every global event — a major performance win for large entity counts.
 *
 * ## Usage
 * @code
 *   EntityEventBus bus;
 *
 *   // Subscribe to events for a specific entity
 *   auto handle = bus.Subscribe<DamagedEvent>(entityId,
 *       [](const DamagedEvent& e) { ApplyDamage(e.amount); });
 *
 *   // Publish to a single entity (only its handlers fire)
 *   bus.Publish<DamagedEvent>(entityId, { .amount = 50.0f });
 *
 *   // Broadcast to ALL entities that have handlers for this event type
 *   bus.Broadcast<DamagedEvent>({ .amount = 10.0f });
 * @endcode
 *
 * @see EventBus for the global (non-entity-scoped) event bus
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /// @brief Entity identifier for event addressing (matches EnTT entity or network ID)
    using EventEntityID = uint64_t;

    // Forward declaration
    class EntityEventBus;

    /**
     * @brief RAII handle for entity-scoped subscriptions.
     *
     * Auto-unsubscribes when destroyed. Move-only.
     */
    class EntitySubscriptionHandle
    {
      public:
        EntitySubscriptionHandle() = default;

        EntitySubscriptionHandle(const EntitySubscriptionHandle&) = delete;
        EntitySubscriptionHandle& operator=(const EntitySubscriptionHandle&) = delete;

        EntitySubscriptionHandle(EntitySubscriptionHandle&& other) noexcept
            : m_unsubscribeFn(std::move(other.m_unsubscribeFn)), m_id(other.m_id)
        {
            other.m_id = 0;
        }

        EntitySubscriptionHandle& operator=(EntitySubscriptionHandle&& other) noexcept
        {
            if (this != &other)
            {
                Unsubscribe();
                m_unsubscribeFn = std::move(other.m_unsubscribeFn);
                m_id = other.m_id;
                other.m_id = 0;
            }
            return *this;
        }

        ~EntitySubscriptionHandle() { Unsubscribe(); }

        void Unsubscribe()
        {
            if (m_unsubscribeFn && m_id != 0)
            {
                m_unsubscribeFn(m_id);
                m_unsubscribeFn = nullptr;
                m_id = 0;
            }
        }

        [[nodiscard]] bool IsActive() const noexcept { return m_id != 0 && m_unsubscribeFn != nullptr; }

      private:
        friend class EntityEventBus;

        using UnsubscribeFn = std::function<void(uint64_t)>;

        EntitySubscriptionHandle(UnsubscribeFn fn, uint64_t id) : m_unsubscribeFn(std::move(fn)), m_id(id) {}

        UnsubscribeFn m_unsubscribeFn;
        uint64_t m_id = 0;
    };

    /**
     * @brief Per-entity event bus with O3DE-style ById addressing.
     *
     * Each event type × entity ID pair has its own handler list. Publishing
     * to a specific entity only invokes handlers registered for that entity,
     * avoiding the broadcast-to-all pattern.
     *
     * Thread safety: Per-channel mutexes protect handler lists. Safe to subscribe
     * and publish from different threads.
     */
    class EntityEventBus
    {
      public:
        EntityEventBus() = default;

        EntityEventBus(const EntityEventBus&) = delete;
        EntityEventBus& operator=(const EntityEventBus&) = delete;

        /// @brief Access the engine-wide entity event bus singleton.
        static EntityEventBus& Global()
        {
            static EntityEventBus instance;
            return instance;
        }

        /**
         * @brief Subscribe a handler to events of type E for a specific entity.
         *
         * @tparam E       Event type
         * @param entityId Target entity
         * @param handler  Callable invoked with const E& when the entity receives this event
         * @return EntitySubscriptionHandle owning the subscription lifetime
         */
        template <typename E>
        [[nodiscard]] EntitySubscriptionHandle Subscribe(EventEntityID entityId, std::function<void(const E&)> handler)
        {
            auto& ch = GetOrCreateChannel<E>();
            std::lock_guard<std::mutex> lock(ch.mutex);

            uint64_t id = ++m_nextId;
            ch.entityHandlers[entityId].push_back({id, std::move(handler)});

            return EntitySubscriptionHandle([this, entityId](uint64_t subId) { DoUnsubscribe<E>(entityId, subId); },
                                            id);
        }

        /**
         * @brief Publish an event to a specific entity.
         *
         * Only handlers registered for this entity + event type combination will fire.
         *
         * @tparam E       Event type
         * @param entityId Target entity
         * @param event    The event value
         */
        template <typename E> void Publish(EventEntityID entityId, const E& event)
        {
            auto* ch = FindChannel<E>();
            if (!ch)
                return;

            // Snapshot handlers for this entity under lock
            std::vector<std::function<void(const E&)>> snapshot;
            {
                std::lock_guard<std::mutex> lock(ch->mutex);
                auto it = ch->entityHandlers.find(entityId);
                if (it == ch->entityHandlers.end())
                    return;
                snapshot.reserve(it->second.size());
                for (auto& entry : it->second)
                    snapshot.push_back(entry.handler);
            }

            for (auto& handler : snapshot)
                handler(event);
        }

        /**
         * @brief Broadcast an event to ALL entities that have handlers for this type.
         *
         * Useful for events that affect many entities (e.g. area damage, time-of-day change).
         *
         * @tparam E     Event type
         * @param event  The event value
         */
        template <typename E> void Broadcast(const E& event)
        {
            auto* ch = FindChannel<E>();
            if (!ch)
                return;

            // Snapshot all handlers across all entities
            std::vector<std::function<void(const E&)>> snapshot;
            {
                std::lock_guard<std::mutex> lock(ch->mutex);
                for (auto& [entityId, entries] : ch->entityHandlers)
                {
                    for (auto& entry : entries)
                        snapshot.push_back(entry.handler);
                }
            }

            for (auto& handler : snapshot)
                handler(event);
        }

        /**
         * @brief Remove all subscriptions for a specific entity across all event types.
         *
         * Call when an entity is destroyed to clean up all its handlers.
         *
         * @param entityId Entity being destroyed
         */
        void RemoveEntity(EventEntityID entityId)
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            for (auto& [typeIdx, channel] : m_channels)
            {
                channel->RemoveEntity(entityId);
            }
        }

        /// @brief Remove all subscriptions for all entities and all event types.
        void ClearAll()
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            m_channels.clear();
        }

        /// @brief Get the number of entities with handlers for event type E.
        template <typename E> [[nodiscard]] size_t EntityCount()
        {
            auto* ch = FindChannel<E>();
            if (!ch)
                return 0;
            std::lock_guard<std::mutex> lock(ch->mutex);
            return ch->entityHandlers.size();
        }

        /// @brief Get total handler count for a specific entity and event type.
        template <typename E> [[nodiscard]] size_t HandlerCount(EventEntityID entityId)
        {
            auto* ch = FindChannel<E>();
            if (!ch)
                return 0;
            std::lock_guard<std::mutex> lock(ch->mutex);
            auto it = ch->entityHandlers.find(entityId);
            if (it == ch->entityHandlers.end())
                return 0;
            return it->second.size();
        }

      private:
        // Type-erased base for per-event-type channels
        struct IChannel
        {
            virtual ~IChannel() = default;
            virtual void RemoveEntity(EventEntityID entityId) = 0;
        };

        // Per-event-type channel with per-entity handler lists
        template <typename E> struct ChannelOf : IChannel
        {
            struct Entry
            {
                uint64_t id;
                std::function<void(const E&)> handler;
            };

            mutable std::mutex mutex;
            std::unordered_map<EventEntityID, std::vector<Entry>> entityHandlers;

            void RemoveEntity(EventEntityID entityId) override
            {
                std::lock_guard<std::mutex> lock(mutex);
                entityHandlers.erase(entityId);
            }
        };

        template <typename E> ChannelOf<E>& GetOrCreateChannel()
        {
            std::type_index key = typeid(E);
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            auto it = m_channels.find(key);
            if (it == m_channels.end())
            {
                auto ch = std::make_unique<ChannelOf<E>>();
                auto* raw = ch.get();
                m_channels.emplace(key, std::move(ch));
                return *raw;
            }
            return *static_cast<ChannelOf<E>*>(it->second.get());
        }

        template <typename E> ChannelOf<E>* FindChannel()
        {
            std::type_index key = typeid(E);
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            auto it = m_channels.find(key);
            if (it == m_channels.end())
                return nullptr;
            return static_cast<ChannelOf<E>*>(it->second.get());
        }

        template <typename E> void DoUnsubscribe(EventEntityID entityId, uint64_t subId)
        {
            auto* ch = FindChannel<E>();
            if (!ch)
                return;
            std::lock_guard<std::mutex> lock(ch->mutex);
            auto it = ch->entityHandlers.find(entityId);
            if (it == ch->entityHandlers.end())
                return;
            auto& entries = it->second;
            entries.erase(
                std::remove_if(entries.begin(), entries.end(), [subId](const auto& e) { return e.id == subId; }),
                entries.end());
            if (entries.empty())
                ch->entityHandlers.erase(it);
        }

        mutable std::mutex m_channelsMutex;
        std::unordered_map<std::type_index, std::unique_ptr<IChannel>> m_channels;
        std::atomic<uint64_t> m_nextId{0};
    };

} // namespace Spark
