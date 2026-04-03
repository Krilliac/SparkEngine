/**
 * @file OWEngineSystems.h
 * @brief Wires open world gameplay into engine subsystems
 * @author Spark Engine Team
 * @date 2026
 *
 * OWEngineSystems registers open-world-specific data with engine infrastructure:
 * save serializers, wildlife behavior trees, cinematic sequences, biome-driven
 * weather/time-of-day rules, dynamic music, and event bus subscriptions.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

namespace OpenWorld
{

    /**
     * @brief Bridges open world game logic with engine subsystems
     *
     * Constructed and owned by SparkGameOpenWorldModule. On Initialize() it
     * registers open-world assets and handlers with save, AI, cinematic,
     * weather, music, and event systems.
     */
    class OWEngineSystems
    {
      public:
        OWEngineSystems() = default;
        ~OWEngineSystems();

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Console command helpers
        std::string SaveGame(const std::string& slotName);
        std::string LoadGame(const std::string& slotName);
        std::string SetWeather(const std::string& weatherName);
        std::string SetTime(float hour);
        std::string GetWeatherStatus() const;
        std::string GetAbilitySummary() const;

      private:
        void RegisterSaveSerializers();
        void RegisterBehaviorTrees();
        void RegisterCinematicSequences();
        void SetupWeatherAndTimeOfDay();
        void RegisterMusicTracks();
        void SubscribeToEvents();

        Spark::IEngineContext* m_context = nullptr;
        bool m_initialized = false;
        std::vector<Spark::SubscriptionHandle> m_eventHandles;
    };

} // namespace OpenWorld
