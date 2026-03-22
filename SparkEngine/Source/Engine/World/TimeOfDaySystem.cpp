/**
 * @file TimeOfDaySystem.cpp
 * @brief Time-of-day simulation — sun position, light color, ambient intensity
 */

#include "TimeOfDaySystem.h"

#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace DirectX;

namespace Spark
{

    TimeOfDaySystem& TimeOfDaySystem::GetInstance()
    {
        static TimeOfDaySystem instance;
        return instance;
    }

    void TimeOfDaySystem::Update(float deltaTime)
    {
        if (m_paused || deltaTime <= 0.0f)
            return;

        // Advance game time: timeScale converts real seconds to game seconds,
        // then divide by 3600 to get hours
        float gameSeconds = deltaTime * m_timeScale;
        m_hour += gameSeconds / 3600.0f;

        // Wrap past midnight
        while (m_hour >= 24.0f)
        {
            m_hour -= 24.0f;
            m_dayCount++;
        }

        UpdateSunState();
    }

    void TimeOfDaySystem::SetTimeOfDay(float hour)
    {
        m_hour = std::fmod(std::max(0.0f, hour), 24.0f);
        UpdateSunState();
    }

    DayPeriod TimeOfDaySystem::GetDayPeriod() const
    {
        if (m_hour < 5.0f)
            return DayPeriod::Night;
        if (m_hour < 7.0f)
            return DayPeriod::Dawn;
        if (m_hour < 10.0f)
            return DayPeriod::Morning;
        if (m_hour < 14.0f)
            return DayPeriod::Midday;
        if (m_hour < 17.0f)
            return DayPeriod::Afternoon;
        if (m_hour < 19.0f)
            return DayPeriod::Dusk;
        if (m_hour < 21.0f)
            return DayPeriod::Evening;
        return DayPeriod::LateNight;
    }

    std::string TimeOfDaySystem::GetTimeString() const
    {
        int h = static_cast<int>(m_hour);
        int m = static_cast<int>((m_hour - static_cast<float>(h)) * 60.0f);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        return std::string(buf);
    }

    // =========================================================================
    // Sun state computation
    // =========================================================================

    void TimeOfDaySystem::UpdateSunState()
    {
        // Map hour to angle: 6:00 = sunrise (0 elevation), 12:00 = noon (max elevation),
        // 18:00 = sunset (0 elevation). Uses a shifted sine wave.
        // angle = (hour/24) * 2*PI - PI/2, so that hour=6 -> angle=0, hour=12 -> PI/2
        float t = m_hour / 24.0f;
        float angle = t * 2.0f * static_cast<float>(M_PI) - static_cast<float>(M_PI) / 2.0f;

        // Sun elevation (y) and east-west position (x)
        // At noon (angle = PI/2): y = 1 (directly overhead)
        // At midnight (angle = -PI/2): y = -1 (below horizon)
        float elevation = std::sin(angle);
        float azimuth = std::cos(angle);

        // Normalize the direction vector
        float len = std::sqrt(elevation * elevation + azimuth * azimuth);
        if (len > 0.0001f)
        {
            m_sunDirection.x = azimuth / len;
            m_sunDirection.y = elevation / len;
            m_sunDirection.z = 0.0f;
        }

        // Sun intensity: ramp from 0 at horizon to 1 when fully above
        m_sunIntensity = std::clamp(elevation * 2.0f, 0.0f, 1.0f);

        // Sun color varies with time of day:
        // - Dawn/dusk (sun near horizon): warm orange/red
        // - Midday: white-ish
        // - Night: no sun contribution
        constexpr XMFLOAT3 kNoonColor{1.0f, 0.98f, 0.92f};    // Slightly warm white
        constexpr XMFLOAT3 kDawnDuskColor{1.0f, 0.55f, 0.2f}; // Warm orange
        constexpr XMFLOAT3 kNightColor{0.1f, 0.1f, 0.2f};     // Deep blue

        if (elevation > 0.3f)
        {
            // Full daylight — interpolate from dawn to noon
            float dayT = std::clamp((elevation - 0.3f) / 0.7f, 0.0f, 1.0f);
            m_sunColor = LerpColor(kDawnDuskColor, kNoonColor, dayT);
        }
        else if (elevation > 0.0f)
        {
            // Near horizon — dawn/dusk colors
            float horizonT = elevation / 0.3f;
            m_sunColor = LerpColor(kNightColor, kDawnDuskColor, horizonT);
        }
        else
        {
            // Below horizon — night
            m_sunColor = kNightColor;
        }

        // Ambient light: brighter during day, dim blue at night
        constexpr XMFLOAT3 kDayAmbient{0.15f, 0.15f, 0.14f};
        constexpr XMFLOAT3 kNightAmbient{0.03f, 0.03f, 0.06f};
        float ambientT = std::clamp(elevation + 0.2f, 0.0f, 1.0f);
        m_ambientColor = LerpColor(kNightAmbient, kDayAmbient, ambientT);
        m_ambientIntensity = 0.1f + 0.3f * ambientT;
    }

    DirectX::XMFLOAT3 TimeOfDaySystem::LerpColor(const XMFLOAT3& a, const XMFLOAT3& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

} // namespace Spark
