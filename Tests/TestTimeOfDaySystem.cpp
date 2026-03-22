// TestTimeOfDaySystem.cpp - Tests for time-of-day simulation and sun direction
// Standalone implementation for CI testing (no engine dependency)

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace TestTimeOfDay
{

    enum class DayPeriod
    {
        Night,     // 0:00 - 5:00
        Dawn,      // 5:00 - 7:00
        Morning,   // 7:00 - 10:00
        Midday,    // 10:00 - 14:00
        Afternoon, // 14:00 - 17:00
        Dusk,      // 17:00 - 19:00
        Evening,   // 19:00 - 21:00
        LateNight  // 21:00 - 24:00
    };

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    static Vec3 LerpColor(const Vec3& a, const Vec3& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    class TimeOfDaySystem
    {
      public:
        void Update(float deltaTime)
        {
            if (m_paused)
                return;

            // Convert real seconds to game hours via time scale
            float gameSeconds = deltaTime * m_timeScale;
            float gameHours = gameSeconds / 3600.0f;
            m_hour += gameHours;

            while (m_hour >= 24.0f)
            {
                m_hour -= 24.0f;
                m_dayCount++;
            }

            UpdateSunState();
        }

        void SetTimeOfDay(float hour)
        {
            // Wrap modulo 24
            m_hour = std::fmod(hour, 24.0f);
            if (m_hour < 0.0f)
                m_hour += 24.0f;
            UpdateSunState();
        }

        float GetTimeOfDay() const { return m_hour; }

        void SetTimeScale(float scale) { m_timeScale = std::max(0.0f, scale); }
        float GetTimeScale() const { return m_timeScale; }

        void SetPaused(bool paused) { m_paused = paused; }
        bool IsPaused() const { return m_paused; }

        int GetDayCount() const { return m_dayCount; }

        Vec3 GetSunDirection() const { return m_sunDirection; }
        Vec3 GetSunColor() const { return m_sunColor; }
        float GetSunIntensity() const { return m_sunIntensity; }
        Vec3 GetAmbientColor() const { return m_ambientColor; }
        float GetAmbientIntensity() const { return m_ambientIntensity; }

        bool IsNight() const { return m_sunDirection.y < 0.0f; }
        bool IsDay() const { return m_sunDirection.y >= 0.0f; }

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
            char buf[6];
            std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
            return std::string(buf);
        }

      private:
        void UpdateSunState()
        {
            // Sun arc: rises at 6:00, peaks at 12:00, sets at 18:00
            // Map hour to angle: 6=0 deg (horizon), 12=90 deg (zenith), 18=180 deg (horizon)
            constexpr float pi = 3.14159265358979323846f;
            float sunAngle = ((m_hour - 6.0f) / 12.0f) * pi;

            m_sunDirection.y = std::sin(sunAngle);
            m_sunDirection.x = -std::cos(sunAngle);
            m_sunDirection.z = 0.0f;

            // Sun intensity: positive when above horizon, zero otherwise
            m_sunIntensity = std::max(0.0f, m_sunDirection.y);

            // Sun color: warm at dawn/dusk, white at noon, dark at night
            Vec3 noonColor = {1.0f, 1.0f, 0.95f};
            Vec3 sunsetColor = {1.0f, 0.5f, 0.2f};
            Vec3 nightColor = {0.05f, 0.05f, 0.1f};

            if (m_sunDirection.y > 0.3f)
            {
                m_sunColor = noonColor;
            }
            else if (m_sunDirection.y > 0.0f)
            {
                float t = m_sunDirection.y / 0.3f;
                m_sunColor = LerpColor(sunsetColor, noonColor, t);
            }
            else
            {
                m_sunColor = nightColor;
            }

            // Ambient: brighter during day, dimmer at night
            Vec3 dayAmbient = {0.15f, 0.15f, 0.14f};
            Vec3 nightAmbient = {0.03f, 0.03f, 0.06f};
            float dayFactor = std::clamp(m_sunDirection.y + 0.2f, 0.0f, 1.0f);
            m_ambientColor = LerpColor(nightAmbient, dayAmbient, dayFactor);
            m_ambientIntensity = 0.1f + 0.4f * dayFactor;
        }

        float m_hour = 12.0f;
        float m_timeScale = 60.0f; // 1 real second = 1 game minute
        bool m_paused = false;
        int m_dayCount = 0;

        Vec3 m_sunDirection{0.0f, 1.0f, 0.0f};
        Vec3 m_sunColor{1.0f, 1.0f, 1.0f};
        float m_sunIntensity = 1.0f;
        Vec3 m_ambientColor{0.1f, 0.1f, 0.12f};
        float m_ambientIntensity = 0.3f;
    };

} // namespace TestTimeOfDay

// =============================================================================
// Tests — Time Progression
// =============================================================================

TEST(TimeOfDay_DefaultIsNoon)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    EXPECT_NEAR(tod.GetTimeOfDay(), 12.0f, 0.001f);
    EXPECT_TRUE(tod.IsDay());
    EXPECT_FALSE(tod.IsNight());
}

TEST(TimeOfDay_SetTime)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    tod.SetTimeOfDay(6.0f);
    EXPECT_NEAR(tod.GetTimeOfDay(), 6.0f, 0.001f);

    tod.SetTimeOfDay(23.5f);
    EXPECT_NEAR(tod.GetTimeOfDay(), 23.5f, 0.001f);
}

TEST(TimeOfDay_SetTimeWraps)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    // Values >= 24 should wrap
    tod.SetTimeOfDay(25.0f);
    EXPECT_NEAR(tod.GetTimeOfDay(), 1.0f, 0.001f);

    // Negative values should wrap to positive
    tod.SetTimeOfDay(-2.0f);
    EXPECT_NEAR(tod.GetTimeOfDay(), 22.0f, 0.001f);
}

TEST(TimeOfDay_UpdateAdvancesTime)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(12.0f);
    tod.SetTimeScale(3600.0f); // 1 real second = 1 game hour

    tod.Update(1.0f); // 1 second of real time
    EXPECT_NEAR(tod.GetTimeOfDay(), 13.0f, 0.01f);
}

TEST(TimeOfDay_DayRollover)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(23.0f);
    tod.SetTimeScale(3600.0f); // 1 real second = 1 game hour

    EXPECT_EQ(tod.GetDayCount(), 0);

    // Advance 2 hours — should roll over to 1:00 and increment day count
    tod.Update(2.0f);
    EXPECT_NEAR(tod.GetTimeOfDay(), 1.0f, 0.01f);
    EXPECT_EQ(tod.GetDayCount(), 1);
}

// =============================================================================
// Tests — Sun Direction
// =============================================================================

TEST(TimeOfDay_SunDirectionAtNoon)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(12.0f);

    auto dir = tod.GetSunDirection();
    // At noon, sun should be directly overhead (y near 1.0)
    EXPECT_GT(dir.y, 0.9f);
    EXPECT_NEAR(dir.x, 0.0f, 0.1f);
}

TEST(TimeOfDay_SunDirectionAtDawn)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(6.0f);

    auto dir = tod.GetSunDirection();
    // At 6:00, sun is at the horizon (y near 0)
    EXPECT_NEAR(dir.y, 0.0f, 0.05f);
}

TEST(TimeOfDay_SunDirectionAtMidnight)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(0.0f);

    auto dir = tod.GetSunDirection();
    // At midnight, sun is below horizon (y < 0)
    EXPECT_LT(dir.y, 0.0f);
    EXPECT_TRUE(tod.IsNight());
}

TEST(TimeOfDay_SunIntensityAboveHorizon)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    // At noon — full intensity
    tod.SetTimeOfDay(12.0f);
    EXPECT_GT(tod.GetSunIntensity(), 0.8f);

    // At midnight — zero intensity
    tod.SetTimeOfDay(0.0f);
    EXPECT_NEAR(tod.GetSunIntensity(), 0.0f, 0.01f);
}

// =============================================================================
// Tests — Day Period Transitions
// =============================================================================

TEST(TimeOfDay_NightPeriod)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    tod.SetTimeOfDay(2.0f);
    EXPECT_EQ(static_cast<int>(tod.GetDayPeriod()), static_cast<int>(TestTimeOfDay::DayPeriod::Night));

    tod.SetTimeOfDay(4.99f);
    EXPECT_EQ(static_cast<int>(tod.GetDayPeriod()), static_cast<int>(TestTimeOfDay::DayPeriod::Night));
}

TEST(TimeOfDay_DawnPeriod)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    tod.SetTimeOfDay(5.0f);
    EXPECT_EQ(static_cast<int>(tod.GetDayPeriod()), static_cast<int>(TestTimeOfDay::DayPeriod::Dawn));

    tod.SetTimeOfDay(6.5f);
    EXPECT_EQ(static_cast<int>(tod.GetDayPeriod()), static_cast<int>(TestTimeOfDay::DayPeriod::Dawn));
}

TEST(TimeOfDay_MiddayPeriod)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    tod.SetTimeOfDay(12.0f);
    EXPECT_EQ(static_cast<int>(tod.GetDayPeriod()), static_cast<int>(TestTimeOfDay::DayPeriod::Midday));
}

TEST(TimeOfDay_AllPeriodsSequential)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    // Verify each boundary transitions to the expected period
    float hours[] = {2.0f, 5.5f, 8.0f, 12.0f, 15.0f, 18.0f, 20.0f, 22.0f};
    TestTimeOfDay::DayPeriod expected[] = {
        TestTimeOfDay::DayPeriod::Night,     TestTimeOfDay::DayPeriod::Dawn,
        TestTimeOfDay::DayPeriod::Morning,   TestTimeOfDay::DayPeriod::Midday,
        TestTimeOfDay::DayPeriod::Afternoon, TestTimeOfDay::DayPeriod::Dusk,
        TestTimeOfDay::DayPeriod::Evening,   TestTimeOfDay::DayPeriod::LateNight,
    };

    for (int i = 0; i < 8; ++i)
    {
        tod.SetTimeOfDay(hours[i]);
        EXPECT_EQ(static_cast<int>(tod.GetDayPeriod()), static_cast<int>(expected[i]));
    }
}

// =============================================================================
// Tests — Pause and Resume
// =============================================================================

TEST(TimeOfDay_PausePreventsUpdate)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(12.0f);
    tod.SetTimeScale(3600.0f);

    tod.SetPaused(true);
    EXPECT_TRUE(tod.IsPaused());

    tod.Update(5.0f); // 5 seconds — would normally advance 5 hours
    EXPECT_NEAR(tod.GetTimeOfDay(), 12.0f, 0.001f);
}

TEST(TimeOfDay_ResumeAfterPause)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(10.0f);
    tod.SetTimeScale(3600.0f);

    tod.SetPaused(true);
    tod.Update(1.0f); // No change while paused

    tod.SetPaused(false);
    EXPECT_FALSE(tod.IsPaused());

    tod.Update(1.0f); // Should now advance 1 hour
    EXPECT_NEAR(tod.GetTimeOfDay(), 11.0f, 0.01f);
}

TEST(TimeOfDay_ZeroTimeScaleFreezes)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeOfDay(15.0f);
    tod.SetTimeScale(0.0f);

    tod.Update(10.0f);
    EXPECT_NEAR(tod.GetTimeOfDay(), 15.0f, 0.001f);
}

// =============================================================================
// Tests — Time String Formatting
// =============================================================================

TEST(TimeOfDay_TimeString)
{
    TestTimeOfDay::TimeOfDaySystem tod;

    tod.SetTimeOfDay(14.5f);
    EXPECT_EQ(tod.GetTimeString(), std::string("14:30"));

    tod.SetTimeOfDay(0.0f);
    EXPECT_EQ(tod.GetTimeString(), std::string("00:00"));

    tod.SetTimeOfDay(23.75f);
    EXPECT_EQ(tod.GetTimeString(), std::string("23:45"));
}

TEST(TimeOfDay_TimeScaleClampedNonNegative)
{
    TestTimeOfDay::TimeOfDaySystem tod;
    tod.SetTimeScale(-100.0f);
    EXPECT_GE(tod.GetTimeScale(), 0.0f);
}
