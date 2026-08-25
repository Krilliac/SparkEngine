/**
 * @file OWDynamicEventSystem.h
 * @brief Dynamic world events, random encounters, and emergent gameplay
 * @author Spark Engine Team
 * @date 2026
 *
 * Spawns time-limited world events that create emergent gameplay: bandit raids
 * on settlements, wildlife stampedes, merchant caravans to escort, weather-
 * driven disasters, and mythical creature sightings.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenWorld
{

    /// @brief Template defining a world event type
    struct WorldEventTemplate
    {
        uint32_t templateId = 0;
        std::string name;
        std::string description;
        WorldEventType type = WorldEventType::BanditRaid;
        float duration = 120.0f; ///< Seconds the event lasts
        float cooldown = 600.0f; ///< Minimum seconds between occurrences
        int minDangerLevel = 1;  ///< Region must be at least this dangerous
        uint32_t xpReward = 100;
        std::vector<Biome> validBiomes; ///< Biomes where this event can spawn
    };

    /// @brief Runtime state for an active world event
    struct ActiveWorldEvent
    {
        uint32_t eventId = 0;
        uint32_t templateId = 0;
        std::string name;
        WorldEventType type = WorldEventType::BanditRaid;
        EventState state = EventState::Dormant;
        float posX = 0.0f;
        float posZ = 0.0f;
        uint32_t regionId = 0;
        float timeRemaining = 0.0f;
        float totalDuration = 0.0f;
        bool playerParticipating = false;
    };

    /// @brief Mutable dynamic-event state stored in an OpenWorld save.
    struct DynamicEventSaveState
    {
        std::vector<ActiveWorldEvent> activeEvents;
        std::vector<std::pair<uint32_t, float>> cooldowns;
        uint32_t nextEventId = 1;
        float eventCheckTimer = 0.0f;
        uint32_t totalEventsCompleted = 0;
    };

    /**
     * @brief Dynamic world event spawning and lifecycle management
     */
    class OWDynamicEventSystem
    {
      public:
        OWDynamicEventSystem() = default;
        ~OWDynamicEventSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime, float playerX, float playerZ, uint32_t playerRegionId);
        void Shutdown();
        void RenderDebugUI();

        size_t GetTemplateCount() const { return m_templates.size(); }
        size_t GetActiveEventCount() const { return m_activeEvents.size(); }

        /// @brief Force-spawn a specific event type in a region
        uint32_t TriggerEvent(uint32_t templateId, uint32_t regionId, float x, float z);

        /// @brief Player joins an active event
        bool JoinEvent(uint32_t eventId);

        DynamicEventSaveState CaptureSaveState() const;
        bool RestoreSaveState(const DynamicEventSaveState& state, std::string* error = nullptr);

        std::string GetEventListString() const;
        std::string GetActiveEventsString() const;

      private:
        void DefineEventTemplates();
        void CheckForNewEvents(float playerX, float playerZ, uint32_t playerRegionId);
        void UpdateActiveEvents(float deltaTime, float playerX, float playerZ);
        void CompleteEvent(uint32_t eventId);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<WorldEventTemplate> m_templates;
        std::unordered_map<uint32_t, ActiveWorldEvent> m_activeEvents;
        std::unordered_map<uint32_t, float> m_cooldowns; ///< templateId -> remaining cooldown
        uint32_t m_nextEventId{1};
        float m_eventCheckTimer{0.0f};
        uint32_t m_totalEventsCompleted{0};
        bool m_initialized{false};
    };

} // namespace OpenWorld
