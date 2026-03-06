/**
 * @file DayNightCycle.h
 * @brief Time-of-day system driving sun position, lighting, and sky color
 * @author Spark Engine Team
 * @date 2025
 *
 * Simulates a day/night cycle with configurable time scale. Outputs sun
 * direction, ambient color, and sky tint that can be fed into the lighting
 * and post-processing systems.
 *
 * ## Usage
 * @code
 *   Spark::DayNightCycle cycle;
 *   cycle.SetTimeOfDay(6.0f);      // 6 AM
 *   cycle.SetTimeScale(60.0f);     // 1 real second = 1 game minute
 *
 *   // Each frame
 *   cycle.Update(deltaTime);
 *
 *   auto sunDir = cycle.GetSunDirection();
 *   auto ambient = cycle.GetAmbientColor();
 *   auto skyColor = cycle.GetSkyColor();
 *   bool isNight = cycle.IsNight();
 * @endcode
 */

#pragma once

#include "../Events/EventSystem.h"

#include <cmath>
#include <algorithm>
#include <functional>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Spark {

// =============================================================================
// Color helpers
// =============================================================================

/**
 * @brief Simple RGBA color for platform-independent use
 */
struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    static Color Lerp(const Color& a, const Color& b, float t) {
        return {
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t
        };
    }
};

// =============================================================================
// Time-of-Day Period
// =============================================================================

/**
 * @brief Named periods of the day
 */
enum class DayPeriod {
    Night,          ///< 0:00 - 5:00
    Dawn,           ///< 5:00 - 7:00
    Morning,        ///< 7:00 - 10:00
    Midday,         ///< 10:00 - 14:00
    Afternoon,      ///< 14:00 - 17:00
    Dusk,           ///< 17:00 - 19:00
    Evening,        ///< 19:00 - 21:00
    LateNight       ///< 21:00 - 24:00
};

// =============================================================================
// Sun Direction
// =============================================================================

/**
 * @brief 3D vector for sun direction (platform-independent)
 */
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// =============================================================================
// Day/Night Cycle
// =============================================================================

/**
 * @class DayNightCycle
 * @brief Manages time-of-day simulation with sun movement and lighting parameters
 *
 * The sun follows a simple circular arc across the sky. At hour 12 (noon),
 * the sun is at its highest point. At hour 0/24 (midnight), the sun is
 * directly below the horizon.
 */
class DayNightCycle {
public:
    DayNightCycle() = default;

    /**
     * @brief Advance the time-of-day simulation
     * @param dt Real-time delta in seconds
     */
    void Update(float dt) {
        if (m_paused) return;

        float previousHour = m_currentHour;
        DayPeriod previousPeriod = GetDayPeriod();

        // Advance time
        float gameSeconds = dt * m_timeScale;
        m_currentHour += gameSeconds / 3600.0f;

        // Wrap around 24 hours
        while (m_currentHour >= 24.0f) {
            m_currentHour -= 24.0f;
            m_dayCount++;
        }

        // Detect period changes for callbacks
        DayPeriod currentPeriod = GetDayPeriod();
        if (currentPeriod != previousPeriod && m_onPeriodChanged) {
            m_onPeriodChanged(previousPeriod, currentPeriod);
        }

        // Recalculate derived values
        UpdateSunPosition();
        UpdateColors();
    }

    // ---- Time control ----

    /** @brief Set the current time of day [0, 24) */
    void SetTimeOfDay(float hour) {
        m_currentHour = std::fmod(std::max(0.0f, hour), 24.0f);
        UpdateSunPosition();
        UpdateColors();
    }

    /** @brief Get the current time of day [0, 24) */
    float GetTimeOfDay() const { return m_currentHour; }

    /**
     * @brief Set the time scale multiplier
     *
     * A scale of 60 means 1 real second = 1 game minute.
     * A scale of 1 means real-time (1 real second = 1 game second).
     */
    void SetTimeScale(float scale) { m_timeScale = std::max(0.0f, scale); }
    float GetTimeScale() const { return m_timeScale; }

    /** @brief Pause/unpause time progression */
    void SetPaused(bool paused) { m_paused = paused; }
    bool IsPaused() const { return m_paused; }

    /** @brief Get the number of days elapsed since simulation start */
    int GetDayCount() const { return m_dayCount; }

    // ---- Output values (computed each Update) ----

    /**
     * @brief Get the sun direction vector (normalized)
     *
     * Y-up coordinate system. Positive Y = sun above horizon.
     * Negative Y = sun below horizon (night).
     */
    Vec3 GetSunDirection() const { return m_sunDirection; }

    /** @brief Get the current ambient light color */
    Color GetAmbientColor() const { return m_ambientColor; }

    /** @brief Get the current sun/directional light color */
    Color GetSunColor() const { return m_sunColor; }

    /** @brief Get the current sky color for skybox tinting */
    Color GetSkyColor() const { return m_skyColor; }

    /** @brief Get the sun intensity multiplier [0, 1] */
    float GetSunIntensity() const { return m_sunIntensity; }

    /** @brief Get the ambient light intensity multiplier [0, 1] */
    float GetAmbientIntensity() const { return m_ambientIntensity; }

    /** @brief Check if it's currently nighttime (sun below horizon) */
    bool IsNight() const { return m_sunDirection.y < 0.0f; }

    /** @brief Check if it's daytime (sun above horizon) */
    bool IsDay() const { return m_sunDirection.y >= 0.0f; }

    /** @brief Get the current named period of the day */
    DayPeriod GetDayPeriod() const {
        if (m_currentHour < 5.0f) return DayPeriod::Night;
        if (m_currentHour < 7.0f) return DayPeriod::Dawn;
        if (m_currentHour < 10.0f) return DayPeriod::Morning;
        if (m_currentHour < 14.0f) return DayPeriod::Midday;
        if (m_currentHour < 17.0f) return DayPeriod::Afternoon;
        if (m_currentHour < 19.0f) return DayPeriod::Dusk;
        if (m_currentHour < 21.0f) return DayPeriod::Evening;
        return DayPeriod::LateNight;
    }

    /** @brief Get a human-readable name for the current period */
    static const char* GetPeriodName(DayPeriod period) {
        switch (period) {
            case DayPeriod::Night:      return "Night";
            case DayPeriod::Dawn:       return "Dawn";
            case DayPeriod::Morning:    return "Morning";
            case DayPeriod::Midday:     return "Midday";
            case DayPeriod::Afternoon:  return "Afternoon";
            case DayPeriod::Dusk:       return "Dusk";
            case DayPeriod::Evening:    return "Evening";
            case DayPeriod::LateNight:  return "Late Night";
            default:                    return "Unknown";
        }
    }

    /** @brief Get a formatted time string (e.g. "14:30") */
    std::string GetTimeString() const {
        int hours = static_cast<int>(m_currentHour);
        int minutes = static_cast<int>((m_currentHour - hours) * 60.0f);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", hours, minutes);
        return std::string(buf);
    }

    // ---- Configuration ----

    /** @brief Set the sun's east-west arc axis (default: east to west along X axis) */
    void SetSunArcAxis(float x, float z) {
        m_sunArcX = x;
        m_sunArcZ = z;
    }

    /** @brief Set callback for day period transitions */
    void SetOnPeriodChanged(std::function<void(DayPeriod, DayPeriod)> callback) {
        m_onPeriodChanged = std::move(callback);
    }

private:
    void UpdateSunPosition() {
        // Map hour to angle: 0h = -PI/2 (nadir), 6h = 0 (horizon), 12h = PI/2 (zenith), 18h = PI (horizon), 24h = 3PI/2
        float angle = (m_currentHour / 24.0f) * 2.0f * static_cast<float>(M_PI) - static_cast<float>(M_PI) / 2.0f;

        m_sunDirection.x = m_sunArcX * std::cos(angle);
        m_sunDirection.y = std::sin(angle);
        m_sunDirection.z = m_sunArcZ * std::cos(angle);

        // Normalize
        float len = std::sqrt(m_sunDirection.x * m_sunDirection.x +
                              m_sunDirection.y * m_sunDirection.y +
                              m_sunDirection.z * m_sunDirection.z);
        if (len > 0.0001f) {
            m_sunDirection.x /= len;
            m_sunDirection.y /= len;
            m_sunDirection.z /= len;
        }

        // Sun intensity based on height above horizon
        m_sunIntensity = std::clamp(m_sunDirection.y * 2.0f, 0.0f, 1.0f);
    }

    void UpdateColors() {
        // Color palette for different times of day
        static const Color nightAmbient    = {0.05f, 0.05f, 0.15f, 1.0f};
        static const Color dawnAmbient     = {0.4f,  0.3f,  0.2f,  1.0f};
        static const Color dayAmbient      = {0.6f,  0.6f,  0.7f,  1.0f};
        static const Color duskAmbient     = {0.4f,  0.25f, 0.15f, 1.0f};

        static const Color nightSun   = {0.1f,  0.1f,  0.2f,  1.0f};
        static const Color dawnSun    = {1.0f,  0.6f,  0.3f,  1.0f};
        static const Color daySun     = {1.0f,  0.95f, 0.85f, 1.0f};
        static const Color duskSun    = {1.0f,  0.5f,  0.2f,  1.0f};

        static const Color nightSky   = {0.02f, 0.02f, 0.08f, 1.0f};
        static const Color dawnSky    = {0.7f,  0.5f,  0.4f,  1.0f};
        static const Color daySky     = {0.4f,  0.6f,  0.9f,  1.0f};
        static const Color duskSky    = {0.8f,  0.4f,  0.3f,  1.0f};

        float hour = m_currentHour;

        if (hour < 5.0f) {
            // Night
            m_ambientColor = nightAmbient;
            m_sunColor = nightSun;
            m_skyColor = nightSky;
            m_ambientIntensity = 0.1f;
        } else if (hour < 7.0f) {
            // Dawn transition
            float t = (hour - 5.0f) / 2.0f;
            m_ambientColor = Color::Lerp(nightAmbient, dawnAmbient, t);
            m_sunColor = Color::Lerp(nightSun, dawnSun, t);
            m_skyColor = Color::Lerp(nightSky, dawnSky, t);
            m_ambientIntensity = 0.1f + t * 0.3f;
        } else if (hour < 10.0f) {
            // Morning -> midday
            float t = (hour - 7.0f) / 3.0f;
            m_ambientColor = Color::Lerp(dawnAmbient, dayAmbient, t);
            m_sunColor = Color::Lerp(dawnSun, daySun, t);
            m_skyColor = Color::Lerp(dawnSky, daySky, t);
            m_ambientIntensity = 0.4f + t * 0.6f;
        } else if (hour < 17.0f) {
            // Day
            m_ambientColor = dayAmbient;
            m_sunColor = daySun;
            m_skyColor = daySky;
            m_ambientIntensity = 1.0f;
        } else if (hour < 19.0f) {
            // Dusk transition
            float t = (hour - 17.0f) / 2.0f;
            m_ambientColor = Color::Lerp(dayAmbient, duskAmbient, t);
            m_sunColor = Color::Lerp(daySun, duskSun, t);
            m_skyColor = Color::Lerp(daySky, duskSky, t);
            m_ambientIntensity = 1.0f - t * 0.6f;
        } else if (hour < 21.0f) {
            // Evening -> night
            float t = (hour - 19.0f) / 2.0f;
            m_ambientColor = Color::Lerp(duskAmbient, nightAmbient, t);
            m_sunColor = Color::Lerp(duskSun, nightSun, t);
            m_skyColor = Color::Lerp(duskSky, nightSky, t);
            m_ambientIntensity = 0.4f - t * 0.3f;
        } else {
            // Late night
            m_ambientColor = nightAmbient;
            m_sunColor = nightSun;
            m_skyColor = nightSky;
            m_ambientIntensity = 0.1f;
        }
    }

    // Time state
    float m_currentHour = 12.0f;       ///< Current time [0, 24)
    float m_timeScale = 60.0f;         ///< Time multiplier (60 = 1 real sec = 1 game min)
    bool m_paused = false;
    int m_dayCount = 0;

    // Sun arc configuration
    float m_sunArcX = 1.0f;           ///< Sun east-west movement axis
    float m_sunArcZ = 0.0f;           ///< Sun north-south movement axis

    // Computed output
    Vec3 m_sunDirection = {0.0f, 1.0f, 0.0f};
    Color m_ambientColor;
    Color m_sunColor;
    Color m_skyColor;
    float m_sunIntensity = 1.0f;
    float m_ambientIntensity = 1.0f;

    // Callback
    std::function<void(DayPeriod, DayPeriod)> m_onPeriodChanged;
};

} // namespace Spark
