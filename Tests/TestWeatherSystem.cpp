// TestWeatherSystem.cpp - Tests for weather simulation and transitions
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <algorithm>
#include <string>

namespace TestWeather {

enum class WeatherType { Clear, Cloudy, Rain, Snow, Fog, Storm, Count };

struct WeatherState {
    WeatherType type = WeatherType::Clear;
    float intensity = 0.0f;
    float precipitationRate = 0.0f;
    float fogDensity = 0.0f;
    float ambientMultiplier = 1.0f;
    float wetness = 0.0f;
    float snowCoverage = 0.0f;
    float windSpeed = 0.0f;
};

inline WeatherState GetPreset(WeatherType type) {
    WeatherState s;
    s.type = type;
    switch (type) {
        case WeatherType::Clear:
            s.intensity = 0.0f; s.ambientMultiplier = 1.0f; s.fogDensity = 0.0f;
            break;
        case WeatherType::Rain:
            s.intensity = 0.7f; s.precipitationRate = 500.0f; s.fogDensity = 0.1f;
            s.ambientMultiplier = 0.5f; s.wetness = 0.8f; s.windSpeed = 5.0f;
            break;
        case WeatherType::Snow:
            s.intensity = 0.6f; s.precipitationRate = 300.0f; s.fogDensity = 0.15f;
            s.ambientMultiplier = 0.8f; s.snowCoverage = 0.7f;
            break;
        case WeatherType::Storm:
            s.intensity = 1.0f; s.precipitationRate = 1000.0f; s.fogDensity = 0.2f;
            s.ambientMultiplier = 0.3f; s.wetness = 1.0f; s.windSpeed = 15.0f;
            break;
        default: break;
    }
    return s;
}

static float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }
static float Lerp(float a, float b, float t) { return a + (b - a) * t; }

class WeatherSystem {
public:
    WeatherSystem() {
        m_current = GetPreset(WeatherType::Clear);
        m_target = m_current;
    }

    void SetWeather(WeatherType type, float intensity = -1.0f, float transitionTime = 3.0f) {
        m_previous = m_current;
        m_target = GetPreset(type);
        if (intensity >= 0.0f) {
            m_target.intensity = std::clamp(intensity, 0.0f, 1.0f);
            auto preset = GetPreset(type);
            m_target.precipitationRate = preset.precipitationRate * m_target.intensity;
            m_target.fogDensity = preset.fogDensity * m_target.intensity;
        }
        m_transitionDuration = std::max(transitionTime, 0.01f);
        m_progress = 0.0f;
        m_transitioning = true;
    }

    void Update(float dt) {
        if (m_transitioning) {
            m_progress += dt / m_transitionDuration;
            if (m_progress >= 1.0f) {
                m_progress = 1.0f;
                m_transitioning = false;
                m_current = m_target;
            } else {
                float t = SmoothStep(m_progress);
                m_current.intensity = Lerp(m_previous.intensity, m_target.intensity, t);
                m_current.precipitationRate = Lerp(m_previous.precipitationRate, m_target.precipitationRate, t);
                m_current.fogDensity = Lerp(m_previous.fogDensity, m_target.fogDensity, t);
                m_current.ambientMultiplier = Lerp(m_previous.ambientMultiplier, m_target.ambientMultiplier, t);
                m_current.type = (t < 0.5f) ? m_previous.type : m_target.type;
            }
        }
    }

    const WeatherState& GetCurrentState() const { return m_current; }
    bool IsTransitioning() const { return m_transitioning; }
    float GetProgress() const { return m_progress; }

    static const char* GetTypeName(WeatherType t) {
        switch (t) {
            case WeatherType::Clear: return "Clear";
            case WeatherType::Cloudy: return "Cloudy";
            case WeatherType::Rain: return "Rain";
            case WeatherType::Snow: return "Snow";
            case WeatherType::Fog: return "Fog";
            case WeatherType::Storm: return "Storm";
            default: return "Unknown";
        }
    }

private:
    WeatherState m_current, m_previous, m_target;
    bool m_transitioning = false;
    float m_transitionDuration = 3.0f;
    float m_progress = 0.0f;
};

} // namespace TestWeather

// =============================================================================
// Tests
// =============================================================================

TEST(Weather_DefaultIsClear) {
    TestWeather::WeatherSystem ws;
    auto& state = ws.GetCurrentState();
    EXPECT_EQ((int)state.type, (int)TestWeather::WeatherType::Clear);
    EXPECT_NEAR(state.intensity, 0.0f, 0.001f);
    EXPECT_NEAR(state.ambientMultiplier, 1.0f, 0.001f);
}

TEST(Weather_SetWeatherTransition) {
    TestWeather::WeatherSystem ws;
    ws.SetWeather(TestWeather::WeatherType::Rain, -1.0f, 1.0f);

    EXPECT_TRUE(ws.IsTransitioning());

    // Midway through transition
    ws.Update(0.5f);
    EXPECT_TRUE(ws.IsTransitioning());
    auto& mid = ws.GetCurrentState();
    EXPECT_GT(mid.precipitationRate, 0.0f);
    EXPECT_LT(mid.precipitationRate, 500.0f);

    // Complete transition
    ws.Update(0.6f);
    EXPECT_FALSE(ws.IsTransitioning());
    auto& final_ = ws.GetCurrentState();
    EXPECT_EQ((int)final_.type, (int)TestWeather::WeatherType::Rain);
    EXPECT_NEAR(final_.precipitationRate, 500.0f, 0.1f);
}

TEST(Weather_IntensityOverride) {
    TestWeather::WeatherSystem ws;
    ws.SetWeather(TestWeather::WeatherType::Rain, 0.5f, 0.01f);
    ws.Update(0.1f); // Complete transition

    auto& state = ws.GetCurrentState();
    EXPECT_NEAR(state.intensity, 0.5f, 0.01f);
    // Precipitation should be scaled
    EXPECT_NEAR(state.precipitationRate, 250.0f, 1.0f);
}

TEST(Weather_PresetValues) {
    auto rain = TestWeather::GetPreset(TestWeather::WeatherType::Rain);
    EXPECT_NEAR(rain.precipitationRate, 500.0f, 0.1f);
    EXPECT_NEAR(rain.wetness, 0.8f, 0.01f);
    EXPECT_GT(rain.windSpeed, 0.0f);

    auto snow = TestWeather::GetPreset(TestWeather::WeatherType::Snow);
    EXPECT_NEAR(snow.snowCoverage, 0.7f, 0.01f);
    EXPECT_NEAR(snow.precipitationRate, 300.0f, 0.1f);
}

TEST(Weather_StormHighIntensity) {
    auto storm = TestWeather::GetPreset(TestWeather::WeatherType::Storm);
    EXPECT_NEAR(storm.intensity, 1.0f, 0.01f);
    EXPECT_NEAR(storm.precipitationRate, 1000.0f, 0.1f);
    EXPECT_GT(storm.windSpeed, 10.0f);
}

TEST(Weather_NoTransitionWhenNotSet) {
    TestWeather::WeatherSystem ws;
    EXPECT_FALSE(ws.IsTransitioning());
    ws.Update(1.0f);
    EXPECT_FALSE(ws.IsTransitioning());
}

TEST(Weather_TypeName) {
    EXPECT_EQ(std::string(TestWeather::WeatherSystem::GetTypeName(TestWeather::WeatherType::Rain)),
              std::string("Rain"));
    EXPECT_EQ(std::string(TestWeather::WeatherSystem::GetTypeName(TestWeather::WeatherType::Storm)),
              std::string("Storm"));
}

TEST(Weather_SmoothStepBounds) {
    EXPECT_NEAR(TestWeather::SmoothStep(0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(TestWeather::SmoothStep(1.0f), 1.0f, 0.001f);
    EXPECT_NEAR(TestWeather::SmoothStep(0.5f), 0.5f, 0.001f);
    // SmoothStep should be between 0 and 1 for inputs in [0, 1]
    EXPECT_GT(TestWeather::SmoothStep(0.3f), 0.0f);
    EXPECT_LT(TestWeather::SmoothStep(0.3f), 0.3f); // SmoothStep < linear for t < 0.5
}
