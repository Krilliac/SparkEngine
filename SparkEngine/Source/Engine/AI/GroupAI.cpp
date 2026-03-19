/**
 * @file GroupAI.cpp
 * @brief Implementation of the CryEngine-inspired group/squad AI coordination system
 */

#include "GroupAI.h"

#include <algorithm>
#include <cmath>

namespace Spark::AI
{

    // =========================================================================
    // Constants
    // =========================================================================

    /// Threats older than this (in seconds since last seen) are removed
    static constexpr float kThreatDecayTime = 15.0f;

    /// Alert level decay rate per second when no threats exist
    static constexpr float kAlertDecayRate = 0.2f;

    /// Alert level ramp rate per second when threats exist
    static constexpr float kAlertRampRate = 0.5f;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void GroupAISystem::Initialize()
    {
        m_groups.clear();
        m_entityToGroup.clear();
        m_nextID = 1;
        m_gameTime = 0.0f;
        m_initialized = true;
    }

    void GroupAISystem::Shutdown()
    {
        m_groups.clear();
        m_entityToGroup.clear();
        m_initialized = false;
    }

    // =========================================================================
    // Group Lifecycle
    // =========================================================================

    uint32_t GroupAISystem::CreateGroup(uint32_t leaderEntityID)
    {
        if (!m_initialized)
        {
            return 0;
        }

        uint32_t id = m_nextID++;

        AIGroup group;
        group.id = id;

        GroupMember leader;
        leader.entityID = leaderEntityID;
        leader.role = GroupRole::Leader;
        leader.alive = true;
        group.members.push_back(leader);

        m_groups[id] = std::move(group);
        m_entityToGroup[leaderEntityID] = id;

        return id;
    }

    void GroupAISystem::DisbandGroup(uint32_t groupID)
    {
        auto it = m_groups.find(groupID);
        if (it == m_groups.end())
        {
            return;
        }

        // Remove reverse lookups for all members
        for (const auto& member : it->second.members)
        {
            m_entityToGroup.erase(member.entityID);
        }

        m_groups.erase(it);
    }

    // =========================================================================
    // Member Management
    // =========================================================================

    void GroupAISystem::AddMember(uint32_t groupID, uint32_t entityID, GroupRole role)
    {
        auto it = m_groups.find(groupID);
        if (it == m_groups.end())
        {
            return;
        }

        // Check if already a member
        for (const auto& member : it->second.members)
        {
            if (member.entityID == entityID)
            {
                return;
            }
        }

        GroupMember member;
        member.entityID = entityID;
        member.role = role;
        member.alive = true;
        it->second.members.push_back(member);

        m_entityToGroup[entityID] = groupID;
    }

    void GroupAISystem::RemoveMember(uint32_t groupID, uint32_t entityID)
    {
        auto it = m_groups.find(groupID);
        if (it == m_groups.end())
        {
            return;
        }

        auto& members = it->second.members;
        auto memberIt = std::find_if(members.begin(), members.end(),
                                     [entityID](const GroupMember& m) { return m.entityID == entityID; });

        if (memberIt != members.end())
        {
            bool wasLeader = (memberIt->role == GroupRole::Leader);
            members.erase(memberIt);
            m_entityToGroup.erase(entityID);

            // If the leader was removed, promote a replacement
            if (wasLeader && !members.empty())
            {
                AutoPromoteLeader(it->second);
            }
        }
    }

    // =========================================================================
    // Threat Management
    // =========================================================================

    void GroupAISystem::ReportThreat(uint32_t groupID, const ThreatInfo& threat)
    {
        auto it = m_groups.find(groupID);
        if (it == m_groups.end())
        {
            return;
        }

        auto& threats = it->second.knownThreats;

        // Update existing threat if already tracked
        for (auto& existing : threats)
        {
            if (existing.entityID == threat.entityID)
            {
                existing.lastKnownPosition = threat.lastKnownPosition;
                existing.lastSeenTime = m_gameTime;
                existing.threatLevel = std::max(existing.threatLevel, threat.threatLevel);
                return;
            }
        }

        // Add new threat with current game time
        ThreatInfo newThreat = threat;
        newThreat.lastSeenTime = m_gameTime;
        threats.push_back(newThreat);
    }

    // =========================================================================
    // Tactic Management
    // =========================================================================

    void GroupAISystem::SetTactic(uint32_t groupID, GroupTactic tactic)
    {
        auto it = m_groups.find(groupID);
        if (it == m_groups.end())
        {
            return;
        }
        it->second.currentTactic = tactic;
    }

    // =========================================================================
    // Update
    // =========================================================================

    void GroupAISystem::Update(float deltaTime)
    {
        if (!m_initialized)
        {
            return;
        }

        m_gameTime += deltaTime;

        for (auto& [groupID, group] : m_groups)
        {
            // --- Decay old threats ---
            auto& threats = group.knownThreats;
            threats.erase(std::remove_if(threats.begin(), threats.end(), [this](const ThreatInfo& t)
                                         { return (m_gameTime - t.lastSeenTime) > kThreatDecayTime; }),
                          threats.end());

            // --- Update alert level ---
            if (threats.empty())
            {
                group.alertLevel = std::max(0.0f, group.alertLevel - kAlertDecayRate * deltaTime);
            }
            else
            {
                // Alert ramps toward the highest threat level
                float maxThreat = 0.0f;
                for (const auto& t : threats)
                {
                    maxThreat = std::max(maxThreat, t.threatLevel);
                }
                group.alertLevel = std::min(1.0f, group.alertLevel + kAlertRampRate * deltaTime);
                group.alertLevel = std::max(group.alertLevel, maxThreat * 0.5f);
            }

            // --- Auto-promote leader if needed ---
            bool hasLeader = false;
            for (const auto& member : group.members)
            {
                if (member.role == GroupRole::Leader && member.alive)
                {
                    hasLeader = true;
                    break;
                }
            }
            if (!hasLeader && !group.members.empty())
            {
                AutoPromoteLeader(group);
            }
        }
    }

    // =========================================================================
    // Queries
    // =========================================================================

    AIGroup* GroupAISystem::GetGroup(uint32_t groupID)
    {
        auto it = m_groups.find(groupID);
        return (it != m_groups.end()) ? &it->second : nullptr;
    }

    const AIGroup* GroupAISystem::GetGroup(uint32_t groupID) const
    {
        auto it = m_groups.find(groupID);
        return (it != m_groups.end()) ? &it->second : nullptr;
    }

    uint32_t GroupAISystem::FindGroupForEntity(uint32_t entityID) const
    {
        auto it = m_entityToGroup.find(entityID);
        return (it != m_entityToGroup.end()) ? it->second : 0;
    }

    // =========================================================================
    // Internal
    // =========================================================================

    void GroupAISystem::AutoPromoteLeader(AIGroup& group)
    {
        if (group.members.empty())
        {
            return;
        }

        // Prefer alive members; pick the first alive non-leader
        for (auto& member : group.members)
        {
            if (member.alive)
            {
                member.role = GroupRole::Leader;
                return;
            }
        }

        // No alive members — promote the first one anyway
        group.members[0].role = GroupRole::Leader;
    }

} // namespace Spark::AI
