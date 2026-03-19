// TestGroupAI.cpp - Tests for AI group creation, membership, threat reporting, and tactics
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TestGroupAI
{

    using GroupID = uint32_t;
    using EntityID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    enum class GroupTactic : uint8_t
    {
        Idle,
        Patrol,
        Attack,
        Defend,
        Retreat,
        Flank
    };

    struct ThreatInfo
    {
        EntityID threatEntity;
        Vec3 lastKnownPosition;
        float threatLevel = 0.0f;
    };

    struct AIGroup
    {
        GroupID id = 0;
        std::string name;
        std::unordered_set<EntityID> members;
        GroupTactic tactic = GroupTactic::Idle;
        std::vector<ThreatInfo> threats;
    };

    struct GroupAISystem
    {
        std::unordered_map<GroupID, AIGroup> groups;
        GroupID nextID = 1;

        GroupID CreateGroup(const std::string& name)
        {
            GroupID id = nextID++;
            groups[id] = {id, name, {}, GroupTactic::Idle, {}};
            return id;
        }

        bool AddMember(GroupID gid, EntityID entity)
        {
            auto it = groups.find(gid);
            if (it == groups.end())
                return false;
            it->second.members.insert(entity);
            return true;
        }

        bool RemoveMember(GroupID gid, EntityID entity)
        {
            auto it = groups.find(gid);
            if (it == groups.end())
                return false;
            return it->second.members.erase(entity) > 0;
        }

        void ReportThreat(GroupID gid, EntityID threatEntity, Vec3 pos, float level)
        {
            auto it = groups.find(gid);
            if (it == groups.end())
                return;
            // Update existing or add new
            for (auto& t : it->second.threats)
            {
                if (t.threatEntity == threatEntity)
                {
                    t.lastKnownPosition = pos;
                    t.threatLevel = level;
                    return;
                }
            }
            it->second.threats.push_back({threatEntity, pos, level});
        }

        bool SetTactic(GroupID gid, GroupTactic tactic)
        {
            auto it = groups.find(gid);
            if (it == groups.end())
                return false;
            it->second.tactic = tactic;
            return true;
        }

        GroupID FindGroupForEntity(EntityID entity) const
        {
            for (const auto& [gid, group] : groups)
            {
                if (group.members.count(entity) > 0)
                    return gid;
            }
            return 0;
        }
    };

} // namespace TestGroupAI

// ============================================================================
// Group Creation and Membership
// ============================================================================

TEST(GroupAI_CreateGroup)
{
    TestGroupAI::GroupAISystem sys;
    auto gid = sys.CreateGroup("Squad Alpha");
    EXPECT_TRUE(sys.groups.find(gid) != sys.groups.end());
    EXPECT_EQ(sys.groups[gid].name, std::string("Squad Alpha"));
}

TEST(GroupAI_AddMember)
{
    TestGroupAI::GroupAISystem sys;
    auto gid = sys.CreateGroup("Squad");
    EXPECT_TRUE(sys.AddMember(gid, 10));
    EXPECT_TRUE(sys.AddMember(gid, 20));
    EXPECT_EQ(static_cast<int>(sys.groups[gid].members.size()), 2);
    EXPECT_FALSE(sys.AddMember(999, 10));
}

TEST(GroupAI_FindGroupForEntity)
{
    TestGroupAI::GroupAISystem sys;
    auto g1 = sys.CreateGroup("Alpha");
    auto g2 = sys.CreateGroup("Bravo");
    sys.AddMember(g1, 10);
    sys.AddMember(g2, 20);

    EXPECT_EQ(sys.FindGroupForEntity(10), g1);
    EXPECT_EQ(sys.FindGroupForEntity(20), g2);
    EXPECT_EQ(sys.FindGroupForEntity(99), (uint32_t)0);
}

// ============================================================================
// Threat Reporting
// ============================================================================

TEST(GroupAI_ReportThreat)
{
    TestGroupAI::GroupAISystem sys;
    auto gid = sys.CreateGroup("Squad");
    sys.ReportThreat(gid, 50, {10, 0, 5}, 8.0f);
    EXPECT_EQ(static_cast<int>(sys.groups[gid].threats.size()), 1);
    EXPECT_NEAR(sys.groups[gid].threats[0].threatLevel, 8.0f, 0.001f);

    // Update existing threat
    sys.ReportThreat(gid, 50, {12, 0, 6}, 9.0f);
    EXPECT_EQ(static_cast<int>(sys.groups[gid].threats.size()), 1);
    EXPECT_NEAR(sys.groups[gid].threats[0].threatLevel, 9.0f, 0.001f);
}

// ============================================================================
// Tactics
// ============================================================================

TEST(GroupAI_SetTactic)
{
    TestGroupAI::GroupAISystem sys;
    auto gid = sys.CreateGroup("Squad");
    EXPECT_EQ(static_cast<int>(sys.groups[gid].tactic), static_cast<int>(TestGroupAI::GroupTactic::Idle));

    EXPECT_TRUE(sys.SetTactic(gid, TestGroupAI::GroupTactic::Flank));
    EXPECT_EQ(static_cast<int>(sys.groups[gid].tactic), static_cast<int>(TestGroupAI::GroupTactic::Flank));

    EXPECT_FALSE(sys.SetTactic(999, TestGroupAI::GroupTactic::Attack));
}
