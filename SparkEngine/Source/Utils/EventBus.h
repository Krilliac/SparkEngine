/**
 * @file EventBus.h
 * @brief Type-safe publish/subscribe event bus with RAII subscription handles
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a thread-safe, type-erased event bus where publishers and subscribers
 * are decoupled by event type. Subscribers receive a `SubscriptionHandle` that
 * automatically unsubscribes when destroyed (RAII). Multiple buses can exist
 * simultaneously; a default global bus is available via `EventBus::Global()`.
 *
 * ## Design
 * - Each event type has its own handler list, so publishing one event type does
 *   not touch unrelated handler lists.
 * - Handlers are called synchronously on the thread that calls `Publish<E>()`.
 * - `SubscriptionHandle` is move-only and auto-unsubscribes on destruction.
 * - Thread safety: subscribing, unsubscribing, and publishing are all guarded by
 *   per-type mutexes, so it is safe to publish from one thread while subscribing
 *   from another. Do not publish recursively from within a handler.
 *
 * ## Usage
 * @code
 *   // Define an event type (any struct or class)
 *   struct PlayerDiedEvent { int playerId; };
 *
 *   // Subscribe (handle auto-unsubscribes when destroyed)
 *   auto handle = Spark::EventBus::Global().Subscribe<PlayerDiedEvent>(
 *       [](const PlayerDiedEvent& e) {
 *           RespawnPlayer(e.playerId);
 *       });
 *
 *   // Publish from anywhere
 *   Spark::EventBus::Global().Publish<PlayerDiedEvent>({ .playerId = 7 });
 *
 *   // Explicit unsubscribe (or just let the handle go out of scope)
 *   handle.Unsubscribe();
 *
 *   // Per-instance bus (e.g. scoped to a single level)
 *   Spark::EventBus levelBus;
 *   auto h = levelBus.Subscribe<PlayerDiedEvent>([](const PlayerDiedEvent&){});
 * @endcode
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

    // Forward declaration
    class EventBus;

    // =========================================================================
    // SubscriptionHandle
    // =========================================================================

    /**
     * @class SubscriptionHandle
     * @brief RAII handle that automatically unsubscribes from the event bus on destruction.
     *
     * Move-only: copying would create ambiguity over which copy owns the subscription.
     * Calling `Unsubscribe()` explicitly or allowing destruction both cleanly remove
     * the handler from the bus.
     */
    class SubscriptionHandle
    {
      public:
        /// Default-constructed handle is inactive.
        SubscriptionHandle() = default;

        SubscriptionHandle(const SubscriptionHandle&) = delete;
        SubscriptionHandle& operator=(const SubscriptionHandle&) = delete;

        SubscriptionHandle(SubscriptionHandle&& other) noexcept
            : m_unsubscribeFn(std::move(other.m_unsubscribeFn)), m_id(other.m_id)
        {
            other.m_id = 0;
        }

        SubscriptionHandle& operator=(SubscriptionHandle&& other) noexcept
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

        /// Destroy the handle, automatically unsubscribing if still active.
        ~SubscriptionHandle() { Unsubscribe(); }

        /**
         * @brief Manually unsubscribe from the event bus.
         * Safe to call multiple times — subsequent calls are no-ops.
         */
        void Unsubscribe()
        {
            if (m_unsubscribeFn && m_id != 0)
            {
                m_unsubscribeFn(m_id);
                m_unsubscribeFn = nullptr;
                m_id = 0;
            }
        }

        /**
         * @brief Return whether this handle is currently subscribed.
         * @return  true if the subscription is active.
         */
        [[nodiscard]] bool IsActive() const noexcept { return m_id != 0 && m_unsubscribeFn != nullptr; }

      private:
        friend class EventBus;

        using UnsubscribeFn = std::function<void(uint64_t)>;

        SubscriptionHandle(UnsubscribeFn fn, uint64_t id) : m_unsubscribeFn(std::move(fn)), m_id(id) {}

        UnsubscribeFn m_unsubscribeFn;
        uint64_t m_id = 0;
    };

    // =========================================================================
    // EventBus
    // =========================================================================

    /**
     * @class EventBus
     * @brief Type-safe publish/subscribe event bus.
     *
     * Subscribers register handlers for specific event types. Publishers fire an
     * event by calling `Publish<EventType>(event)`. All handlers registered for
     * that event type are invoked synchronously in registration order.
     */
    class EventBus
    {
      public:
        EventBus() = default;

        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;
        EventBus(EventBus&&) = delete;
        EventBus& operator=(EventBus&&) = delete;

        /**
         * @brief Access the engine-wide global event bus.
         * @return  Reference to the singleton EventBus.
         */
        static EventBus& Global()
        {
            static EventBus instance;
            return instance;
        }

        /**
         * @brief Subscribe a handler to events of type `E`.
         *
         * @tparam E      Event type. Any struct or class.
         * @param handler Callable invoked with `const E&` on each publish.
         * @return        `SubscriptionHandle` owning the subscription lifetime.
         */
        template <typename E> [[nodiscard]] SubscriptionHandle Subscribe(std::function<void(const E&)> handler)
        {
            ChannelOf<E>& ch = GetOrCreateChannel<E>();
            std::lock_guard<std::mutex> lock(ch.mutex);

            uint64_t id = ++m_nextId;
            ch.entries.push_back({id, std::move(handler)});

            return SubscriptionHandle([this](uint64_t subId) { DoUnsubscribe<E>(subId); }, id);
        }

        /**
         * @brief Publish an event, invoking all registered handlers synchronously.
         *
         * Handlers are called in registration order. Do not subscribe or unsubscribe
         * from within a handler (may deadlock).
         *
         * @tparam E     Event type.
         * @param event  The event value passed as `const E&` to all handlers.
         */
        template <typename E> void Publish(const E& event)
        {
            // Guard against infinite recursion (handler publishes same event type)
            static thread_local int s_publishDepth = 0;
            if (s_publishDepth > 0)
                return; // Silently ignore recursive publish for the same event type

            ChannelOf<E>* ch = FindChannel<E>();
            if (!ch)
                return;

            // Snapshot handler list under lock so we can call outside the lock.
            std::vector<std::function<void(const E&)>> snapshot;
            {
                std::lock_guard<std::mutex> lock(ch->mutex);
                snapshot.reserve(ch->entries.size());
                for (auto& entry : ch->entries)
                    snapshot.push_back(entry.handler);
            }

            ++s_publishDepth;
            for (auto& handler : snapshot)
            {
                try
                {
                    handler(event);
                }
                catch (const std::exception&)
                {
                    // Fault isolation: skip faulted handler, continue dispatching
                }
                catch (...)
                {
                    // Fault isolation: skip faulted handler, continue dispatching
                }
            }
            --s_publishDepth;
        }

        /**
         * @brief Remove all subscriptions for event type `E`.
         *
         * Existing `SubscriptionHandle`s for `E` become inactive but are still
         * safe to destroy.
         *
         * @tparam E  Event type to clear.
         */
        template <typename E> void ClearSubscribers()
        {
            ChannelOf<E>* ch = FindChannel<E>();
            if (!ch)
                return;
            std::lock_guard<std::mutex> lock(ch->mutex);
            ch->entries.clear();
        }

        /**
         * @brief Remove all subscriptions for every event type.
         */
        void ClearAll()
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            m_channels.clear();
        }

        /**
         * @brief Return the number of active subscribers for event type `E`.
         * @tparam E  Event type.
         * @return    Subscriber count.
         */
        template <typename E> [[nodiscard]] size_t SubscriberCount()
        {
            ChannelOf<E>* ch = FindChannel<E>();
            if (!ch)
                return 0;
            std::lock_guard<std::mutex> lock(ch->mutex);
            return ch->entries.size();
        }

      private:
        // Type-erased base so we can store channels in a single map
        struct IChannel
        {
            virtual ~IChannel() = default;
        };

        // Per-event-type channel (one per E)
        template <typename E> struct ChannelOf : IChannel
        {
            struct Entry
            {
                uint64_t id;
                std::function<void(const E&)> handler;
            };

            mutable std::mutex mutex;
            std::vector<Entry> entries;
        };

        // Get or create the channel for E (acquires m_channelsMutex)
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

        // Find the channel for E without creating it (acquires m_channelsMutex)
        template <typename E> ChannelOf<E>* FindChannel()
        {
            std::type_index key = typeid(E);
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            auto it = m_channels.find(key);
            if (it == m_channels.end())
                return nullptr;
            return static_cast<ChannelOf<E>*>(it->second.get());
        }

        // Remove a specific subscription by ID
        template <typename E> void DoUnsubscribe(uint64_t id)
        {
            ChannelOf<E>* ch = FindChannel<E>();
            if (!ch)
                return;
            std::lock_guard<std::mutex> lock(ch->mutex);
            auto& entries = ch->entries;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [id](const typename ChannelOf<E>::Entry& e) { return e.id == id; }),
                          entries.end());
        }

        mutable std::mutex m_channelsMutex;
        std::unordered_map<std::type_index, std::unique_ptr<IChannel>> m_channels;
        std::atomic<uint64_t> m_nextId{0};
    };

} // namespace Spark
