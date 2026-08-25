/**
 * @file OWEngineSystems.h
 * @brief Wires open world gameplay into engine subsystems
 * @author Spark Engine Team
 * @date 2026
 *
 * OWEngineSystems connects open-world-specific data with engine infrastructure:
 * versioned gameplay persistence, wildlife behavior trees, cinematic sequences, biome-driven
 * weather/time-of-day rules, dynamic music, and event bus subscriptions.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"
#include "Persistence/OWSaveData.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OpenWorld
{

    /**
     * @brief Bridges open world game logic with engine subsystems
     *
     * Constructed and owned by SparkGameOpenWorldModule. On Initialize() it
     * connects open-world assets and handlers with save, AI, cinematic,
     * weather, music, and event systems.
     */
    class OWEngineSystems
    {
      public:
        OWEngineSystems() = default;
        ~OWEngineSystems();

        bool Initialize(Spark::IEngineContext* context);
        /// @brief Attach the module-owned gameplay systems that persistence snapshots.
        void BindGameState(OWPlayerSystem& player, OWExplorationSystem& exploration, OWGatheringSystem& gathering,
                           OWSettlementSystem& settlements, OWWildlifeSystem& wildlife, OWDynamicEventSystem& events);
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

        /// @brief Deterministic text codec used by disk persistence and regression tests.
        static std::string SerializeSnapshot(const OWGameSaveData& data);
        static bool DeserializeSnapshot(std::string_view text, OWGameSaveData& outData, std::string& error);

      private:
        void ConfigurePersistence();
        void RegisterBehaviorTrees();
        void RegisterCinematicSequences();
        void SetupWeatherAndTimeOfDay();
        void RegisterMusicTracks();
        void SubscribeToEvents();
        OWGameSaveData CaptureSnapshot() const;
        bool ValidateSnapshot(const OWGameSaveData& data, std::string& error) const;
        bool RestoreSnapshot(const OWGameSaveData& data, std::string& error);
        static bool IsValidSlotName(const std::string& slotName);
        static std::filesystem::path GetModuleSavePath(const std::string& slotName);

        Spark::IEngineContext* m_context = nullptr;
        bool m_initialized = false;
        std::vector<Spark::SubscriptionHandle> m_eventHandles;
        OWPlayerSystem* m_player = nullptr;
        OWExplorationSystem* m_exploration = nullptr;
        OWGatheringSystem* m_gathering = nullptr;
        OWSettlementSystem* m_settlements = nullptr;
        OWWildlifeSystem* m_wildlife = nullptr;
        OWDynamicEventSystem* m_events = nullptr;
    };

} // namespace OpenWorld
