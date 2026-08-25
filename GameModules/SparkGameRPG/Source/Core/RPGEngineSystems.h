/**
 * @file RPGEngineSystems.h
 * @brief Wires RPG gameplay into save, AI, cinematic, environment, audio, and event systems
 * @author Spark Engine Team
 * @date 2026
 *
 * RPGEngineSystems registers RPG-specific data with engine infrastructure:
 * save configuration, NPC behavior trees, cinematic sequences, weather/time-of-day
 * rules, music tracks, and event bus subscriptions.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <functional>
#include <string>
#include <vector>

namespace RPG
{

    /**
     * @brief Bridges RPG game logic with engine subsystems
     *
     * Constructed and owned by SparkGameRPGModule. On Initialize() it registers
     * RPG-specific assets and handlers with the engine's save, AI, cinematic,
     * weather, music, and event systems.
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

        /** @brief Release subscriptions and clean up registrations */
        void Shutdown();

        // ---- Console command helpers ----

        /** @brief Save to a named slot with current RPG state */
        std::string SaveGame(const std::string& slotName, const std::string& demoState);

        /** @brief Load from a named slot */
        std::string LoadGame(const std::string& slotName, std::string& outDemoState,
                             const std::function<bool(const std::string&)>& validateDemoState);

        /** @brief Set weather type by name (clear/rain/snow/fog/storm) */
        std::string SetWeather(const std::string& weatherName);

        /** @brief Set time of day (0-24 hours) */
        std::string SetTime(float hour);

      private:
        void ConfigureSaveSystem();
        void RegisterBehaviorTrees();
        void RegisterCinematicSequences();
        void SetupWeatherAndTimeOfDay();
        void RegisterMusicTracks();
        void SubscribeToEvents();

        Spark::IEngineContext* m_context = nullptr;
        bool m_initialized = false;

        // Event subscription handles (RAII, auto-unsubscribe on destruction)
        std::vector<Spark::SubscriptionHandle> m_eventHandles;
    };

} // namespace RPG
