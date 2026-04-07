// TestCollisionAvoidance.cpp - Tests for agent registration, velocity solving, and separation
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace TestCollAvoid
{

    using AgentID = uint32_t;

    struct Vec2
    {
        float x = 0.0f, y = 0.0f;

        float Length() const { return std::sqrt(x * x + y * y); }
        Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
        Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
        Vec2 operator*(float s) const { return {x * s, y * s}; }

        float DistanceTo(const Vec2& o) const { return (o - *this).Length(); }
    };

    struct Agent
    {
        AgentID id = 0;
        Vec2 position;
        Vec2 preferredVelocity;
        Vec2 resolvedVelocity;
        float radius = 0.5f;
        float maxSpeed = 5.0f;
    };

    struct CollisionAvoidanceSystem
    {
        std::unordered_map<AgentID, Agent> agents;
        AgentID nextID = 1;

        AgentID RegisterAgent(Vec2 pos, float radius, float maxSpeed)
        {
            AgentID id = nextID++;
            agents[id] = {id, pos, {0, 0}, {0, 0}, radius, maxSpeed};
            return id;
        }

        bool UnregisterAgent(AgentID id) { return agents.erase(id) > 0; }

        bool SetPreferredVelocity(AgentID id, Vec2 vel)
        {
            auto it = agents.find(id);
            if (it == agents.end())
                return false;
            it->second.preferredVelocity = vel;
            return true;
        }

        void Solve()
        {
            // Simple repulsion-based collision avoidance
            for (auto& [id, agent] : agents)
                agent.resolvedVelocity = agent.preferredVelocity;

            for (auto& [idA, a] : agents)
            {
                for (auto& [idB, b] : agents)
                {
                    if (idA >= idB)
                        continue;
                    float dist = a.position.DistanceTo(b.position);
                    float minDist = a.radius + b.radius;
                    if (dist < minDist && dist > 0.001f)
                    {
                        // Push apart
                        Vec2 dir = {(a.position.x - b.position.x) / dist, (a.position.y - b.position.y) / dist};
                        float pushStrength = (minDist - dist) * 2.0f;
                        a.resolvedVelocity = a.resolvedVelocity + dir * pushStrength;
                        b.resolvedVelocity = b.resolvedVelocity + dir * (-pushStrength);
                    }
                }
            }

            // Clamp to max speed
            for (auto& [id, agent] : agents)
            {
                float len = agent.resolvedVelocity.Length();
                if (len > agent.maxSpeed)
                    agent.resolvedVelocity = agent.resolvedVelocity * (agent.maxSpeed / len);
            }
        }
    };

} // namespace TestCollAvoid

// ============================================================================
// Registration
// ============================================================================

TEST(CollisionAvoidance_RegisterAgent)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    auto id1 = sys.RegisterAgent({0, 0}, 0.5f, 5.0f);
    auto id2 = sys.RegisterAgent({10, 0}, 0.5f, 5.0f);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(static_cast<int>(sys.agents.size()), 2);

    EXPECT_TRUE(sys.UnregisterAgent(id1));
    EXPECT_EQ(static_cast<int>(sys.agents.size()), 1);
    EXPECT_FALSE(sys.UnregisterAgent(id1));
}

// ============================================================================
// Preferred Velocity
// ============================================================================

TEST(CollisionAvoidance_SetPreferredVelocity)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    auto id = sys.RegisterAgent({0, 0}, 0.5f, 5.0f);
    EXPECT_TRUE(sys.SetPreferredVelocity(id, {3.0f, 4.0f}));
    EXPECT_NEAR(sys.agents[id].preferredVelocity.x, 3.0f, 0.001f);
    EXPECT_FALSE(sys.SetPreferredVelocity(999, {1, 1}));
}

// ============================================================================
// Solving — agents that overlap should be pushed apart
// ============================================================================

TEST(CollisionAvoidance_SolveNoOverlap)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    auto id1 = sys.RegisterAgent({0, 0}, 0.5f, 5.0f);
    auto id2 = sys.RegisterAgent({10, 0}, 0.5f, 5.0f);
    sys.SetPreferredVelocity(id1, {1, 0});
    sys.SetPreferredVelocity(id2, {-1, 0});

    sys.Solve();
    // No overlap, velocities should match preferred
    EXPECT_NEAR(sys.agents[id1].resolvedVelocity.x, 1.0f, 0.001f);
    EXPECT_NEAR(sys.agents[id2].resolvedVelocity.x, -1.0f, 0.001f);
}

TEST(CollisionAvoidance_SolveWithOverlap)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    // Two agents overlapping (distance 0.5, combined radius 1.0)
    auto id1 = sys.RegisterAgent({0, 0}, 0.5f, 10.0f);
    auto id2 = sys.RegisterAgent({0.5f, 0}, 0.5f, 10.0f);
    sys.SetPreferredVelocity(id1, {0, 0});
    sys.SetPreferredVelocity(id2, {0, 0});

    sys.Solve();
    // Agents should be pushed apart: id1 leftward, id2 rightward
    EXPECT_LT(sys.agents[id1].resolvedVelocity.x, 0.0f);
    EXPECT_GT(sys.agents[id2].resolvedVelocity.x, 0.0f);
}

TEST(CollisionAvoidance_MaxSpeedClamp)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    // Severely overlapping agents with low max speed
    auto id1 = sys.RegisterAgent({0, 0}, 1.0f, 2.0f);
    auto id2 = sys.RegisterAgent({0.1f, 0}, 1.0f, 2.0f);
    sys.SetPreferredVelocity(id1, {0, 0});
    sys.SetPreferredVelocity(id2, {0, 0});

    sys.Solve();
    float speed1 = sys.agents[id1].resolvedVelocity.Length();
    float speed2 = sys.agents[id2].resolvedVelocity.Length();
    EXPECT_LE(speed1, 2.0f + 0.01f);
    EXPECT_LE(speed2, 2.0f + 0.01f);
}

TEST(CollisionAvoidance_UnregisterAll)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    auto id1 = sys.RegisterAgent({0, 0}, 0.5f, 5.0f);
    auto id2 = sys.RegisterAgent({5, 0}, 0.5f, 5.0f);
    auto id3 = sys.RegisterAgent({10, 0}, 0.5f, 5.0f);

    EXPECT_TRUE(sys.UnregisterAgent(id1));
    EXPECT_TRUE(sys.UnregisterAgent(id2));
    EXPECT_TRUE(sys.UnregisterAgent(id3));
    EXPECT_TRUE(sys.agents.empty());
}

TEST(CollisionAvoidance_ThreeAgentsOverlap)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    auto id1 = sys.RegisterAgent({0, 0}, 1.0f, 10.0f);
    auto id2 = sys.RegisterAgent({0.5f, 0}, 1.0f, 10.0f);
    auto id3 = sys.RegisterAgent({0.25f, 0.5f}, 1.0f, 10.0f);

    sys.SetPreferredVelocity(id1, {0, 0});
    sys.SetPreferredVelocity(id2, {0, 0});
    sys.SetPreferredVelocity(id3, {0, 0});

    sys.Solve();
    // All agents should have non-zero resolved velocity (pushed apart)
    EXPECT_GT(sys.agents[id1].resolvedVelocity.Length(), 0.001f);
    EXPECT_GT(sys.agents[id2].resolvedVelocity.Length(), 0.001f);
    EXPECT_GT(sys.agents[id3].resolvedVelocity.Length(), 0.001f);
}

TEST(CollisionAvoidance_SolvePreservesPreferred)
{
    TestCollAvoid::CollisionAvoidanceSystem sys;
    auto id = sys.RegisterAgent({0, 0}, 0.5f, 5.0f);
    sys.SetPreferredVelocity(id, {3.0f, 4.0f});

    sys.Solve(); // Single agent, no collisions
    EXPECT_NEAR(sys.agents[id].resolvedVelocity.x, 3.0f, 0.001f);
    EXPECT_NEAR(sys.agents[id].resolvedVelocity.y, 4.0f, 0.001f);
}
