/**
 * @file TimeOfDaySystem.h
 * @brief Time-of-day simulation with sun direction, light color, and weather integration
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks a 24-hour clock (0.0 = midnight, 12.0 = noon) and derives sun direction,
 * directional light color, and ambient intensity. Integrates with WeatherSystem
 * when available via EngineContext.
 *
 * @code
 *   auto& tod = Spark::TimeOfDaySystem::GetInstance();
 *   tod.SetTimeOfDay(14.5f);      // 2:30 PM
 *   tod.SetTimeScale(60.0f);      // 1 real second = 1 game minute
 *   tod.Update(deltaTime);
 *
 *   auto sunDir = tod.GetSunDirection();
 *   auto sunColor = tod.GetSunColor();
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <algorithm>
#include <cmath>
#include <string>

namespace Spark
{

    /**
     * @brief Period of the day derived from the current hour
     */
    enum class DayPeriod
    {
        Night,     ///< 0:00 - 5:00
        Dawn,      ///< 5:00 - 7:00
        Morning,   ///< 7:00 - 10:00
        Midday,    ///< 10:00 - 14:00
        Afternoon, ///< 14:00 - 17:00
        Dusk,      ///< 17:00 - 19:00
        Evening,   ///< 19:00 - 21:00
        LateNight  ///< 21:00 - 24:00
    };

    /**
     * @brief Simulates a 24-hour day/night cycle with sun position and light color
     *
     * The system tracks time in hours [0, 24) and computes:
     * - Sun direction via spherical mapping (rises in east, sets in west)
     * - Sun color that shifts warm at dawn/dusk, white at noon, blue-tinted at night
     * - Sun intensity based on elevation above the horizon
     * - Ambient light that dims at night
     *
     * The time scale controls how fast game time passes relative to real time.
     * A scale of 60.0 means 1 real second = 1 game minute.
     */
    class TimeOfDaySystem
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the global TimeOfDaySystem
         */
        [[deprecated("Use EngineContext::Get()->GetSystem<TimeOfDaySystem>() instead")]]
        static TimeOfDaySystem& GetInstance();

        /**
         * @brief Advance the clock by deltaTime (real seconds) scaled by time scale
         * @param deltaTime Real-time seconds since last frame
         */
        void Update(float deltaTime);

        // ---- Time control ---------------------------------------------------

        /**
         * @brief Set the current time of day
         * @param hour Hour in [0, 24). Values are wrapped modulo 24.
         */
        void SetTimeOfDay(float hour);

        /** @brief Get the current time of day in hours [0, 24) */
        float GetTimeOfDay() const { return m_hour; }

        /**
         * @brief Set how fast game time passes relative to real time
         * @param scale Seconds of game time per real second. 60 = 1 min/sec. 0 = paused.
         */
        void SetTimeScale(float scale) { m_timeScale = std::max(0.0f, scale); }

        /** @brief Get the current time scale */
        float GetTimeScale() const { return m_timeScale; }

        /** @brief Pause time progression */
        void SetPaused(bool paused) { m_paused = paused; }

        /** @brief Check if time is paused */
        bool IsPaused() const { return m_paused; }

        /** @brief Get the number of full days elapsed since system start */
        int GetDayCount() const { return m_dayCount; }

        // ---- Derived lighting state -----------------------------------------

        /** @brief Get the normalized sun direction vector (points toward sun) */
        DirectX::XMFLOAT3 GetSunDirection() const { return m_sunDirection; }

        /** @brief Get the sun color based on time of day */
        DirectX::XMFLOAT3 GetSunColor() const { return m_sunColor; }

        /** @brief Get sun intensity [0, 1] — 0 when below horizon */
        float GetSunIntensity() const { return m_sunIntensity; }

        /** @brief Get ambient light color (dim blue at night, warm gray during day) */
        DirectX::XMFLOAT3 GetAmbientColor() const { return m_ambientColor; }

        /** @brief Get ambient light intensity [0, 1] */
        float GetAmbientIntensity() const { return m_ambientIntensity; }

        /** @brief Is the sun below the horizon? */
        bool IsNight() const { return m_sunDirection.y < 0.0f; }

        /** @brief Is the sun above the horizon? */
        bool IsDay() const { return m_sunDirection.y >= 0.0f; }

        /** @brief Get the current period of the day */
        DayPeriod GetDayPeriod() const;

        /**
         * @brief Get formatted time string (HH:MM)
         * @return Time string like "14:30"
         */
        std::string GetTimeString() const;

      private:
        TimeOfDaySystem() = default;

        /** @brief Recompute sun direction, color, and intensity from m_hour */
        void UpdateSunState();

        /** @brief Linearly interpolate between two colors */
        static DirectX::XMFLOAT3 LerpColor(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);

        float m_hour = 12.0f;      ///< Current time in hours [0, 24)
        float m_timeScale = 60.0f; ///< Game seconds per real second (60 = 1 min/sec)
        bool m_paused = false;     ///< Whether time progression is paused
        int m_dayCount = 0;        ///< Number of full days elapsed

        DirectX::XMFLOAT3 m_sunDirection{0.0f, 1.0f, 0.0f};  ///< Normalized sun direction
        DirectX::XMFLOAT3 m_sunColor{1.0f, 1.0f, 1.0f};      ///< Sun light color
        float m_sunIntensity = 1.0f;                         ///< Sun intensity [0, 1]
        DirectX::XMFLOAT3 m_ambientColor{0.1f, 0.1f, 0.12f}; ///< Ambient light color
        float m_ambientIntensity = 0.3f;                     ///< Ambient intensity [0, 1]
    };

} // namespace Spark
