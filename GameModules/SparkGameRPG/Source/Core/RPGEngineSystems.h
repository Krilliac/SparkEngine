/**
 * @file RPGEngineSystems.h
 * @brief Wires RPG gameplay into engine subsystems (save, animation, AI, cinematic, etc.)
 * @author Spark Engine Team
 * @date 2026
 *
 * RPGEngineSystems registers RPG-specific data with engine infrastructure:
 * save serializers, animation state machines, NPC behavior trees, cinematic
 * sequences, weather/time-of-day rules, coroutine tasks, music tracks, and
 * event bus subscriptions.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

namespace RPG
{

    /**
     * @brief Bridges RPG game logic with engine subsystems
     *
     * Constructed and owned by SparkGameRPGModule. On Initialize() it registers
     * RPG-specific assets and handlers with the engine's save, animation, AI,
     * cinematic, weather, coroutine, music, and event systems.
     */
    class RPGEngineSystems
    {
      public:
        RPGEngineSystems() = default;
        ~RPGEngineSystems();

        /**
         * @brief Register all RPG data with engine subsystems
         * @param context  Engine context providing access to all subsystems
         * @return true on success
         */
        bool Initialize(Spark::IEngineContext* context);

        /** @brief Per-frame update for engine system integration (coroutines, music, etc.) */
        void Update(float deltaTime);

        /** @brief Release subscriptions and clean up registrations */
        void Shutdown();

        /** @brief Render debug UI for engine system integrations */
        void RenderDebugUI();

        // ---- Console command helpers ----

        /** @brief Save to a named slot with current RPG state */
        std::string SaveGame(const std::string& slotName);

        /** @brief Load from a named slot */
        std::string LoadGame(const std::string& slotName);

        /** @brief Set weather type by name (clear/rain/snow/fog/storm) */
        std::string SetWeather(const std::string& weatherName);

        /** @brief Set time of day (0-24 hours) */
        std::string SetTime(float hour);

      private:
        void RegisterSaveSerializers();
        void RegisterAnimationStateMachines();
        void RegisterBehaviorTrees();
        void RegisterCinematicSequences();
        void SetupWeatherAndTimeOfDay();
        void RegisterCoroutines();
        void RegisterMusicTracks();
        void SubscribeToEvents();

        Spark::IEngineContext* m_context = nullptr;
        bool m_initialized = false;

        // Event subscription handles (RAII, auto-unsubscribe on destruction)
        std::vector<Spark::SubscriptionHandle> m_eventHandles;
    };

} // namespace RPG
