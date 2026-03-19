/**
 * @file GroupAI.h
 * @brief Group/squad AI coordination system
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Manages groups of AI entities that share threat intelligence and act under
 * coordinated tactics. Each group has a leader, members with assigned roles,
 * a shared threat list, and a current tactic that drives collective behavior.
 *
 * ## Architecture
 * ```
 * GroupAISystem (singleton)
 *   ├── m_groups          (active groups, keyed by ID)
 *   ├── m_entityToGroup   (reverse lookup: entity → group ID)
 *   └── Update(dt)
 *         ├── DecayThreats()      → remove stale threats
 *         ├── UpdateAlertLevels() → compute from active threats
 *         └── AutoAssignRoles()   → fill vacancies (leader dies, etc.)
 * ```
 *
 * ## Integration with FormationSystem
 * Each AIGroup can optionally reference a FormationSystem formation via
 * `formationID`. When set, the group tactic influences formation shape
 * (e.g., Flank → Wedge, HoldPosition → Line).
 *
 * ## Usage
 * @code
 *   auto& gs = Spark::AI::GroupAISystem::GetInstance();
 *   gs.Initialize();
 *
 *   uint32_t gid = gs.CreateGroup(leaderEntity);
 *   gs.AddMember(gid, soldierA, GroupRole::Flanker);
 *   gs.AddMember(gid, soldierB, GroupRole::Support);
 *
 *   gs.ReportThreat(gid, {playerEntity, {10,0,5}, 0.0f, 0.9f});
 *   gs.SetTactic(gid, GroupTactic::Flank);
 *
 *   // Each frame:
 *   gs.Update(deltaTime);
 * @endcode
 *
 * @see FormationSystem.h (formation shapes linked to groups)
 * @see AISystem.h (per-entity behavior trees)
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::AI
{

    // =========================================================================
    // Group Enums
    // =========================================================================

    /** @brief Roles that members can fill within a group. */
    enum class GroupRole : uint8_t
    {
        Leader,  ///< Group commander — drives formation center
        Flanker, ///< Attacks from the sides
        Support, ///< Provides covering fire or healing
        Sniper,  ///< Long-range specialist
        Scout    ///< Recon and early warning
    };

    /** @brief High-level group tactics that coordinate member behavior. */
    enum class GroupTactic : uint8_t
    {
        Idle,         ///< No active tactic — members act independently
        Advance,      ///< Move toward objective as a group
        HoldPosition, ///< Defend current location
        Flank,        ///< Split and attack from multiple angles
        Retreat,      ///< Fall back to a safe position
        Surround      ///< Encircle target from all directions
    };

    // =========================================================================
    // Group Data Structures
    // =========================================================================

    /** @brief A member of an AI group with a role assignment. */
    struct GroupMember
    {
        uint32_t entityID = 0;
        GroupRole role = GroupRole::Flanker;
        bool alive = true;
    };

    /**
     * @brief Information about a known threat shared across the group.
     *
     * Threats decay over time — if not refreshed, they are eventually removed
     * from the group's awareness.
     */
    struct ThreatInfo
    {
        uint32_t entityID = 0;                        ///< Threat entity
        XMFLOAT3 lastKnownPosition{0.0f, 0.0f, 0.0f}; ///< Last observed position
        float lastSeenTime = 0.0f;                    ///< Game time when last observed
        float threatLevel = 0.0f;                     ///< Severity (0 = negligible, 1 = critical)
    };

    /**
     * @brief A coordinated group of AI entities.
     *
     * Groups share threat intelligence so that all members react to threats
     * spotted by any single member. The current tactic determines collective
     * behavior, and the optional formationID links to a FormationSystem formation.
     */
    struct AIGroup
    {
        uint32_t id = 0;
        std::vector<GroupMember> members;
        std::vector<ThreatInfo> knownThreats;
        GroupTactic currentTactic = GroupTactic::Idle;
        uint32_t formationID = 0; ///< Linked FormationSystem formation (0 = none)
        float alertLevel = 0.0f;  ///< 0 = calm, 1 = full combat alert
    };

    // =========================================================================
    // GroupAISystem
    // =========================================================================

    /**
     * @class GroupAISystem
     * @brief Singleton that manages AI squad coordination and threat sharing.
     *
     * Provides group lifecycle, member management, threat reporting, and
     * per-frame updates that decay stale threats and adjust alert levels.
     */
    class GroupAISystem
    {
      public:
        /** @brief Get the singleton instance. */
        static GroupAISystem& GetInstance()
        {
            static GroupAISystem s;
            return s;
        }

        /** @brief Initialize the system. */
        void Initialize();

        /** @brief Shut down and disband all groups. */
        void Shutdown();

        /**
         * @brief Create a new AI group with a leader.
         * @param leaderEntityID Entity that leads the group.
         * @return Unique group ID.
         */
        uint32_t CreateGroup(uint32_t leaderEntityID);

        /**
         * @brief Disband and remove a group.
         * @param groupID Group to disband.
         */
        void DisbandGroup(uint32_t groupID);

        /**
         * @brief Add a member to a group.
         * @param groupID  Group to add to.
         * @param entityID Entity joining the group.
         * @param role     Role assignment for this member.
         */
        void AddMember(uint32_t groupID, uint32_t entityID, GroupRole role);

        /**
         * @brief Remove a member from a group.
         * @param groupID  Group to remove from.
         * @param entityID Entity leaving the group.
         */
        void RemoveMember(uint32_t groupID, uint32_t entityID);

        /**
         * @brief Report a threat to the group's shared awareness.
         * @param groupID Group to inform.
         * @param threat  Threat information to share.
         */
        void ReportThreat(uint32_t groupID, const ThreatInfo& threat);

        /**
         * @brief Set the active tactic for a group.
         * @param groupID Group to update.
         * @param tactic  New tactic to adopt.
         */
        void SetTactic(uint32_t groupID, GroupTactic tactic);

        /**
         * @brief Update all groups: decay threats, update alert levels, auto-assign roles.
         * @param deltaTime Frame time in seconds.
         */
        void Update(float deltaTime);

        /**
         * @brief Get a group by ID.
         * @param groupID Group ID.
         * @return Pointer to the group, or nullptr if not found.
         */
        AIGroup* GetGroup(uint32_t groupID);

        /**
         * @brief Get a group by ID (const).
         * @param groupID Group ID.
         * @return Const pointer to the group, or nullptr if not found.
         */
        const AIGroup* GetGroup(uint32_t groupID) const;

        /**
         * @brief Find which group an entity belongs to.
         * @param entityID Entity to search for.
         * @return Group ID, or 0 if the entity is not in any group.
         */
        uint32_t FindGroupForEntity(uint32_t entityID) const;

        /** @brief Get the number of active groups. */
        size_t GetGroupCount() const { return m_groups.size(); }

      private:
        GroupAISystem() = default;

        /** @brief Promote a new leader if the current one is dead or removed. */
        void AutoPromoteLeader(AIGroup& group);

        std::unordered_map<uint32_t, AIGroup> m_groups;
        std::unordered_map<uint32_t, uint32_t> m_entityToGroup; ///< entity → group ID
        uint32_t m_nextID = 1;
        float m_gameTime = 0.0f;
        bool m_initialized = false;
    };

} // namespace Spark::AI
