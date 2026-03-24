/**
 * @file Delegate.h
 * @brief Lightweight multicast delegate for local one-to-many callbacks
 * @author Spark Engine Team
 * @date 2026
 *
 * Lighter than EventBus for direct object-to-object signaling where global
 * pub/sub and thread safety are not needed. Suitable for UI button clicks,
 * component change notifications, and property observers.
 *
 * ## Usage
 * @code
 *   Spark::Delegate<int, float> onDamage;
 *
 *   auto id = (onDamage += [](int hp, float dmg) {
 *       std::print("Hit for {} damage, {} HP left\n", dmg, hp);
 *   });
 *
 *   onDamage.Broadcast(85, 15.0f);
 *
 *   onDamage -= id;  // Unsubscribe
 * @endcode
 */

#pragma once

#include "LogMacros.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <utility>
#include <vector>

namespace Spark
{

    /**
     * @class Delegate
     * @brief A multicast delegate that invokes all registered handlers on Broadcast.
     * @tparam Args Argument types passed to handlers.
     *
     * Not thread-safe. Single-threaded use only. For thread-safe global pub/sub,
     * use EventBus instead.
     */
    template <typename... Args> class Delegate
    {
      public:
        using HandlerID = uint64_t;

        /**
         * @brief Register a handler. Returns an ID for later removal.
         */
        [[nodiscard]] HandlerID operator+=(std::function<void(Args...)> handler)
        {
            HandlerID id = m_nextId++;
            m_handlers.emplace_back(id, std::move(handler));
            return id;
        }

        /**
         * @brief Remove a handler by its ID.
         */
        void operator-=(HandlerID id)
        {
            m_handlers.erase(std::remove_if(m_handlers.begin(), m_handlers.end(),
                                            [id](const auto& entry) { return entry.first == id; }),
                             m_handlers.end());
        }

        /**
         * @brief Invoke all registered handlers with the given arguments.
         */
        void Broadcast(Args... args) const
        {
            for (const auto& [id, handler] : m_handlers)
            {
                try
                {
                    handler(args...);
                }
                catch (const std::exception& e)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "Delegate::Broadcast handler %llu threw: %s",
                                    static_cast<unsigned long long>(id), e.what());
                }
                catch (...)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "Delegate::Broadcast handler %llu threw unknown exception",
                                    static_cast<unsigned long long>(id));
                }
            }
        }

        /**
         * @brief Remove all handlers.
         */
        void Clear() { m_handlers.clear(); }

        /**
         * @brief Number of currently registered handlers.
         */
        [[nodiscard]] size_t GetHandlerCount() const { return m_handlers.size(); }

      private:
        std::vector<std::pair<HandlerID, std::function<void(Args...)>>> m_handlers;
        HandlerID m_nextId = 1;
    };

} // namespace Spark
