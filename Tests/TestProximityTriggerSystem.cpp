// TestProximityTriggerSystem.cpp - Tests for proximity trigger creation, removal, and enter/exit
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TestProxTrigger
{

    using TriggerID = uint32_t;
    using EntityID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        float DistanceTo(const Vec3& other) const
        {
            float dx = x - other.x, dy = y - other.y, dz = z - other.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    };

    struct ProximityTrigger
    {
        TriggerID id = 0;
        Vec3 position;
        float radius = 1.0f;
        std::unordered_set<EntityID> entitiesInside;
    };

    struct TriggerEvent
    {
        TriggerID triggerID;
        EntityID entityID;
        bool entered; // true = enter, false = exit
    };

    struct ProximityTriggerSystem
    {
        std::unordered_map<TriggerID, ProximityTrigger> triggers;
        std::vector<TriggerEvent> events;
        TriggerID nextID = 1;

        TriggerID CreateTrigger(Vec3 pos, float radius)
        {
            TriggerID id = nextID++;
            triggers[id] = {id, pos, radius, {}};
            return id;
        }

        bool RemoveTrigger(TriggerID id) { return triggers.erase(id) > 0; }

        void Update(const std::unordered_map<EntityID, Vec3>& entityPositions)
        {
            events.clear();
            for (auto& [tid, trigger] : triggers)
            {
                std::unordered_set<EntityID> nowInside;
                for (const auto& [eid, pos] : entityPositions)
                {
                    if (pos.DistanceTo(trigger.position) <= trigger.radius)
                        nowInside.insert(eid);
                }

                // Detect enters
                for (EntityID eid : nowInside)
                {
                    if (trigger.entitiesInside.find(eid) == trigger.entitiesInside.end())
                        events.push_back({tid, eid, true});
                }
                // Detect exits
                for (EntityID eid : trigger.entitiesInside)
                {
                    if (nowInside.find(eid) == nowInside.end())
                        events.push_back({tid, eid, false});
                }

                trigger.entitiesInside = std::move(nowInside);
            }
        }
    };

} // namespace TestProxTrigger

// ============================================================================
// Creation and Removal
// ============================================================================

TEST(ProximityTrigger_CreateAndRemove)
{
    TestProxTrigger::ProximityTriggerSystem sys;
    auto id1 = sys.CreateTrigger({0, 0, 0}, 5.0f);
    auto id2 = sys.CreateTrigger({10, 0, 0}, 3.0f);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(static_cast<int>(sys.triggers.size()), 2);

    EXPECT_TRUE(sys.RemoveTrigger(id1));
    EXPECT_EQ(static_cast<int>(sys.triggers.size()), 1);
    EXPECT_FALSE(sys.RemoveTrigger(id1));
}

// ============================================================================
// Enter and Exit Detection
// ============================================================================

TEST(ProximityTrigger_EnterEvent)
{
    TestProxTrigger::ProximityTriggerSystem sys;
    auto tid = sys.CreateTrigger({0, 0, 0}, 5.0f);

    // Entity outside
    std::unordered_map<TestProxTrigger::EntityID, TestProxTrigger::Vec3> positions;
    positions[1] = {20, 0, 0};
    sys.Update(positions);
    EXPECT_EQ(static_cast<int>(sys.events.size()), 0);

    // Entity moves inside
    positions[1] = {2, 0, 0};
    sys.Update(positions);
    EXPECT_EQ(static_cast<int>(sys.events.size()), 1);
    EXPECT_TRUE(sys.events[0].entered);
    EXPECT_EQ(sys.events[0].triggerID, tid);
}

TEST(ProximityTrigger_ExitEvent)
{
    TestProxTrigger::ProximityTriggerSystem sys;
    sys.CreateTrigger({0, 0, 0}, 5.0f);

    std::unordered_map<TestProxTrigger::EntityID, TestProxTrigger::Vec3> positions;
    // Enter first
    positions[1] = {2, 0, 0};
    sys.Update(positions);
    EXPECT_EQ(static_cast<int>(sys.events.size()), 1);
    EXPECT_TRUE(sys.events[0].entered);

    // Stay inside — no new events
    sys.Update(positions);
    EXPECT_EQ(static_cast<int>(sys.events.size()), 0);

    // Exit
    positions[1] = {20, 0, 0};
    sys.Update(positions);
    EXPECT_EQ(static_cast<int>(sys.events.size()), 1);
    EXPECT_FALSE(sys.events[0].entered);
}

TEST(ProximityTrigger_MultipleEntities)
{
    TestProxTrigger::ProximityTriggerSystem sys;
    sys.CreateTrigger({0, 0, 0}, 5.0f);

    std::unordered_map<TestProxTrigger::EntityID, TestProxTrigger::Vec3> positions;
    positions[1] = {1, 0, 0};
    positions[2] = {2, 0, 0};
    positions[3] = {100, 0, 0};
    sys.Update(positions);

    // Two entities should have entered
    int enterCount = 0;
    for (const auto& evt : sys.events)
        if (evt.entered)
            enterCount++;
    EXPECT_EQ(enterCount, 2);
}
