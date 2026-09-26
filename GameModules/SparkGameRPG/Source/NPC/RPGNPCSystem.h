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
 *
 * When a schedule entry becomes active the NPC walks to its new post along a path
 * from the engine NavMesh (Spark::AI::NavMeshBuilder / NavMeshQuery) of its area;
 * it never teleports. An NPC whose area has no NavMesh, or whose post is unreachable,
 * stays where it is.
 *
 * Contract: game thread only (driven from SparkGameRPGModule::OnUpdate). The system
 * owns its NPC table and one NavMesh plus query per area it navigates. Allocation
 * happens at registration, NavMesh bakes and schedule transitions; the per-frame
 * update of routes, patrols and the clock does not allocate. Scalability: a handful
 * of NPCs per area, one path query per NPC per schedule transition.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"
#include "Engine/AI/NavMeshTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{
    struct RPGAreaInfo;

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

        // Schedule travel (runtime only: rebuilt from the restored position after a load)
        int activeScheduleEntry = -1; ///< Index into schedule of the entry in force; -1 until the next update
        std::vector<XMFLOAT3> route;  ///< NavMesh waypoints toward the active entry's post; empty when arrived
        std::size_t routeIndex = 0;   ///< Next waypoint in route

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
        RPGNPCSystem();
        ~RPGNPCSystem();

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

        // === Navigation ===

        /**
         * @brief Bake a flat ground NavMesh at y = 0 over the XZ bounds of every area that hosts an NPC
         * @param areas The world's areas (RPGWorldSetup::GetAreas())
         * @return false if an NPC's area is missing, has no ground at y = 0, or fails to bake
         */
        bool BuildAreaNavigation(const std::vector<RPGAreaInfo>& areas);

        /**
         * @brief Bake an area's NavMesh from walkable triangles through the engine NavMeshBuilder
         *
         * Replaces the area's previous NavMesh; NPCs of that area drop their routes and replan on the next update.
         * @param areaId Area the NavMesh serves
         * @param vertices Walkable geometry vertices
         * @param indices Triangle list into vertices (counter-clockwise seen from above)
         * @return true if the bake produced at least one walkable triangle
         */
        bool BuildAreaNavMesh(uint32_t areaId, const std::vector<XMFLOAT3>& vertices,
                              const std::vector<uint32_t>& indices);

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
        struct AreaNavigation;

        void RegisterDefaultNPCs();
        void UpdateSchedules();
        void UpdatePatrols(float deltaTime);
        void UpdateRoutes(float deltaTime);
        bool PlanRoute(NPCData& npc, const XMFLOAT3& destination);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, NPCData> m_npcs;
        std::unordered_map<uint32_t, std::unique_ptr<AreaNavigation>> m_areaNavigation; ///< Keyed by area id
        float m_worldTime{0.0f};
        float m_worldHour{8.0f}; ///< Start at 8:00 AM

        static constexpr float SECONDS_PER_GAME_HOUR = 60.0f; ///< 1 real minute = 1 game hour
        static constexpr float WALK_SPEED = 3.0f;             ///< Metres per second on routes and patrols
        static constexpr float ARRIVAL_DISTANCE = 1.0f;       ///< Closer than this counts as already at a post
    };

} // namespace RPG
