// TestFormationSystem.cpp - Tests for formation creation, slot assignment, and world positions
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestFormation
{

    using FormationID = uint32_t;
    using EntityID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    };

    struct FormationSlot
    {
        Vec3 localOffset;
        EntityID assignedEntity = 0;
        bool occupied = false;
    };

    struct Formation
    {
        FormationID id = 0;
        std::string name;
        Vec3 leaderPosition;
        float heading = 0.0f; // radians
        std::vector<FormationSlot> slots;
    };

    struct FormationSystem
    {
        std::unordered_map<FormationID, Formation> formations;
        FormationID nextID = 1;

        FormationID CreateFormation(const std::string& name, Vec3 leaderPos, int slotCount)
        {
            FormationID id = nextID++;
            Formation f{id, name, leaderPos, 0.0f, {}};
            // Default: spread slots in a line along X
            for (int i = 0; i < slotCount; i++)
            {
                float offset = static_cast<float>(i + 1) * 2.0f;
                f.slots.push_back({{offset, 0.0f, 0.0f}, 0, false});
            }
            formations[id] = std::move(f);
            return id;
        }

        bool AssignToSlot(FormationID fid, int slotIndex, EntityID entity)
        {
            auto it = formations.find(fid);
            if (it == formations.end())
                return false;
            auto& f = it->second;
            if (slotIndex < 0 || slotIndex >= static_cast<int>(f.slots.size()))
                return false;
            f.slots[slotIndex].assignedEntity = entity;
            f.slots[slotIndex].occupied = true;
            return true;
        }

        Vec3 GetSlotWorldPosition(FormationID fid, int slotIndex) const
        {
            auto it = formations.find(fid);
            if (it == formations.end())
                return {0, 0, 0};
            const auto& f = it->second;
            if (slotIndex < 0 || slotIndex >= static_cast<int>(f.slots.size()))
                return {0, 0, 0};
            // Simple: leader position + local offset (ignoring rotation for test simplicity)
            return f.leaderPosition + f.slots[slotIndex].localOffset;
        }

        bool DisbandFormation(FormationID fid) { return formations.erase(fid) > 0; }
    };

} // namespace TestFormation

// ============================================================================
// Creation and Disbanding
// ============================================================================

TEST(FormationSystem_CreateFormation)
{
    TestFormation::FormationSystem sys;
    auto fid = sys.CreateFormation("Wedge", {0, 0, 0}, 4);
    EXPECT_TRUE(sys.formations.find(fid) != sys.formations.end());
    EXPECT_EQ(static_cast<int>(sys.formations[fid].slots.size()), 4);
}

TEST(FormationSystem_DisbandFormation)
{
    TestFormation::FormationSystem sys;
    auto fid = sys.CreateFormation("Line", {0, 0, 0}, 3);
    EXPECT_TRUE(sys.DisbandFormation(fid));
    EXPECT_FALSE(sys.DisbandFormation(fid));
    EXPECT_TRUE(sys.formations.empty());
}

// ============================================================================
// Slot Assignment
// ============================================================================

TEST(FormationSystem_AssignToSlot)
{
    TestFormation::FormationSystem sys;
    auto fid = sys.CreateFormation("Column", {0, 0, 0}, 3);

    EXPECT_TRUE(sys.AssignToSlot(fid, 0, 100));
    EXPECT_TRUE(sys.AssignToSlot(fid, 1, 101));
    EXPECT_TRUE(sys.AssignToSlot(fid, 2, 102));

    EXPECT_TRUE(sys.formations[fid].slots[0].occupied);
    EXPECT_EQ(sys.formations[fid].slots[0].assignedEntity, (uint32_t)100);

    // Invalid slot index
    EXPECT_FALSE(sys.AssignToSlot(fid, 5, 103));
    // Invalid formation
    EXPECT_FALSE(sys.AssignToSlot(999, 0, 103));
}

// ============================================================================
// World Position Calculation
// ============================================================================

TEST(FormationSystem_GetSlotWorldPosition)
{
    TestFormation::FormationSystem sys;
    auto fid = sys.CreateFormation("Line", {10, 0, 5}, 2);

    // Slot 0 offset: (2,0,0), slot 1 offset: (4,0,0)
    auto pos0 = sys.GetSlotWorldPosition(fid, 0);
    EXPECT_NEAR(pos0.x, 12.0f, 0.001f);
    EXPECT_NEAR(pos0.z, 5.0f, 0.001f);

    auto pos1 = sys.GetSlotWorldPosition(fid, 1);
    EXPECT_NEAR(pos1.x, 14.0f, 0.001f);
    EXPECT_NEAR(pos1.z, 5.0f, 0.001f);
}
