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
#include <string_view>
#include <thread>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Spark
{

    // Forward declaration
    class EventBus;

    namespace Detail
    {
        /**
         * @brief Recognises the Itanium-mangled name (typeid(T).name()) of an external-linkage event type.
         *
         * Accepts class names, nested names, std substitutions and template arguments made of those,
         * builtins, cv/pointer/reference types and integral literals. Everything else is refused:
         * function-local types (Z), anonymous namespaces (_GLOBAL__N), unnamed and lambda types (Ut, Ul),
         * clang's $_N types, expressions. Refusing a shared type only loses the cross-module name match;
         * accepting an internal one would alias two distinct event types. Pure and allocation-free.
         */
        class SharedTypeNameParser
        {
          public:
            static bool IsShared(const char* name) noexcept
            {
                SharedTypeNameParser parser{name};
                return parser.Type() && *parser.m_cursor == '\0';
            }

          private:
            explicit SharedTypeNameParser(const char* name) noexcept : m_cursor(name) {}

            static bool IsDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }
            bool At(const char* set) const noexcept { return *m_cursor != '\0' && std::strchr(set, *m_cursor); }

            // <length><identifier>
            bool SourceName() noexcept
            {
                if (*m_cursor < '1' || *m_cursor > '9')
                {
                    return false;
                }
                size_t length = 0;
                while (IsDigit(*m_cursor))
                {
                    length = length * 10 + static_cast<size_t>(*m_cursor - '0');
                    ++m_cursor;
                    if (length > 4096)
                    {
                        return false;
                    }
                }
                if (std::memchr(m_cursor, '\0', length) != nullptr)
                {
                    return false;
                }
                const std::string_view identifier(m_cursor, length);
                if (identifier.starts_with("_GLOBAL__N") || identifier.find('$') != std::string_view::npos)
                {
                    return false;
                }
                m_cursor += length;
                return true;
            }

            // One name component (or std substitution), with optional template arguments.
            bool Component() noexcept
            {
                if (*m_cursor == 'S')
                {
                    ++m_cursor;
                    if (*m_cursor == 't')
                    {
                        ++m_cursor;
                        if (!SourceName())
                        {
                            return false;
                        }
                    }
                    else if (At("absiod"))
                    {
                        ++m_cursor;
                    }
                    else
                    {
                        // S_ or S<seq-id>_: a back-reference to an already accepted component.
                        while (IsDigit(*m_cursor) || (*m_cursor >= 'A' && *m_cursor <= 'Z'))
                        {
                            ++m_cursor;
                        }
                        if (*m_cursor != '_')
                        {
                            return false;
                        }
                        ++m_cursor;
                    }
                }
                else if (!SourceName())
                {
                    return false;
                }
                return *m_cursor != 'I' || TemplateArgs();
            }

            bool Type() noexcept
            {
                if (At("vwbcahstijlmxynofde"))
                {
                    ++m_cursor;
                    return true;
                }
                if (At("PROKV"))
                {
                    ++m_cursor;
                    return Type();
                }
                if (*m_cursor == 'N')
                {
                    ++m_cursor;
                    do
                    {
                        if (!Component())
                        {
                            return false;
                        }
                    } while (*m_cursor != 'E');
                    ++m_cursor;
                    return true;
                }
                return Component();
            }

            // L<integral builtin>[n]<digits>E
            bool Literal() noexcept
            {
                ++m_cursor;
                if (!At("bcahstijlmxy"))
                {
                    return false;
                }
                ++m_cursor;
                if (*m_cursor == 'n')
                {
                    ++m_cursor;
                }
                if (!IsDigit(*m_cursor))
                {
                    return false;
                }
                while (IsDigit(*m_cursor))
                {
                    ++m_cursor;
                }
                if (*m_cursor != 'E')
                {
                    return false;
                }
                ++m_cursor;
                return true;
            }

            bool TemplateArg() noexcept { return *m_cursor == 'L' ? Literal() : Type(); }

            // I<arg>+E, where an argument may be a pack J<arg>*E.
            bool TemplateArgs() noexcept
            {
                ++m_cursor;
                while (*m_cursor != 'E')
                {
                    if (*m_cursor == 'J')
                    {
                        ++m_cursor;
                        while (*m_cursor != 'E')
                        {
                            if (!TemplateArg())
                            {
                                return false;
                            }
                        }
                        ++m_cursor;
                    }
                    else if (!TemplateArg())
                    {
                        return false;
                    }
                }
                ++m_cursor;
                return true;
            }

            const char* m_cursor;
        };
    } // namespace Detail

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

#if defined(__APPLE__) || (defined(_LIBCPP_VERSION) && !defined(_WIN32))
        // libc++ compares type_info by address on ELF (and for unique RTTI on Apple
        // arm64), and a hidden-visibility module carries its own copy, so the same SDK
        // event type would miss across the module boundary. Channels are therefore
        // hashed by mangled name and match by name for external-linkage types; the
        // lookup stays O(1) in both directions. libstdc++ and MSVC already compare
        // names, so they keep std::type_index. Internal-linkage types stay distinct
        // even when their mangled names match (Detail::SharedTypeNameParser).
        struct ChannelKey
        {
            const std::type_info* type;
            bool shared; ///< The name identifies the type across modules; a pure function of the name.
        };
        struct ChannelKeyHash
        {
            // Equal keys always have equal names, so hashing the name is consistent.
            size_t operator()(const ChannelKey& key) const noexcept
            {
                return std::hash<std::string_view>{}(key.type->name());
            }
        };
        struct ChannelKeyEqual
        {
            bool operator()(const ChannelKey& a, const ChannelKey& b) const noexcept
            {
                return *a.type == *b.type || (a.shared && b.shared && std::strcmp(a.type->name(), b.type->name()) == 0);
            }
        };
        using ChannelMap = std::unordered_map<ChannelKey, std::unique_ptr<IChannel>, ChannelKeyHash, ChannelKeyEqual>;
        template <typename E> static ChannelKey KeyOf() noexcept
        {
            // Parsed once per type per module image.
            static const bool shared = Detail::SharedTypeNameParser::IsShared(typeid(E).name());
            return ChannelKey{&typeid(E), shared};
        }
#else
        using ChannelMap = std::unordered_map<std::type_index, std::unique_ptr<IChannel>>;
        template <typename E> static std::type_index KeyOf() noexcept { return std::type_index(typeid(E)); }
#endif

        // Find the channel for E. The caller holds m_channelsMutex.
        template <typename E> IChannel* FindChannelLocked() const
        {
            const auto it = m_channels.find(KeyOf<E>());
            return it != m_channels.end() ? it->second.get() : nullptr;
        }

        // Get or create the channel for E (acquires m_channelsMutex)
        template <typename E> ChannelOf<E>& GetOrCreateChannel()
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            if (IChannel* existing = FindChannelLocked<E>())
                return *static_cast<ChannelOf<E>*>(existing);
            auto ch = std::make_unique<ChannelOf<E>>();
            auto* raw = ch.get();
            m_channels.emplace(KeyOf<E>(), std::move(ch));
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
        ChannelMap m_channels;
        std::mutex m_publishingMutex;
        std::vector<PublishingEntry> m_publishing;
        std::atomic<uint64_t> m_nextId{0};
        std::shared_ptr<LifetimeToken> m_lifetime = std::make_shared<LifetimeToken>();
    };

} // namespace Spark
