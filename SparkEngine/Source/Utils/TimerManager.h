/**
 * @file TimerManager.h
 * @brief Centralized gameplay timer service with named timers
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages named timers with pause/resume/rate control, separate from
 * the tween and coroutine systems. Inspired by Unreal's FTimerManager.
 *
 * ## Usage
 * @code
 *   auto& timers = Spark::TimerManager::GetInstance();
 *   timers.Initialize();
 *
 *   // One-shot timer (fire once after 3 seconds)
 *   timers.SetTimer("respawn", 3.0f, false, []() {
 *       SpawnPlayer();
 *   });
 *
 *   // Looping timer (fire every 1 second)
 *   timers.SetTimer("regen", 1.0f, true, []() {
 *       player.health += 5;
 *   });
 *
 *   // Pause/resume
 *   timers.PauseTimer("regen");
 *   timers.ResumeTimer("regen");
 *
 *   // Change rate
 *   timers.SetTimerRate("regen", 0.5f);  // now fires every 0.5s
 *
 *   // Query
 *   float remaining = timers.GetRemainingTime("respawn");
 *   bool active = timers.IsTimerActive("respawn");
 *
 *   // Each frame:
 *   timers.Update(deltaTime);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "LogMacros.h"

namespace Spark
{

    /** @brief Timer state */
    enum class TimerState
    {
        Active, ///< Counting down
        Paused, ///< Frozen, will resume where it left off
        Expired ///< Completed (one-shot) or pending removal
    };

    /** @brief Callback type for timer expiration */
    using TimerCallback = std::function<void()>;

    /** @brief A single managed timer */
    struct ManagedTimer
    {
        std::string name;                      ///< Timer name for lookup
        float rate = 1.0f;                     ///< Interval in seconds
        float elapsed = 0.0f;                  ///< Time accumulated since last fire
        bool looping = false;                  ///< Whether to repeat
        TimerState state = TimerState::Active; ///< Current state
        TimerCallback callback;                ///< Function to call on expiration
        uint32_t fireCount = 0;                ///< Number of times this timer has fired
    };

    /**
     * @brief Centralized gameplay timer service
     *
     * Manages a pool of named timers that tick each frame. Timers can be
     * one-shot or looping, paused/resumed, and have their rate changed
     * at runtime. More structured than raw cooldown tracking.
     *
     * Thread safety: Not thread-safe. Call from main thread only.
     */
    class TimerManager
    {
      public:
        static TimerManager& GetInstance()
        {
            static TimerManager instance;
            return instance;
        }

        /** @brief Initialize the timer manager */
        void Initialize()
        {
            m_timers.clear();
            m_pendingRemovals.clear();
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "TimerManager initialized");
        }

        /** @brief Shut down and clear all timers */
        void Shutdown()
        {
            m_timers.clear();
            m_initialized = false;
        }

        /**
         * @brief Set (create or replace) a named timer
         * @param name Unique timer name
         * @param rate Interval in seconds
         * @param looping Whether to repeat after firing
         * @param callback Function to call when timer fires
         */
        void SetTimer(const std::string& name, float rate, bool looping, TimerCallback callback)
        {
            if (name.empty())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "TimerManager::SetTimer: empty timer name");
                return;
            }
            if (rate <= 0.0f)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "TimerManager::SetTimer: invalid rate {} for timer '{}'", rate,
                               name);
                return;
            }
            if (!callback)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "TimerManager::SetTimer: null callback for timer '{}'", name);
                return;
            }
            SPARK_LOG_DEBUG(Spark::LogCategory::Core, "TimerManager: set timer '{}' rate={}s looping={}", name, rate,
                            looping);

            ManagedTimer timer;
            timer.name = name;
            timer.rate = rate;
            timer.looping = looping;
            timer.callback = std::move(callback);
            timer.state = TimerState::Active;
            timer.elapsed = 0.0f;
            timer.fireCount = 0;
            m_timers[name] = std::move(timer);
        }

        /** @brief Remove a timer */
        void ClearTimer(const std::string& name)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Core, "TimerManager: clearing timer '{}'", name);
            m_pendingRemovals.push_back(name);
        }

        /** @brief Clear all timers */
        void ClearAllTimers() { m_timers.clear(); }

        /** @brief Pause a timer (preserves elapsed time) */
        void PauseTimer(const std::string& name)
        {
            auto it = m_timers.find(name);
            if (it != m_timers.end() && it->second.state == TimerState::Active)
                it->second.state = TimerState::Paused;
        }

        /** @brief Resume a paused timer */
        void ResumeTimer(const std::string& name)
        {
            auto it = m_timers.find(name);
            if (it != m_timers.end() && it->second.state == TimerState::Paused)
                it->second.state = TimerState::Active;
        }

        /** @brief Change the rate of an existing timer */
        void SetTimerRate(const std::string& name, float newRate)
        {
            auto it = m_timers.find(name);
            if (it != m_timers.end() && newRate > 0.0f)
                it->second.rate = newRate;
        }

        /** @brief Reset a timer's elapsed time to zero */
        void ResetTimer(const std::string& name)
        {
            auto it = m_timers.find(name);
            if (it != m_timers.end())
            {
                it->second.elapsed = 0.0f;
                it->second.state = TimerState::Active;
            }
        }

        /**
         * @brief Update all timers — call once per frame
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime)
        {
            if (!m_initialized)
                return;

            // Process pending removals from previous frame
            for (const auto& name : m_pendingRemovals)
                m_timers.erase(name);
            m_pendingRemovals.clear();

            // Tick all active timers
            for (auto& [name, timer] : m_timers)
            {
                if (timer.state != TimerState::Active)
                    continue;

                timer.elapsed += deltaTime;

                if (timer.elapsed >= timer.rate)
                {
                    // Fire the callback
                    if (timer.callback)
                        timer.callback();
                    timer.fireCount++;

                    if (timer.looping)
                    {
                        // Subtract rate (handles accumulated time for very short timers)
                        timer.elapsed -= timer.rate;
                    }
                    else
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "TimerManager: one-shot timer '{}' fired", name);
                        timer.state = TimerState::Expired;
                        m_pendingRemovals.push_back(name);
                    }
                }
            }
        }

        // ====================================================================
        // Query
        // ====================================================================

        /** @brief Check if a timer exists and is active */
        bool IsTimerActive(const std::string& name) const
        {
            auto it = m_timers.find(name);
            return it != m_timers.end() && it->second.state == TimerState::Active;
        }

        /** @brief Check if a timer exists (any state) */
        bool TimerExists(const std::string& name) const { return m_timers.count(name) > 0; }

        /** @brief Check if a timer is paused */
        bool IsTimerPaused(const std::string& name) const
        {
            auto it = m_timers.find(name);
            return it != m_timers.end() && it->second.state == TimerState::Paused;
        }

        /** @brief Get remaining time until next fire */
        float GetRemainingTime(const std::string& name) const
        {
            auto it = m_timers.find(name);
            if (it == m_timers.end())
                return 0.0f;
            float remaining = it->second.rate - it->second.elapsed;
            return remaining > 0.0f ? remaining : 0.0f;
        }

        /** @brief Get elapsed time since last fire */
        float GetElapsedTime(const std::string& name) const
        {
            auto it = m_timers.find(name);
            return (it != m_timers.end()) ? it->second.elapsed : 0.0f;
        }

        /** @brief Get the number of times a timer has fired */
        uint32_t GetFireCount(const std::string& name) const
        {
            auto it = m_timers.find(name);
            return (it != m_timers.end()) ? it->second.fireCount : 0;
        }

        /** @brief Get total number of timers */
        size_t GetTimerCount() const { return m_timers.size(); }

        /** @brief Get all timer names */
        std::vector<std::string> GetTimerNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_timers.size());
            for (const auto& [name, timer] : m_timers)
                names.push_back(name);
            return names;
        }

        /** @brief Get console-friendly status */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[TimerManager] Not initialized";
            uint32_t active = 0, paused = 0;
            for (const auto& [name, timer] : m_timers)
            {
                if (timer.state == TimerState::Active)
                    active++;
                else if (timer.state == TimerState::Paused)
                    paused++;
            }
            return "[TimerManager] Total: " + std::to_string(m_timers.size()) + " | Active: " + std::to_string(active) +
                   " | Paused: " + std::to_string(paused);
        }

      private:
        TimerManager() = default;

        bool m_initialized = false;
        std::unordered_map<std::string, ManagedTimer> m_timers;
        std::vector<std::string> m_pendingRemovals;
    };

} // namespace Spark
