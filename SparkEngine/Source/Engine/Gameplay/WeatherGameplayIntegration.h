/**
 * @file WeatherGameplayIntegration.h
 * @brief Bridges weather system events to gameplay mechanics (physics, AI, audio)
 * @author Spark Engine Team
 * @date 2026
 *
 * Subscribes to WeatherChangedEvent via EventBus and propagates effects:
 * - Wind forces on dynamic physics bodies
 * - AI perception modifiers (fog/storm reduce visibility)
 * - Ambient weather audio state tracking
 *
 * @see WeatherSystem.h, EventBus.h, PhysicsSystem.h
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/EventBus.h"
#include "../../Engine/Events/EventSystem.h"
#include "../../Graphics/WeatherSystem.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <string>

class PhysicsSystem;

namespace Spark::Gameplay
{

    /**
     * @brief Modifier that adjusts AI perception based on weather conditions.
     *
     * Fog and storms reduce visibility. Clear weather has no effect.
     * Query GetVisibilityMultiplier() to scale AI sight ranges.
     */
    struct WeatherPerceptionModifier
    {
        float fogIntensity = 0.0f;   ///< Current fog density [0, 1]
        float stormIntensity = 0.0f; ///< Current storm intensity [0, 1]

        /**
         * @brief Get the visibility multiplier for AI sight range.
         * @return Multiplier in [0.2, 1.0] — lower in fog/storms.
         */
        float GetVisibilityMultiplier() const;
    };

    /**
     * @brief Tracks ambient weather audio state with volume levels.
     *
     * Stores desired volume levels for weather sound layers.
     * The audio system reads these to control playback.
     */
    struct WeatherAudioState
    {
        float rainVolume = 0.0f;    ///< Rain loop volume [0, 1]
        float thunderVolume = 0.0f; ///< Thunder volume [0, 1]
        float windVolume = 0.0f;    ///< Wind ambient volume [0, 1]
        float snowVolume = 0.0f;    ///< Snow ambient volume [0, 1]
        bool isActive = false;      ///< Whether any weather audio should play
    };

    /**
     * @class WeatherGameplayIntegration
     * @brief Singleton that bridges weather events to gameplay systems.
     *
     * Call Initialize() at startup with an EventBus to begin receiving
     * weather change events. Call Update() each frame to apply continuous
     * wind forces to physics bodies and refresh audio/perception state.
     */
    class WeatherGameplayIntegration
    {
      public:
        static WeatherGameplayIntegration& GetInstance();

        /**
         * @brief Initialize and subscribe to weather events.
         * @param eventBus EventBus to subscribe to (must outlive this object).
         */
        void Initialize(EventBus* eventBus);

        /**
         * @brief Per-frame update: applies wind forces and refreshes state.
         * @param dt Delta time in seconds.
         * @param weather Current weather system (may be null).
         * @param physics Current physics system (may be null).
         */
        void Update(float dt, const WeatherSystem* weather, PhysicsSystem* physics);

        /** @brief Unsubscribe from events and reset state. */
        void Shutdown();

        /** @brief Get the current AI perception modifier. */
        const WeatherPerceptionModifier& GetPerceptionModifier() const { return m_perceptionModifier; }

        /** @brief Get the current weather audio state. */
        const WeatherAudioState& GetAudioState() const { return m_audioState; }

      private:
        WeatherGameplayIntegration() = default;

        void OnWeatherChanged(const WeatherChangedEvent& event);
        void UpdateWindForces(const WeatherSystem* weather, PhysicsSystem* physics);
        void UpdatePerception(const WeatherSystem* weather);
        void UpdateAudio(const WeatherSystem* weather);

        SubscriptionHandle m_weatherSubscription;
        WeatherPerceptionModifier m_perceptionModifier;
        WeatherAudioState m_audioState;
        bool m_initialized = false;
    };

} // namespace Spark::Gameplay
