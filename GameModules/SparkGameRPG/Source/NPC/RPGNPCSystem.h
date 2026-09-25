/**
 * @file RPGNPCSystem.h
 * @brief NPC AI with behavior states, schedules, and disposition
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides NPC definitions with behavior states (idle, patrol, guard, merchant,
 * quest giver), a schedule system where NPCs change behavior by time of day,
 * a disposition system (friendly/neutral/hostile) affected by player actions,
 * and integration points for AI behavior trees.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief A scheduled behavior change for an NPC
    struct NPCScheduleEntry
    {
        float startHour = 0.0f; ///< 0-24 hour of day
        float endHour = 24.0f;
        NPCBehavior behavior = NPCBehavior::Idle;
        float posX = 0.0f; ///< Position during this schedule
        float posY = 0.0f;
        float posZ = 0.0f;
    };

    /// @brief Patrol waypoint for NPCs in patrol behavior
    struct PatrolWaypoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float waitTime = 2.0f; ///< Seconds to wait at waypoint
    };

    /// @brief Definition and live state of an NPC
    struct NPCData
    {
        uint32_t npcId = 0;
        std::string name;
        uint32_t areaId = 0;
        NPCBehavior currentBehavior = NPCBehavior::Idle;
        NPCDisposition disposition = NPCDisposition::Neutral;
        int dispositionValue = 50; ///< 0 = hostile, 50 = neutral, 100 = friendly

        // Position
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;

        // Schedule
        std::vector<NPCScheduleEntry> schedule;

        // Patrol data
        std::vector<PatrolWaypoint> patrolPath;
        int currentWaypointIndex = 0;
        float waypointWaitTimer = 0.0f;

        // Dialogue/quest references
        uint32_t dialogueTreeId = 0; ///< 0 = no dialogue
        uint32_t questId = 0;        ///< Quest offered by this NPC (0 = none)

        // Merchant data
        std::vector<uint32_t> shopItems; ///< Item IDs available for sale
    };

    /// @brief The mutable, save-worthy part of one NPC (definitions come from RegisterDefaultNPCs)
    struct NPCPersistentState
    {
        uint32_t npcId = 0;
        int dispositionValue = 50; ///< 0-100; the disposition tier is derived from it on restore
        NPCBehavior behavior = NPCBehavior::Idle;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        int currentWaypointIndex = 0;
        float waypointWaitTimer = 0.0f;
    };

    /// @brief World clock plus every registered NPC's mutable state, ordered by NPC id
    struct NPCSystemSnapshot
    {
        float worldTime = 0.0f;
        float worldHour = 8.0f;
        std::vector<NPCPersistentState> npcs;
    };

    /**
     * @brief NPC registry, AI behavior, schedules, and disposition tracking
     */
    class RPGNPCSystem
    {
      public:
        RPGNPCSystem() = default;
        ~RPGNPCSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === NPC queries ===
        size_t GetNPCCount() const { return m_npcs.size(); }
        NPCData* GetNPC(uint32_t npcId);
        const NPCData* GetNPC(uint32_t npcId) const;
        std::vector<const NPCData*> GetNPCsInArea(uint32_t areaId) const;
        std::string GetNPCListString() const;

        // === Disposition ===
        void AdjustDisposition(uint32_t npcId, int change);
        NPCDisposition GetDispositionTier(int value) const;

        // === World time for schedules ===
        float GetWorldHour() const { return m_worldHour; }

        // === Persistence ===

        /** @brief Capture the world clock and every NPC's mutable state, sorted by NPC id */
        NPCSystemSnapshot CaptureState() const;

        /**
         * @brief The state a new world starts with: clock at 0 (8:00) and every default NPC as registered
         * @return Snapshot used to migrate saves written before NPC state was persisted
         */
        static NPCSystemSnapshot CaptureDefaultState();

        /**
         * @brief Check a snapshot against the registered NPCs without applying it
         * @return true only if it names every registered NPC exactly once with in-range values
         */
        bool ValidateState(const NPCSystemSnapshot& snapshot) const;

        /**
         * @brief Apply a snapshot; nothing changes unless ValidateState() accepts it
         * @return true if the snapshot was applied
         */
        bool RestoreState(const NPCSystemSnapshot& snapshot);

      private:
        void RegisterDefaultNPCs();
        void UpdateSchedules();
        void UpdatePatrols(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, NPCData> m_npcs;
        float m_worldTime{0.0f};
        float m_worldHour{8.0f}; ///< Start at 8:00 AM

        static constexpr float SECONDS_PER_GAME_HOUR = 60.0f; ///< 1 real minute = 1 game hour
    };

} // namespace RPG
