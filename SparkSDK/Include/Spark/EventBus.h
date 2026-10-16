/**
 * @file EventBus.h
 * @brief Type-safe publish/subscribe event bus with RAII subscription handles
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a thread-safe, type-erased event bus where publishers and subscribers
 * are decoupled by event type. Subscribers receive a `SubscriptionHandle` that
 * automatically unsubscribes when destroyed (RAII). Multiple buses can exist
 * simultaneously; a default bus is available via `EventBus::Global()`.
 * Game modules must use the host-owned bus from IEngineContext::GetEventBus()
 * for host/module communication; a DLL's Global() need not be the host's bus.
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
 *   Spark::EventBus& bus = *context->GetEventBus();
 *   auto handle = bus.Subscribe<PlayerDiedEvent>(
 *       [](const PlayerDiedEvent& e) {
 *           RespawnPlayer(e.playerId);
 *       });
 *
 *   // Publish from anywhere
 *   bus.Publish<PlayerDiedEvent>({ .playerId = 7 });
 *
 *   // Explicit unsubscribe (or just let the handle go out of scope)
 *   handle.Unsubscribe();
 *
 *   // Per-instance bus (e.g. scoped to a single level)
 *   Spark::EventBus levelBus;
 *   auto h = levelBus.Subscribe<PlayerDiedEvent>([](const PlayerDiedEvent&){});
 * @endcode
 *
 * ## DLL lifetime
 * Host and module must use matching C++ ABI, standard library and runtime.
 * Stop publishers and join in-flight dispatch before module teardown. Release
 * module subscription handles and drain or clear queues while the image remains
 * loaded. Unsubscription does not destroy channels: the host must ClearAll()
 * before unloading an image that may have instantiated a channel, whose RTTI,
 * vtable and deleter can reside in that image. Modules must not ClearAll() on a
 * shared host bus themselves. A handle may outlive its bus, but must not outlive
 * the image containing its callback or unsubscribe function.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
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

        // Handles may outlive a bus (notably during static destruction).  The
        // token lets their callbacks become no-ops before the bus members are
        // torn down instead of dereferencing a destroyed EventBus.
        ~EventBus() { m_lifetime.reset(); }

        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;
        EventBus(EventBus&&) = delete;
        EventBus& operator=(EventBus&&) = delete;

        /**
         * @brief Access the default event bus.
         * @return  Reference to the singleton EventBus.
         * Use IEngineContext::GetEventBus() for communication across DLLs.
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

            std::weak_ptr<LifetimeToken> lifetime = m_lifetime;
            return SubscriptionHandle(
                [this, lifetime = std::move(lifetime)](uint64_t subId)
                {
                    if (lifetime.lock())
                        DoUnsubscribe<E>(subId);
                },
                id);
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
            ChannelOf<E>* ch = FindChannel<E>();
            if (!ch)
                return;

            // Guard against infinite recursion: if a handler re-publishes the same
            // event type on this bus and thread, the nested Publish is silently
            // dropped.  The guard lives on the bus, not in a thread_local: a module
            // image that defines a thread-local variable is never unloaded by dyld.
            const PublishingEntry self{std::this_thread::get_id(), ch};
            {
                std::lock_guard<std::mutex> lock(m_publishingMutex);
                if (std::find(m_publishing.begin(), m_publishing.end(), self) != m_publishing.end())
                    return;
                m_publishing.push_back(self);
            }
            const PublishingScope scope{*this, self};

            std::vector<std::function<void(const E&)>> snapshot;
            {
                std::lock_guard<std::mutex> lock(ch->mutex);
                snapshot.reserve(ch->entries.size());
                for (auto& entry : ch->entries)
                    snapshot.push_back(entry.handler);
            }

            for (auto& handler : snapshot)
            {
                try
                {
                    handler(event);
                }
                catch (const std::exception&)
                {
                }
                catch (...)
                {
                }
            }
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
        struct LifetimeToken
        {
        };

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

        // A (thread, channel) pair currently dispatching on this bus.
        using PublishingEntry = std::pair<std::thread::id, const IChannel*>;

        // Removes this Publish call's recursion-guard entry, even if dispatch throws.
        struct PublishingScope
        {
            EventBus& bus;
            PublishingEntry entry;

            ~PublishingScope()
            {
                std::lock_guard<std::mutex> lock(bus.m_publishingMutex);
                auto& active = bus.m_publishing;
                active.erase(std::find(active.begin(), active.end(), entry));
            }
        };

#if defined(__APPLE__) || defined(_LIBCPP_VERSION)
        // Internal-linkage types (anonymous namespaces, function-local classes) are
        // distinct even when their mangled names match, so they never match by name.
        static bool IsSharedTypeName(const char* name) noexcept
        {
            return name[0] != 'Z' && std::strstr(name, "_GLOBAL__N") == nullptr;
        }
#endif

        // Find the channel for E. The caller holds m_channelsMutex.
        template <typename E> IChannel* FindChannelLocked() const
        {
            const auto it = m_channels.find(std::type_index(typeid(E)));
            if (it != m_channels.end())
                return it->second.get();
#if defined(__APPLE__) || defined(_LIBCPP_VERSION)
            // libc++ compares type_info by address on ELF and Apple arm64 (unique RTTI),
            // and a hidden-visibility module carries its own copy, so the same SDK event
            // type misses across the module boundary. Fall back to the mangled name.
            // libstdc++ already compares names, so it never reaches this path.
            const char* name = typeid(E).name();
            if (IsSharedTypeName(name))
            {
                for (const auto& [key, channel] : m_channels)
                {
                    if (std::strcmp(key.name(), name) == 0)
                        return channel.get();
                }
            }
#endif
            return nullptr;
        }

        // Get or create the channel for E (acquires m_channelsMutex)
        template <typename E> ChannelOf<E>& GetOrCreateChannel()
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            if (IChannel* existing = FindChannelLocked<E>())
                return *static_cast<ChannelOf<E>*>(existing);
            auto ch = std::make_unique<ChannelOf<E>>();
            auto* raw = ch.get();
            m_channels.emplace(std::type_index(typeid(E)), std::move(ch));
            return *raw;
        }

        // Find the channel for E without creating it (acquires m_channelsMutex)
        template <typename E> ChannelOf<E>* FindChannel()
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            return static_cast<ChannelOf<E>*>(FindChannelLocked<E>());
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
        std::mutex m_publishingMutex;
        std::vector<PublishingEntry> m_publishing;
        std::atomic<uint64_t> m_nextId{0};
        std::shared_ptr<LifetimeToken> m_lifetime = std::make_shared<LifetimeToken>();
    };

} // namespace Spark
