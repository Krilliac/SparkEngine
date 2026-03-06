/**
 * @file WeatherSystem.h
 * @brief Dynamic weather simulation with rain, snow, fog, and storm effects
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a complete weather system that simulates different weather conditions
 * with smooth transitions. Integrates with the particle system for precipitation,
 * the lighting system for atmosphere changes, and post-processing for fog.
 *
 * ## Usage
 * @code
 *   Spark::WeatherSystem weather;
 *   weather.SetWeather(Spark::WeatherType::Rain, 0.8f, 5.0f); // Rain at 80%, 5s transition
 *   weather.Update(deltaTime);
 *
 *   // Query current state for rendering
 *   auto& state = weather.GetCurrentState();
 *   fogDensity = state.fogDensity;
 *   ambientMod = state.ambientMultiplier;
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
using namespace DirectX;
#else
struct XMFLOAT3 { float x, y, z; };
struct XMFLOAT4 { float x, y, z, w; };
#endif

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>

#include "../Engine/Events/EventSystem.h"
#include "FogSystem.h"

namespace Spark {

// =============================================================================
// Weather Types
// =============================================================================

/**
 * @brief Available weather conditions
 */
enum class WeatherType {
    Clear,      ///< Clear sky, no precipitation
    Cloudy,     ///< Overcast sky, no precipitation
    Rain,       ///< Rainfall with wet surfaces
    Snow,       ///< Snowfall with ground accumulation
    Fog,        ///< Dense fog reducing visibility
    Storm,      ///< Heavy rain with lightning and wind
    Count
};

/**
 * @brief Wind direction presets
 */
enum class WindDirection {
    North,
    South,
    East,
    West,
    NorthEast,
    NorthWest,
    SouthEast,
    SouthWest
};

// =============================================================================
// Weather State
// =============================================================================

/**
 * @brief Complete weather state describing current atmospheric conditions
 *
 * This struct captures all parameters needed to render weather effects.
 * The WeatherSystem interpolates between two states during transitions.
 */
struct WeatherState {
    WeatherType type = WeatherType::Clear;

    // Precipitation
    float intensity = 0.0f;             ///< Overall weather intensity [0, 1]
    float precipitationRate = 0.0f;     ///< Particles per second for rain/snow
    float precipitationSize = 1.0f;     ///< Scale of precipitation particles

    // Wind
    XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f};
    float windSpeed = 0.0f;             ///< Wind speed in m/s
    float windGustiness = 0.0f;         ///< Random wind variation [0, 1]

    // Atmosphere
    float fogDensity = 0.0f;            ///< Volumetric fog density [0, 1]
    float fogStartDistance = 50.0f;     ///< Fog start distance in meters
    float fogEndDistance = 500.0f;      ///< Fog end distance in meters
    XMFLOAT4 fogColor = {0.7f, 0.7f, 0.8f, 1.0f};

    // Lighting modifiers
    float ambientMultiplier = 1.0f;     ///< Multiplier for ambient light intensity
    float directionalMultiplier = 1.0f; ///< Multiplier for directional light
    XMFLOAT4 skyTint = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Color tint applied to skybox

    // Storm-specific
    float lightningFrequency = 0.0f;    ///< Average lightning flashes per minute
    float thunderDelay = 2.0f;          ///< Seconds between lightning and thunder

    // Surface effects
    float wetness = 0.0f;              ///< Surface wetness for rain [0, 1]
    float snowCoverage = 0.0f;         ///< Snow ground coverage [0, 1]
};

// =============================================================================
// Weather Preset Definitions
// =============================================================================

/**
 * @brief Get a default weather state for a given weather type
 */
inline WeatherState GetWeatherPreset(WeatherType type) {
    WeatherState state;
    state.type = type;

    switch (type) {
        case WeatherType::Clear:
            state.intensity = 0.0f;
            state.ambientMultiplier = 1.0f;
            state.directionalMultiplier = 1.0f;
            state.fogDensity = 0.0f;
            state.skyTint = {1.0f, 1.0f, 1.0f, 1.0f};
            break;

        case WeatherType::Cloudy:
            state.intensity = 0.3f;
            state.ambientMultiplier = 0.7f;
            state.directionalMultiplier = 0.5f;
            state.fogDensity = 0.05f;
            state.skyTint = {0.8f, 0.8f, 0.85f, 1.0f};
            break;

        case WeatherType::Rain:
            state.intensity = 0.7f;
            state.precipitationRate = 500.0f;
            state.precipitationSize = 0.8f;
            state.windSpeed = 5.0f;
            state.windGustiness = 0.3f;
            state.fogDensity = 0.1f;
            state.ambientMultiplier = 0.5f;
            state.directionalMultiplier = 0.3f;
            state.skyTint = {0.6f, 0.6f, 0.7f, 1.0f};
            state.fogColor = {0.5f, 0.5f, 0.6f, 1.0f};
            state.wetness = 0.8f;
            break;

        case WeatherType::Snow:
            state.intensity = 0.6f;
            state.precipitationRate = 300.0f;
            state.precipitationSize = 1.5f;
            state.windSpeed = 3.0f;
            state.windGustiness = 0.2f;
            state.fogDensity = 0.15f;
            state.ambientMultiplier = 0.8f;
            state.directionalMultiplier = 0.6f;
            state.skyTint = {0.9f, 0.9f, 0.95f, 1.0f};
            state.fogColor = {0.85f, 0.85f, 0.9f, 1.0f};
            state.snowCoverage = 0.7f;
            break;

        case WeatherType::Fog:
            state.intensity = 0.8f;
            state.fogDensity = 0.6f;
            state.fogStartDistance = 10.0f;
            state.fogEndDistance = 100.0f;
            state.ambientMultiplier = 0.6f;
            state.directionalMultiplier = 0.2f;
            state.skyTint = {0.7f, 0.7f, 0.75f, 1.0f};
            state.fogColor = {0.75f, 0.75f, 0.8f, 1.0f};
            break;

        case WeatherType::Storm:
            state.intensity = 1.0f;
            state.precipitationRate = 1000.0f;
            state.precipitationSize = 1.2f;
            state.windSpeed = 15.0f;
            state.windGustiness = 0.8f;
            state.fogDensity = 0.2f;
            state.ambientMultiplier = 0.3f;
            state.directionalMultiplier = 0.1f;
            state.skyTint = {0.4f, 0.4f, 0.5f, 1.0f};
            state.fogColor = {0.4f, 0.4f, 0.5f, 1.0f};
            state.lightningFrequency = 8.0f;
            state.thunderDelay = 3.0f;
            state.wetness = 1.0f;
            break;

        default:
            break;
    }

    return state;
}

// =============================================================================
// Weather System
// =============================================================================

/**
 * @class WeatherSystem
 * @brief Manages weather state, transitions, and effect parameters
 *
 * The weather system smoothly interpolates between weather states over a
 * configurable transition duration. It outputs rendering parameters that
 * other systems (lighting, particles, post-processing) can consume.
 */
class WeatherSystem {
public:
    using WeatherCallback = std::function<void(WeatherType oldType, WeatherType newType)>;

    WeatherSystem() {
        m_currentState = GetWeatherPreset(WeatherType::Clear);
        m_targetState = m_currentState;
    }

    /**
     * @brief Set a new weather condition with transition
     *
     * @param type           Target weather type
     * @param intensity      Weather intensity override [0, 1] (negative = use preset default)
     * @param transitionTime Seconds to transition to new weather
     */
    void SetWeather(WeatherType type, float intensity = -1.0f, float transitionTime = 3.0f) {
        m_previousState = m_currentState;
        m_targetState = GetWeatherPreset(type);

        if (intensity >= 0.0f) {
            m_targetState.intensity = std::clamp(intensity, 0.0f, 1.0f);
            // Scale precipitation by intensity
            auto preset = GetWeatherPreset(type);
            m_targetState.precipitationRate = preset.precipitationRate * m_targetState.intensity;
            m_targetState.fogDensity = preset.fogDensity * m_targetState.intensity;
            m_targetState.wetness = preset.wetness * m_targetState.intensity;
            m_targetState.snowCoverage = preset.snowCoverage * m_targetState.intensity;
        }

        m_transitionDuration = std::max(transitionTime, 0.01f);
        m_transitionProgress = 0.0f;
        m_isTransitioning = true;
    }

    /**
     * @brief Update the weather system
     * @param dt Delta time in seconds
     */
    void Update(float dt) {
        if (m_isTransitioning) {
            m_transitionProgress += dt / m_transitionDuration;
            if (m_transitionProgress >= 1.0f) {
                m_transitionProgress = 1.0f;
                m_isTransitioning = false;
                m_currentState = m_targetState;

                // Fire callback
                if (m_onWeatherChanged) {
                    m_onWeatherChanged(m_previousState.type, m_currentState.type);
                }

                // Publish event via EventBus
                if (m_eventBus) {
                    WeatherChangedEvent evt;
                    evt.previousType = static_cast<int>(m_previousState.type);
                    evt.newType = static_cast<int>(m_currentState.type);
                    evt.intensity = m_currentState.intensity;
                    m_eventBus->Publish(evt);
                }
            } else {
                // Smooth interpolation (ease in-out)
                float t = SmoothStep(m_transitionProgress);
                m_currentState = LerpState(m_previousState, m_targetState, t);
            }
        }

        // Update wind gusts
        m_windTimer += dt;
        if (m_currentState.windGustiness > 0.0f) {
            float gust = std::sin(m_windTimer * 2.3f) * 0.5f + std::sin(m_windTimer * 5.7f) * 0.3f;
            m_currentWindGust = gust * m_currentState.windGustiness * m_currentState.windSpeed;
        } else {
            m_currentWindGust = 0.0f;
        }

        // Sync fog system with current weather fog parameters
        if (m_fogSystem) {
            m_fogSystem->SetDensity(m_currentState.fogDensity);
            m_fogSystem->SetLinearRange(m_currentState.fogStartDistance, m_currentState.fogEndDistance);
            m_fogSystem->SetColor({
                m_currentState.fogColor.x, m_currentState.fogColor.y,
                m_currentState.fogColor.z, m_currentState.fogColor.w
            });
            if (m_currentState.fogDensity > 0.0f) {
                m_fogSystem->SetEnabled(true);
                if (m_fogSystem->GetMode() == Graphics::FogMode::None)
                    m_fogSystem->SetMode(Graphics::FogMode::Exponential);
            } else {
                m_fogSystem->SetEnabled(false);
            }
        }

        // Update lightning timer for storms
        if (m_currentState.lightningFrequency > 0.0f) {
            m_lightningTimer += dt;
            float interval = 60.0f / m_currentState.lightningFrequency;
            if (m_lightningTimer >= interval) {
                m_lightningTimer -= interval;
                m_lightningFlash = 1.0f;
            }
            // Decay flash
            if (m_lightningFlash > 0.0f) {
                m_lightningFlash = std::max(0.0f, m_lightningFlash - dt * 8.0f);
            }
        }
    }

    // ---- Accessors ----

    /** @brief Get the current interpolated weather state. */
    const WeatherState& GetCurrentState() const { return m_currentState; }

    /** @brief Get the target weather state (what we're transitioning to). */
    const WeatherState& GetTargetState() const { return m_targetState; }

    /** @brief Check if a weather transition is in progress. */
    bool IsTransitioning() const { return m_isTransitioning; }

    /** @brief Get the transition progress [0, 1]. */
    float GetTransitionProgress() const { return m_transitionProgress; }

    /** @brief Get the current effective wind speed including gusts. */
    float GetEffectiveWindSpeed() const {
        return m_currentState.windSpeed + m_currentWindGust;
    }

    /** @brief Get current lightning flash intensity [0, 1]. */
    float GetLightningFlash() const { return m_lightningFlash; }

    /** @brief Set wind direction and speed directly. */
    void SetWind(float x, float y, float z, float speed) {
        m_currentState.windDirection = {x, y, z};
        m_currentState.windSpeed = speed;
        m_targetState.windDirection = {x, y, z};
        m_targetState.windSpeed = speed;
    }

    /** @brief Set intensity of the current weather [0, 1]. */
    void SetIntensity(float intensity) {
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        m_currentState.intensity = intensity;
        m_targetState.intensity = intensity;
    }

    /** @brief Register a callback for weather change events. */
    void SetOnWeatherChanged(WeatherCallback callback) {
        m_onWeatherChanged = std::move(callback);
    }

    /** @brief Set an EventBus for publishing WeatherChangedEvent. */
    void SetEventBus(EventBus* bus) { m_eventBus = bus; }

    /** @brief Set a FogSystem to automatically sync fog parameters with weather. */
    void SetFogSystem(Graphics::FogSystem* fog) { m_fogSystem = fog; }

    /** @brief Get a string name for a weather type. */
    static const char* GetWeatherTypeName(WeatherType type) {
        switch (type) {
            case WeatherType::Clear:  return "Clear";
            case WeatherType::Cloudy: return "Cloudy";
            case WeatherType::Rain:   return "Rain";
            case WeatherType::Snow:   return "Snow";
            case WeatherType::Fog:    return "Fog";
            case WeatherType::Storm:  return "Storm";
            default:                  return "Unknown";
        }
    }

private:
    static float SmoothStep(float t) {
        return t * t * (3.0f - 2.0f * t);
    }

    static float Lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    static XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
        return { Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t) };
    }

    static XMFLOAT4 LerpFloat4(const XMFLOAT4& a, const XMFLOAT4& b, float t) {
        return { Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), Lerp(a.w, b.w, t) };
    }

    static WeatherState LerpState(const WeatherState& a, const WeatherState& b, float t) {
        WeatherState result;
        result.type = (t < 0.5f) ? a.type : b.type;
        result.intensity = Lerp(a.intensity, b.intensity, t);
        result.precipitationRate = Lerp(a.precipitationRate, b.precipitationRate, t);
        result.precipitationSize = Lerp(a.precipitationSize, b.precipitationSize, t);
        result.windDirection = LerpFloat3(a.windDirection, b.windDirection, t);
        result.windSpeed = Lerp(a.windSpeed, b.windSpeed, t);
        result.windGustiness = Lerp(a.windGustiness, b.windGustiness, t);
        result.fogDensity = Lerp(a.fogDensity, b.fogDensity, t);
        result.fogStartDistance = Lerp(a.fogStartDistance, b.fogStartDistance, t);
        result.fogEndDistance = Lerp(a.fogEndDistance, b.fogEndDistance, t);
        result.fogColor = LerpFloat4(a.fogColor, b.fogColor, t);
        result.ambientMultiplier = Lerp(a.ambientMultiplier, b.ambientMultiplier, t);
        result.directionalMultiplier = Lerp(a.directionalMultiplier, b.directionalMultiplier, t);
        result.skyTint = LerpFloat4(a.skyTint, b.skyTint, t);
        result.lightningFrequency = Lerp(a.lightningFrequency, b.lightningFrequency, t);
        result.thunderDelay = Lerp(a.thunderDelay, b.thunderDelay, t);
        result.wetness = Lerp(a.wetness, b.wetness, t);
        result.snowCoverage = Lerp(a.snowCoverage, b.snowCoverage, t);
        return result;
    }

    WeatherState m_currentState;
    WeatherState m_previousState;
    WeatherState m_targetState;

    bool m_isTransitioning = false;
    float m_transitionDuration = 3.0f;
    float m_transitionProgress = 0.0f;

    float m_windTimer = 0.0f;
    float m_currentWindGust = 0.0f;

    float m_lightningTimer = 0.0f;
    float m_lightningFlash = 0.0f;

    WeatherCallback m_onWeatherChanged;

    EventBus* m_eventBus = nullptr;
    Graphics::FogSystem* m_fogSystem = nullptr;
};

} // namespace Spark
