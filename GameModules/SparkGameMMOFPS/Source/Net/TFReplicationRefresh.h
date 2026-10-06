/** @file TFReplicationRefresh.h @brief Bounded dirty-state cache with periodic loss recovery. */
#pragma once

#include "Core/TFTypes.h"

#include <cstddef>
#include <unordered_map>

namespace Terrafront
{
    /**
     * Unreliable delivery cannot confirm that a quiet entity's last update arrived.
     * Refresh its full state after twenty replication ticks (one second), while
     * sending changes immediately. State and refresh time share the same bounded
     * entity lifetime; there is no separate per-peer refresh state.
     */
    template <typename State> class TFReplicationStateCache
    {
      public:
        static constexpr unsigned int kRefreshTicks = 20;
        static constexpr double kRefreshSeconds = kRefreshTicks / static_cast<double>(kReplicationHz);

        bool ShouldSend(EntityId entity, const State& state, double clock)
        {
            const auto previous = m_entries.find(entity);
            if (previous != m_entries.end() && previous->second.state == state && clock >= previous->second.sentAt &&
                clock - previous->second.sentAt < kRefreshSeconds)
            {
                return false;
            }
            Remember(entity, state, clock);
            return true;
        }

        void Remember(EntityId entity, const State& state, double clock)
        {
            m_entries.insert_or_assign(entity, Entry{state, clock});
        }

        void Erase(EntityId entity) { m_entries.erase(entity); }
        void Clear() { m_entries.clear(); }
        bool Empty() const { return m_entries.empty(); }
        std::size_t Size() const { return m_entries.size(); }
        const auto& Entries() const { return m_entries; }

      private:
        struct Entry
        {
            State state;
            double sentAt;
        };
        std::unordered_map<EntityId, Entry> m_entries;
    };
} // namespace Terrafront
