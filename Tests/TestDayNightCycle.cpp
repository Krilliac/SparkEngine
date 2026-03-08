// TestDayNightCycle.cpp - Tests for day/night cycle time-of-day system
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <algorithm>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TestDayNight
{

    struct Color
    {
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        static Color Lerp(const Color& a, const Color& b, float t)
        {
            return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
        }
    };

    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };

    enum class DayPeriod
    {
        Night,
        Dawn,
        Morning,
        Midday,
        Afternoon,
        Dusk,
        Evening,
        LateNight
    };

    class DayNightCycle
    {
      public:
        void Update(float dt)
        {
            if (m_paused)
                return;
            float gameSeconds = dt * m_timeScale;
            m_hour += gameSeconds / 3600.0f;
            while (m_hour >= 24.0f)
            {
                m_hour -= 24.0f;
                m_dayCount++;
            }
            UpdateSun();
        }

        void SetTimeOfDay(float hour)
        {
            m_hour = std::fmod(std::max(0.0f, hour), 24.0f);
            UpdateSun();
        }

        float GetTimeOfDay() const { return m_hour; }
        void SetTimeScale(float s) { m_timeScale = std::max(0.0f, s); }
        float GetTimeScale() const { return m_timeScale; }
        void SetPaused(bool p) { m_paused = p; }
        bool IsPaused() const { return m_paused; }
        int GetDayCount() const { return m_dayCount; }

        Vec3 GetSunDirection() const { return m_sunDir; }
        float GetSunIntensity() const { return m_sunIntensity; }
        bool IsNight() const { return m_sunDir.y < 0.0f; }
        bool IsDay() const { return m_sunDir.y >= 0.0f; }

        DayPeriod GetDayPeriod() const
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

        std::string GetTimeString() const
        {
            int h = static_cast<int>(m_hour);
            int m = static_cast<int>((m_hour - h) * 60.0f);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
            return std::string(buf);
        }

      private:
        void UpdateSun()
        {
            float angle = (m_hour / 24.0f) * 2.0f * static_cast<float>(M_PI) - static_cast<float>(M_PI) / 2.0f;
            m_sunDir.x = std::cos(angle);
            m_sunDir.y = std::sin(angle);
            m_sunDir.z = 0.0f;
            float len = std::sqrt(m_sunDir.x * m_sunDir.x + m_sunDir.y * m_sunDir.y);
            if (len > 0.0001f)
            {
                m_sunDir.x /= len;
                m_sunDir.y /= len;
            }
            m_sunIntensity = std::clamp(m_sunDir.y * 2.0f, 0.0f, 1.0f);
        }

        float m_hour = 12.0f;
        float m_timeScale = 60.0f;
        bool m_paused = false;
        int m_dayCount = 0;
        Vec3 m_sunDir = {0, 1, 0};
        float m_sunIntensity = 1.0f;
    };

} // namespace TestDayNight

// =============================================================================
// Tests
// =============================================================================

TEST(DayNight_DefaultIsNoon)
{
    TestDayNight::DayNightCycle cycle;
    EXPECT_NEAR(cycle.GetTimeOfDay(), 12.0f, 0.01f);
    EXPECT_TRUE(cycle.IsDay());
    EXPECT_FALSE(cycle.IsNight());
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Midday);
}

TEST(DayNight_SetTimeOfDay)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(6.0f);
    EXPECT_NEAR(cycle.GetTimeOfDay(), 6.0f, 0.01f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Dawn);

    cycle.SetTimeOfDay(22.0f);
    EXPECT_NEAR(cycle.GetTimeOfDay(), 22.0f, 0.01f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::LateNight);
}

TEST(DayNight_TimeWraps)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(23.0f);
    cycle.SetTimeScale(3600.0f); // 1 real second = 1 game hour

    cycle.Update(2.0f); // Advance 2 hours -> wraps past midnight
    EXPECT_NEAR(cycle.GetTimeOfDay(), 1.0f, 0.01f);
    EXPECT_EQ(cycle.GetDayCount(), 1);
}

TEST(DayNight_SunAtNoon)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(12.0f);
    auto sun = cycle.GetSunDirection();
    // At noon, sun should be near-vertical (y close to 1)
    EXPECT_GT(sun.y, 0.9f);
    EXPECT_GT(cycle.GetSunIntensity(), 0.9f);
}

TEST(DayNight_SunAtMidnight)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(0.0f);
    auto sun = cycle.GetSunDirection();
    // At midnight, sun should be below horizon
    EXPECT_LT(sun.y, 0.0f);
    EXPECT_TRUE(cycle.IsNight());
    EXPECT_NEAR(cycle.GetSunIntensity(), 0.0f, 0.01f);
}

TEST(DayNight_TimeScale)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(12.0f);
    cycle.SetTimeScale(3600.0f); // 1 second = 1 hour

    cycle.Update(1.0f); // 1 second -> advance 1 hour
    EXPECT_NEAR(cycle.GetTimeOfDay(), 13.0f, 0.01f);
}

TEST(DayNight_Pause)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(12.0f);
    cycle.SetPaused(true);
    EXPECT_TRUE(cycle.IsPaused());

    cycle.Update(100.0f);
    EXPECT_NEAR(cycle.GetTimeOfDay(), 12.0f, 0.01f); // Time should not advance
}

TEST(DayNight_DayPeriods)
{
    TestDayNight::DayNightCycle cycle;

    cycle.SetTimeOfDay(2.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Night);

    cycle.SetTimeOfDay(6.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Dawn);

    cycle.SetTimeOfDay(8.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Morning);

    cycle.SetTimeOfDay(12.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Midday);

    cycle.SetTimeOfDay(15.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Afternoon);

    cycle.SetTimeOfDay(18.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Dusk);

    cycle.SetTimeOfDay(20.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::Evening);

    cycle.SetTimeOfDay(22.0f);
    EXPECT_EQ((int)cycle.GetDayPeriod(), (int)TestDayNight::DayPeriod::LateNight);
}

TEST(DayNight_TimeString)
{
    TestDayNight::DayNightCycle cycle;
    cycle.SetTimeOfDay(14.5f); // 14:30
    EXPECT_EQ(cycle.GetTimeString(), std::string("14:30"));

    cycle.SetTimeOfDay(0.0f);
    EXPECT_EQ(cycle.GetTimeString(), std::string("00:00"));

    cycle.SetTimeOfDay(9.25f); // 9:15
    EXPECT_EQ(cycle.GetTimeString(), std::string("09:15"));
}

TEST(DayNight_DayCountIncrements)
{
    TestDayNight::DayNightCycle cycle;
    EXPECT_EQ(cycle.GetDayCount(), 0);

    cycle.SetTimeOfDay(23.5f);
    cycle.SetTimeScale(3600.0f);
    cycle.Update(1.0f); // +1 hour, wraps past midnight
    EXPECT_EQ(cycle.GetDayCount(), 1);
}
