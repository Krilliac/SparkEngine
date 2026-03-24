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

#include "LogMacros.h"

#include <cstdint>
#include <exception>
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
            if (delay < 0.0f)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "Scheduler::Schedule called with negative delay %.3f, clamping to 0", delay);
                delay = 0.0f;
            }
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
            if (interval <= 0.0f)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                "Scheduler::ScheduleRepeating called with non-positive interval %.3f, ignoring",
                                interval);
                return 0;
            }
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
            if (dt < 0.0f)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Scheduler::Update called with negative dt %.3f, ignoring",
                               dt);
                return;
            }

            // Iterate with index since vector may be modified by removal
            size_t i = 0;
            while (i < m_entries.size())
            {
                auto& entry = m_entries[i];
                entry.timeRemaining -= dt;

                if (entry.timeRemaining <= 0.0f)
                {
                    try
                    {
                        entry.callback();
                    }
                    catch (const std::exception& e)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Core, "Scheduler callback %llu threw: %s",
                                        static_cast<unsigned long long>(entry.id), e.what());
                    }
                    catch (...)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Core, "Scheduler callback %llu threw unknown exception",
                                        static_cast<unsigned long long>(entry.id));
                    }

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
