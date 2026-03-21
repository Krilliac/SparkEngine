/**
 * @file WeatherGameplayIntegration.cpp
 * @brief Bridges weather events to physics wind, AI perception, and audio state
 */

#include "WeatherGameplayIntegration.h"

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
#include "../../Physics/PhysicsSystem.h"
#endif

#include <algorithm>
#include <cmath>

namespace Spark::Gameplay
{

    // =========================================================================
    // WeatherPerceptionModifier
    // =========================================================================

    float WeatherPerceptionModifier::GetVisibilityMultiplier() const
    {
        // Fog and storm both reduce visibility. Combine them multiplicatively.
        // Minimum visibility is 20% so AI isn't completely blind.
        float fogFactor = 1.0f - (fogIntensity * 0.7f);
        float stormFactor = 1.0f - (stormIntensity * 0.5f);
        return std::max(0.2f, fogFactor * stormFactor);
    }

    // =========================================================================
    // WeatherGameplayIntegration — Singleton
    // =========================================================================

    WeatherGameplayIntegration& WeatherGameplayIntegration::GetInstance()
    {
        static WeatherGameplayIntegration s;
        return s;
    }

    void WeatherGameplayIntegration::Initialize(EventBus* eventBus)
    {
        if (m_initialized)
            return;

        if (eventBus)
        {
            m_weatherSubscription =
                eventBus->Subscribe<WeatherChangedEvent>([this](const WeatherChangedEvent& e) { OnWeatherChanged(e); });
        }

        m_perceptionModifier = {};
        m_audioState = {};
        m_initialized = true;
    }

    void WeatherGameplayIntegration::Shutdown()
    {
        m_weatherSubscription.Unsubscribe();
        m_perceptionModifier = {};
        m_audioState = {};
        m_initialized = false;
    }

    void WeatherGameplayIntegration::Update(float /*dt*/, const WeatherSystem* weather, PhysicsSystem* physics)
    {
        if (!m_initialized || !weather)
            return;

        UpdateWindForces(weather, physics);
        UpdatePerception(weather);
        UpdateAudio(weather);
    }

    // =========================================================================
    // Event Handler
    // =========================================================================

    void WeatherGameplayIntegration::OnWeatherChanged(const WeatherChangedEvent& /*event*/)
    {
        // The continuous Update() loop handles all state transitions smoothly.
        // This handler exists for discrete one-shot reactions if needed in the future.
    }

    // =========================================================================
    // Wind Forces
    // =========================================================================

    void WeatherGameplayIntegration::UpdateWindForces(const WeatherSystem* weather, PhysicsSystem* physics)
    {
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
        if (!physics)
            return;

        const auto& state = weather->GetCurrentState();
        if (state.windSpeed < 0.1f)
            return;

        // Wind force = direction * speed * mass-independent scaling factor
        float effectiveSpeed = weather->GetEffectiveWindSpeed();
        XMFLOAT3 windForce = {state.windDirection.x * effectiveSpeed, state.windDirection.y * effectiveSpeed,
                              state.windDirection.z * effectiveSpeed};

        // Apply wind force to all dynamic bodies
        const auto& bodies = physics->GetBodies();
        for (const auto& body : bodies)
        {
            if (!body || body->GetType() != PhysicsBodyType::Dynamic)
                continue;

            // Scale force by body mass so lighter objects are affected more
            float mass = body->GetMass();
            if (mass <= 0.0f)
                continue;

            // Light objects (< 10kg) get full wind force, heavier objects get less
            float massScale = std::min(1.0f, 10.0f / mass);
            XMFLOAT3 scaledForce = {windForce.x * massScale, windForce.y * massScale, windForce.z * massScale};
            body->ApplyForce(scaledForce);
        }
#else
        (void)weather;
        (void)physics;
#endif
    }

    // =========================================================================
    // AI Perception
    // =========================================================================

    void WeatherGameplayIntegration::UpdatePerception(const WeatherSystem* weather)
    {
        const auto& state = weather->GetCurrentState();

        m_perceptionModifier.fogIntensity = state.fogDensity;
        m_perceptionModifier.stormIntensity = (state.type == WeatherType::Storm) ? state.intensity : 0.0f;
    }

    // =========================================================================
    // Audio State
    // =========================================================================

    void WeatherGameplayIntegration::UpdateAudio(const WeatherSystem* weather)
    {
        const auto& state = weather->GetCurrentState();

        bool hasWeather = (state.type != WeatherType::Clear);
        m_audioState.isActive = hasWeather;

        // Rain volume: proportional to precipitation rate for rain/storm
        if (state.type == WeatherType::Rain || state.type == WeatherType::Storm)
        {
            m_audioState.rainVolume = std::clamp(state.precipitationRate / 1000.0f, 0.0f, 1.0f);
        }
        else
        {
            m_audioState.rainVolume = 0.0f;
        }

        // Thunder volume: tied to lightning frequency during storms
        if (state.type == WeatherType::Storm)
        {
            m_audioState.thunderVolume = std::clamp(state.lightningFrequency / 10.0f, 0.0f, 1.0f);
        }
        else
        {
            m_audioState.thunderVolume = 0.0f;
        }

        // Wind volume: proportional to wind speed (any weather type)
        m_audioState.windVolume = std::clamp(state.windSpeed / 15.0f, 0.0f, 1.0f);

        // Snow ambient: soft ambient during snowfall
        if (state.type == WeatherType::Snow)
        {
            m_audioState.snowVolume = state.intensity * 0.5f;
        }
        else
        {
            m_audioState.snowVolume = 0.0f;
        }
    }

} // namespace Spark::Gameplay
