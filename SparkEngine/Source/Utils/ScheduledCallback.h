/**
 * @file ScheduledCallback.h
 * @brief Time-delayed and repeating callback scheduler
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a simple "do X after N seconds" mechanism without ad-hoc Cooldown +
 * boolean flag patterns. Supports one-shot and repeating callbacks.
 *
 * ## Usage
 * @code
 *   Spark::Scheduler scheduler;
 *
 *   // One-shot: explode after 2 seconds
 *   auto h = scheduler.Schedule([]{ SpawnExplosion(); }, 2.0f);
 *
 *   // Repeating: spawn enemies every 5 seconds
 *   scheduler.ScheduleRepeating([]{ SpawnEnemy(); }, 5.0f);
 *
 *   // Each frame
 *   scheduler.Update(deltaTime);
 *
 *   // Cancel a specific callback
 *   scheduler.Cancel(h);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace Spark
{

    /**
     * @class Scheduler
     * @brief Manages time-delayed and repeating callbacks.
     *
     * Call Update(dt) once per frame. Callbacks fire when their time arrives.
     * Not thread-safe.
     */
    class Scheduler
    {
      public:
        using CallbackHandle = uint64_t;

        /**
         * @brief Schedule a one-shot callback after a delay.
         * @param callback Function to invoke.
         * @param delay    Seconds until the callback fires.
         * @return Handle for cancellation.
         */
        [[nodiscard]] CallbackHandle Schedule(std::function<void()> callback, float delay)
        {
            CallbackHandle id = m_nextId++;
            m_entries.push_back({id, std::move(callback), delay, 0.0f, false});
            return id;
        }

        /**
         * @brief Schedule a repeating callback at a fixed interval.
         * @param callback Function to invoke each interval.
         * @param interval Seconds between each invocation.
         * @return Handle for cancellation.
         */
        [[nodiscard]] CallbackHandle ScheduleRepeating(std::function<void()> callback, float interval)
        {
            CallbackHandle id = m_nextId++;
            m_entries.push_back({id, std::move(callback), interval, interval, true});
            return id;
        }

        /**
         * @brief Cancel a scheduled callback.
         * @return true if the callback was still pending and was removed.
         */
        bool Cancel(CallbackHandle handle)
        {
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            {
                if (it->id == handle)
                {
                    m_entries.erase(it);
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Advance time and fire any callbacks whose time has come.
         * @param dt Frame delta time in seconds.
         */
        void Update(float dt)
        {
            // Iterate with index since vector may be modified by removal
            size_t i = 0;
            while (i < m_entries.size())
            {
                auto& entry = m_entries[i];
                entry.timeRemaining -= dt;

                if (entry.timeRemaining <= 0.0f)
                {
                    entry.callback();

                    if (entry.repeating)
                    {
                        entry.timeRemaining += entry.interval;
                        ++i;
                    }
                    else
                    {
                        // Remove one-shot by swapping with last element
                        m_entries[i] = std::move(m_entries.back());
                        m_entries.pop_back();
                        // Don't increment i — re-check the swapped element
                    }
                }
                else
                {
                    ++i;
                }
            }
        }

        /**
         * @brief Number of pending callbacks (one-shot + repeating).
         */
        [[nodiscard]] size_t GetPendingCount() const { return m_entries.size(); }

        /**
         * @brief Cancel all pending callbacks.
         */
        void ClearAll() { m_entries.clear(); }

      private:
        struct Entry
        {
            CallbackHandle id;
            std::function<void()> callback;
            float timeRemaining;
            float interval;
            bool repeating;
        };

        std::vector<Entry> m_entries;
        CallbackHandle m_nextId = 1;
    };

} // namespace Spark
