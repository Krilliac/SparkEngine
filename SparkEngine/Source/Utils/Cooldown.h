/**
 * @file Cooldown.h
 * @brief Reusable cooldown timer for gameplay, AI, and ability systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Replaces scattered manual float-tracking patterns (e.g. `timeSinceLastShot`,
 * `alertTimer`) with a self-contained, testable cooldown primitive.
 *
 * ## Usage
 * @code
 *   Spark::Cooldown fireCooldown(0.5f);  // 500ms between shots
 *
 *   // Each frame
 *   fireCooldown.Update(deltaTime);
 *   if (playerPressedFire && fireCooldown.IsReady()) {
 *       FireBullet();
 *       fireCooldown.Reset();
 *   }
 *
 *   // For UI display
 *   float pct = fireCooldown.GetProgress();  // 0.0 = just reset, 1.0 = ready
 * @endcode
 */

#pragma once

#include <algorithm>

namespace Spark
{

    /**
     * @class Cooldown
     * @brief Simple countdown timer that tracks readiness.
     *
     * Counts elapsed time toward a configurable duration. Once elapsed
     * time reaches or exceeds the duration the cooldown is "ready" and
     * can be reset for the next cycle.
     */
    class Cooldown
    {
      public:
        /**
         * @brief Construct a cooldown with a given duration.
         * @param duration  Time in seconds before the cooldown is ready.
         * @param startReady  If true, the cooldown starts in the ready state.
         */
        explicit Cooldown(float duration = 1.0f, bool startReady = true)
            : m_duration(duration), m_elapsed(startReady ? duration : 0.0f)
        {
        }

        /**
         * @brief Advance the cooldown by deltaTime seconds.
         * @param deltaTime  Frame delta time in seconds.
         */
        void Update(float deltaTime) { m_elapsed = (std::min)(m_elapsed + deltaTime, m_duration); }

        /**
         * @brief Check if the cooldown has finished.
         * @return true if elapsed time >= duration.
         */
        bool IsReady() const { return m_elapsed >= m_duration; }

        /**
         * @brief Reset the cooldown to start counting again from zero.
         */
        void Reset() { m_elapsed = 0.0f; }

        /**
         * @brief Reset and change the duration.
         * @param newDuration  New cooldown duration in seconds.
         */
        void Reset(float newDuration)
        {
            m_duration = newDuration;
            m_elapsed = 0.0f;
        }

        /**
         * @brief Force the cooldown into the ready state immediately.
         */
        void ForceReady() { m_elapsed = m_duration; }

        /**
         * @brief Get normalized progress toward readiness.
         * @return  0.0 when just reset, 1.0 when ready.
         */
        float GetProgress() const
        {
            if (m_duration <= 0.0f)
                return 1.0f;
            return (std::min)(m_elapsed / m_duration, 1.0f);
        }

        /**
         * @brief Get remaining time until ready.
         * @return  Seconds remaining, or 0.0 if already ready.
         */
        float GetRemaining() const { return (std::max)(m_duration - m_elapsed, 0.0f); }

        /**
         * @brief Get the configured duration.
         * @return  Duration in seconds.
         */
        float GetDuration() const { return m_duration; }

        /**
         * @brief Set a new duration without resetting elapsed time.
         * @param duration  New duration in seconds.
         */
        void SetDuration(float duration) { m_duration = duration; }

        /**
         * @brief Get elapsed time since last reset.
         * @return  Elapsed time in seconds.
         */
        float GetElapsed() const { return m_elapsed; }

      private:
        float m_duration;
        float m_elapsed;
    };

    /**
     * @class RepeatTimer
     * @brief Auto-resetting timer that fires periodically.
     *
     * Useful for periodic AI updates, spawn waves, or interval-based logic.
     *
     * @code
     *   Spark::RepeatTimer spawnTimer(2.0f); // every 2 seconds
     *   // Each frame:
     *   int fires = spawnTimer.Update(deltaTime);
     *   for (int i = 0; i < fires; ++i) SpawnEnemy();
     * @endcode
     */
    class RepeatTimer
    {
      public:
        /**
         * @brief Construct a repeating timer.
         * @param interval  Time between fires in seconds.
         */
        explicit RepeatTimer(float interval = 1.0f) : m_interval(interval), m_accumulated(0.0f) {}

        /**
         * @brief Advance the timer and return the number of intervals that elapsed.
         * @param deltaTime  Frame delta time in seconds.
         * @return           Number of times the timer fired this update (usually 0 or 1).
         */
        int Update(float deltaTime)
        {
            if (m_interval <= 0.0f)
                return 0;
            m_accumulated += deltaTime;
            int fires = 0;
            while (m_accumulated >= m_interval)
            {
                m_accumulated -= m_interval;
                ++fires;
            }
            return fires;
        }

        /**
         * @brief Reset accumulated time to zero.
         */
        void Reset() { m_accumulated = 0.0f; }

        /**
         * @brief Get the configured interval.
         * @return  Interval in seconds.
         */
        float GetInterval() const { return m_interval; }

        /**
         * @brief Set a new interval.
         * @param interval  New interval in seconds.
         */
        void SetInterval(float interval) { m_interval = interval; }

        /**
         * @brief Get progress toward the next fire [0, 1).
         * @return  Normalized progress.
         */
        float GetProgress() const
        {
            if (m_interval <= 0.0f)
                return 0.0f;
            return m_accumulated / m_interval;
        }

      private:
        float m_interval;
        float m_accumulated;
    };

} // namespace Spark
