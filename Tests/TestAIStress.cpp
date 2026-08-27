// TestAIStress.cpp - Stress/adversarial tests for AI subsystems
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <any>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestAIStress
{

    // ============================================================================
    // Behavior tree types (mirrors TestAIBehaviorTree.cpp)
    // ============================================================================

    enum class NodeStatus
    {
        Running,
        Success,
        Failure
    };

    class Blackboard
    {
      public:
        template <typename T> void Set(const std::string& key, const T& value) { m_data[key] = value; }

        template <typename T> T Get(const std::string& key, const T& defaultValue = T{}) const
        {
            auto it = m_data.find(key);
            if (it == m_data.end())
                return defaultValue;
            try
            {
                return std::any_cast<T>(it->second);
            }
            catch (...)
            {
                return defaultValue;
            }
        }

        template <typename T> T GetStrict(const std::string& key) const
        {
            auto it = m_data.find(key);
            if (it == m_data.end())
                throw std::runtime_error("Key not found: " + key);
            return std::any_cast<T>(it->second);
        }

        bool Has(const std::string& key) const { return m_data.count(key) > 0; }
        void Clear() { m_data.clear(); }
        size_t Size() const { return m_data.size(); }

      private:
        std::unordered_map<std::string, std::any> m_data;
    };

    class BTNode
    {
      public:
        virtual ~BTNode() = default;
        virtual NodeStatus Tick(float dt, Blackboard& bb) = 0;
    };

    class SequenceNode : public BTNode
    {
      public:
        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

        NodeStatus Tick(float dt, Blackboard& bb) override
        {
            for (auto& child : m_children)
            {
                NodeStatus status = child->Tick(dt, bb);
                if (status != NodeStatus::Success)
                    return status;
            }
            return NodeStatus::Success;
        }

        size_t ChildCount() const { return m_children.size(); }

      private:
        std::vector<std::unique_ptr<BTNode>> m_children;
    };

    class SelectorNode : public BTNode
    {
      public:
        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

        NodeStatus Tick(float dt, Blackboard& bb) override
        {
            for (auto& child : m_children)
            {
                NodeStatus status = child->Tick(dt, bb);
                if (status != NodeStatus::Failure)
                    return status;
            }
            return NodeStatus::Failure;
        }

        size_t ChildCount() const { return m_children.size(); }

      private:
        std::vector<std::unique_ptr<BTNode>> m_children;
    };

    class ActionNode : public BTNode
    {
      public:
        explicit ActionNode(std::function<NodeStatus(float, Blackboard&)> fn) : m_fn(fn) {}
        NodeStatus Tick(float dt, Blackboard& bb) override { return m_fn(dt, bb); }

      private:
        std::function<NodeStatus(float, Blackboard&)> m_fn;
    };

    // ============================================================================
    // Formation types (mirrors TestFormationSystem.cpp)
    // ============================================================================

    using FormationID = uint32_t;
    using EntityID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
        Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }

        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        float LengthSq() const { return x * x + y * y + z * z; }

        float DistanceTo(const Vec3& o) const { return (*this - o).Length(); }

        Vec3 Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 0, 0};
            return {x / len, y / len, z / len};
        }

        float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
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
        float heading = 0.0f;
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
    };

    // ============================================================================
    // Cover types (mirrors TestCoverSystem.cpp)
    // ============================================================================

    using CoverID = uint32_t;

    struct CoverPoint
    {
        CoverID id = 0;
        Vec3 position;
        Vec3 normal;
        bool occupied = false;
    };

    struct CoverSystem
    {
        std::vector<CoverPoint> coverPoints;
        CoverID nextID = 1;

        CoverID RegisterCoverPoint(Vec3 pos, Vec3 normal)
        {
            CoverID id = nextID++;
            coverPoints.push_back({id, pos, normal, false});
            return id;
        }

        const CoverPoint* FindNearestCover(Vec3 from) const
        {
            const CoverPoint* nearest = nullptr;
            float bestDist = 1e30f;
            for (const auto& cp : coverPoints)
            {
                if (cp.occupied)
                    continue;
                float d = from.DistanceTo(cp.position);
                if (d < bestDist)
                {
                    bestDist = d;
                    nearest = &cp;
                }
            }
            return nearest;
        }
    };

    // ============================================================================
    // Steering types (mirrors TestSteeringBehaviors.cpp)
    // ============================================================================

    struct SteeringAgent
    {
        Vec3 position{0, 0, 0};
        Vec3 velocity{0, 0, 0};
        Vec3 forward{0, 0, 1};
        float maxSpeed = 5.0f;
        float maxForce = 10.0f;
        float mass = 1.0f;
    };

    Vec3 Seek(const SteeringAgent& agent, const Vec3& target)
    {
        Vec3 toTarget = target - agent.position;
        float dist = toTarget.Length();
        if (dist < 0.0001f)
            return {0, 0, 0};

        Vec3 desired = toTarget.Normalized() * agent.maxSpeed;
        Vec3 steering = desired - agent.velocity;

        float len = steering.Length();
        if (len > agent.maxForce && agent.maxForce > 0.0f)
        {
            steering = steering.Normalized() * agent.maxForce;
        }
        return steering;
    }

    Vec3 Separation(const SteeringAgent& agent, const std::vector<SteeringAgent>& neighbors,
                    float separationRadius = 3.0f)
    {
        Vec3 steeringForce{0, 0, 0};
        int count = 0;

        for (const auto& other : neighbors)
        {
            Vec3 toAgent = agent.position - other.position;
            float dist = toAgent.Length();

            if (dist > 0.001f && dist < separationRadius)
            {
                Vec3 repulsion = toAgent.Normalized() * (1.0f / dist);
                steeringForce = steeringForce + repulsion;
                count++;
            }
        }

        if (count > 0)
        {
            steeringForce = steeringForce / static_cast<float>(count);
            if (steeringForce.Length() > 0.001f && agent.maxForce > 0.0f)
            {
                steeringForce = steeringForce.Normalized() * agent.maxForce;
            }
        }

        return steeringForce;
    }

    void ApplySteeringForce(SteeringAgent& agent, const Vec3& force, float dt)
    {
        if (agent.mass <= 0.0f)
            return;

        Vec3 acceleration = force / agent.mass;
        agent.velocity = agent.velocity + acceleration * dt;

        float speed = agent.velocity.Length();
        if (speed > agent.maxSpeed && agent.maxSpeed > 0.0f)
        {
            agent.velocity = agent.velocity.Normalized() * agent.maxSpeed;
        }

        agent.position = agent.position + agent.velocity * dt;

        if (agent.velocity.Length() > 0.01f)
        {
            agent.forward = agent.velocity.Normalized();
        }
    }

    // ============================================================================
    // Pathfinding types (simple graph for testing)
    // ============================================================================

    struct NavNode
    {
        uint32_t id = 0;
        Vec3 position{0, 0, 0};
        std::vector<uint32_t> neighbors;
    };

    struct NavGraph
    {
        std::unordered_map<uint32_t, NavNode> nodes;

        void AddNode(uint32_t id, Vec3 pos) { nodes[id] = {id, pos, {}}; }

        void AddEdge(uint32_t from, uint32_t to)
        {
            if (nodes.count(from) && nodes.count(to))
            {
                nodes[from].neighbors.push_back(to);
                nodes[to].neighbors.push_back(from);
            }
        }

        // Simple BFS pathfinding
        std::vector<uint32_t> FindPath(uint32_t startId, uint32_t goalId) const
        {
            if (!nodes.count(startId) || !nodes.count(goalId))
                return {};

            if (startId == goalId)
                return {startId};

            std::unordered_map<uint32_t, uint32_t> cameFrom;
            std::vector<uint32_t> frontier;
            frontier.push_back(startId);
            cameFrom[startId] = startId;

            while (!frontier.empty())
            {
                std::vector<uint32_t> nextFrontier;
                for (uint32_t current : frontier)
                {
                    auto nodeIt = nodes.find(current);
                    if (nodeIt == nodes.end())
                        continue;
                    for (uint32_t neighbor : nodeIt->second.neighbors)
                    {
                        if (cameFrom.count(neighbor) == 0)
                        {
                            cameFrom[neighbor] = current;
                            if (neighbor == goalId)
                            {
                                // Reconstruct path
                                std::vector<uint32_t> path;
                                uint32_t node = goalId;
                                while (node != startId)
                                {
                                    path.push_back(node);
                                    node = cameFrom[node];
                                }
                                path.push_back(startId);
                                std::reverse(path.begin(), path.end());
                                return path;
                            }
                            nextFrontier.push_back(neighbor);
                        }
                    }
                }
                frontier = std::move(nextFrontier);
            }

            return {}; // No path found
        }
    };

} // namespace TestAIStress

// ============================================================================
// Test 1: Behavior Tree Deep Nesting
// ============================================================================

TEST(AIStress_BehaviorTreeDeepNesting)
{
    TestAIStress::Blackboard bb;
    int executionCount = 0;

    // Build a chain of 100 nested sequence nodes, each containing the next
    // The innermost contains an action that succeeds
    auto innerAction = std::make_unique<TestAIStress::ActionNode>(
        [&executionCount](float, TestAIStress::Blackboard&)
        {
            executionCount++;
            return TestAIStress::NodeStatus::Success;
        });

    std::unique_ptr<TestAIStress::BTNode> current = std::move(innerAction);

    for (int i = 0; i < 100; i++)
    {
        auto seq = std::make_unique<TestAIStress::SequenceNode>();
        seq->AddChild(std::move(current));
        current = std::move(seq);
    }

    // Tick the deeply nested tree — should not stack overflow
    TestAIStress::NodeStatus result = current->Tick(0.016f, bb);
    EXPECT_TRUE(result == TestAIStress::NodeStatus::Success);
    EXPECT_EQ(executionCount, 1);
}

// ============================================================================
// Test 2: Behavior Tree Infinite Loop (Always Running)
// ============================================================================

TEST(AIStress_BehaviorTreeInfiniteLoop)
{
    TestAIStress::Blackboard bb;
    int tickCount = 0;

    auto alwaysRunning = std::make_unique<TestAIStress::ActionNode>(
        [&tickCount](float, TestAIStress::Blackboard&)
        {
            tickCount++;
            return TestAIStress::NodeStatus::Running;
        });

    // Tick 10000 times — node always returns Running
    for (int i = 0; i < 10000; i++)
    {
        TestAIStress::NodeStatus status = alwaysRunning->Tick(0.016f, bb);
        EXPECT_TRUE(status == TestAIStress::NodeStatus::Running);
    }

    EXPECT_EQ(tickCount, 10000);
}

// ============================================================================
// Test 3: Behavior Tree Empty Sequence
// ============================================================================

TEST(AIStress_BehaviorTreeEmptySequence)
{
    TestAIStress::Blackboard bb;
    TestAIStress::SequenceNode seq;

    // Empty sequence with no children — should return Success (vacuous truth)
    TestAIStress::NodeStatus result = seq.Tick(0.016f, bb);
    EXPECT_TRUE(result == TestAIStress::NodeStatus::Success);
    EXPECT_EQ(static_cast<int>(seq.ChildCount()), 0);
}

// ============================================================================
// Test 4: Behavior Tree Empty Selector
// ============================================================================

TEST(AIStress_BehaviorTreeEmptySelector)
{
    TestAIStress::Blackboard bb;
    TestAIStress::SelectorNode sel;

    // Empty selector with no children — should return Failure (nothing to try)
    TestAIStress::NodeStatus result = sel.Tick(0.016f, bb);
    EXPECT_TRUE(result == TestAIStress::NodeStatus::Failure);
    EXPECT_EQ(static_cast<int>(sel.ChildCount()), 0);
}

// ============================================================================
// Test 5: Blackboard Type Confusion
// ============================================================================

TEST(AIStress_BlackboardTypeConfusion)
{
    TestAIStress::Blackboard bb;
    bb.Set("value", 42);

    // Try to get as wrong type — safe version returns default
    std::string result = bb.Get<std::string>("value", std::string("fallback"));
    EXPECT_EQ(result, std::string("fallback"));

    // Strict version should throw bad_any_cast
    bool caughtException = false;
    try
    {
        std::string strict = bb.GetStrict<std::string>("value");
        (void)strict;
    }
    catch (const std::bad_any_cast&)
    {
        caughtException = true;
    }
    catch (...)
    {
        // GetStrict might throw runtime_error if key not found, or bad_any_cast for type mismatch
        caughtException = true;
    }
    EXPECT_TRUE(caughtException);
}

// ============================================================================
// Test 6: Blackboard Massive Entries
// ============================================================================

TEST(AIStress_BlackboardMassiveEntries)
{
    TestAIStress::Blackboard bb;

    for (int i = 0; i < 10000; i++)
    {
        bb.Set("key_" + std::to_string(i), i);
    }

    EXPECT_EQ(static_cast<int>(bb.Size()), 10000);

    // Verify first, middle, and last entries
    EXPECT_EQ(bb.Get<int>("key_0"), 0);
    EXPECT_EQ(bb.Get<int>("key_5000"), 5000);
    EXPECT_EQ(bb.Get<int>("key_9999"), 9999);

    // Verify non-existent key returns default
    EXPECT_EQ(bb.Get<int>("key_10000", -1), -1);
}

// ============================================================================
// Test 7: Blackboard Empty Key
// ============================================================================

TEST(AIStress_BlackboardEmptyKey)
{
    TestAIStress::Blackboard bb;

    // Set with empty string key
    bb.Set("", 99);
    EXPECT_TRUE(bb.Has(""));
    EXPECT_EQ(bb.Get<int>(""), 99);

    // Overwrite empty key
    bb.Set("", 200);
    EXPECT_EQ(bb.Get<int>(""), 200);
}

// ============================================================================
// Test 8: Formation Massive Slots
// ============================================================================

TEST(AIStress_FormationMassiveSlots)
{
    TestAIStress::FormationSystem sys;
    auto fid = sys.CreateFormation("MassiveFormation", {0, 0, 0}, 1000);

    EXPECT_EQ(static_cast<int>(sys.formations[fid].slots.size()), 1000);

    // Assign entities to first and last slots
    EXPECT_TRUE(sys.AssignToSlot(fid, 0, 1));
    EXPECT_TRUE(sys.AssignToSlot(fid, 999, 2));

    EXPECT_TRUE(sys.formations[fid].slots[0].occupied);
    EXPECT_TRUE(sys.formations[fid].slots[999].occupied);
    EXPECT_FALSE(sys.formations[fid].slots[500].occupied);
}

// ============================================================================
// Test 9: Formation Zero Slots
// ============================================================================

TEST(AIStress_FormationZeroSlots)
{
    TestAIStress::FormationSystem sys;
    auto fid = sys.CreateFormation("EmptyFormation", {0, 0, 0}, 0);

    EXPECT_EQ(static_cast<int>(sys.formations[fid].slots.size()), 0);

    // Assigning to any slot should fail
    EXPECT_FALSE(sys.AssignToSlot(fid, 0, 100));
    EXPECT_FALSE(sys.AssignToSlot(fid, -1, 100));
}

// ============================================================================
// Test 10: Formation Assign Invalid Entity
// ============================================================================

TEST(AIStress_FormationAssignInvalidEntity)
{
    TestAIStress::FormationSystem sys;
    auto fid = sys.CreateFormation("TestFormation", {0, 0, 0}, 3);

    // Assign to non-existent slot index
    EXPECT_FALSE(sys.AssignToSlot(fid, 5, 100));
    EXPECT_FALSE(sys.AssignToSlot(fid, -1, 100));

    // Assign to non-existent formation
    EXPECT_FALSE(sys.AssignToSlot(999, 0, 100));

    // Valid assignment should still work
    EXPECT_TRUE(sys.AssignToSlot(fid, 0, 100));
}

// ============================================================================
// Test 11: Cover Point Flood
// ============================================================================

TEST(AIStress_CoverPointFlood)
{
    TestAIStress::CoverSystem sys;

    for (int i = 0; i < 5000; i++)
    {
        float x = static_cast<float>(i % 100);
        float z = static_cast<float>(i / 100);
        sys.RegisterCoverPoint({x, 0, z}, {0, 0, 1});
    }

    EXPECT_EQ(static_cast<int>(sys.coverPoints.size()), 5000);

    // Nearest cover should still work efficiently
    const auto* nearest = sys.FindNearestCover({0, 0, 0});
    ASSERT_TRUE(nearest != nullptr);
    EXPECT_NEAR(nearest->position.x, 0.0f, 0.001f);
    EXPECT_NEAR(nearest->position.z, 0.0f, 0.001f);
}

// ============================================================================
// Test 12: Cover Point Duplicate Position
// ============================================================================

TEST(AIStress_CoverPointDuplicatePosition)
{
    TestAIStress::CoverSystem sys;

    // Register multiple cover points at the exact same position
    auto id1 = sys.RegisterCoverPoint({5, 0, 5}, {0, 0, 1});
    auto id2 = sys.RegisterCoverPoint({5, 0, 5}, {1, 0, 0});
    auto id3 = sys.RegisterCoverPoint({5, 0, 5}, {0, 0, -1});

    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    EXPECT_EQ(static_cast<int>(sys.coverPoints.size()), 3);

    // Nearest cover should return one of them
    const auto* nearest = sys.FindNearestCover({5, 0, 5});
    EXPECT_TRUE(nearest != nullptr);
}

// ============================================================================
// Test 13: Cover Query Empty System
// ============================================================================

TEST(AIStress_CoverQueryEmptySystem)
{
    TestAIStress::CoverSystem sys;

    // Query with no cover points registered
    const auto* nearest = sys.FindNearestCover({0, 0, 0});
    EXPECT_TRUE(nearest == nullptr);
}

// ============================================================================
// Test 14: Steering Zero Max Speed
// ============================================================================

TEST(AIStress_SteeringZeroMaxSpeed)
{
    TestAIStress::SteeringAgent agent;
    agent.position = {0, 0, 0};
    agent.velocity = {0, 0, 0};
    agent.maxSpeed = 0.0f;
    agent.maxForce = 10.0f;

    TestAIStress::Vec3 target{10, 0, 0};
    TestAIStress::Vec3 force = TestAIStress::Seek(agent, target);

    // With zero maxSpeed, desired velocity magnitude is 0, so steering is just -velocity
    // Force should not contain NaN or inf
    EXPECT_FALSE(std::isnan(force.x));
    EXPECT_FALSE(std::isnan(force.y));
    EXPECT_FALSE(std::isnan(force.z));

    // Apply force with zero maxSpeed — velocity should remain clamped at 0
    TestAIStress::ApplySteeringForce(agent, force, 0.1f);
    EXPECT_FALSE(std::isnan(agent.position.x));
    EXPECT_FALSE(std::isinf(agent.position.x));
}

// ============================================================================
// Test 15: Steering NaN Position
// ============================================================================

TEST(AIStress_SteeringNaNPosition)
{
    float nan = std::numeric_limits<float>::quiet_NaN();

    TestAIStress::SteeringAgent agent;
    agent.position = {nan, nan, nan};
    agent.velocity = {0, 0, 0};
    agent.maxSpeed = 5.0f;
    agent.maxForce = 10.0f;

    TestAIStress::Vec3 target{10, 0, 0};

    // Seek with NaN position — toTarget will be NaN, normalized will be (0,0,0)
    TestAIStress::Vec3 force = TestAIStress::Seek(agent, target);

    // The result may be NaN due to NaN propagation, but should not crash
    // Just verify no crash occurred
    EXPECT_TRUE(std::isnan(force.x) || std::isfinite(force.x));
}

// ============================================================================
// Test 16: Steering Massive Neighbor Count
// ============================================================================

TEST(AIStress_SteeringMassiveNeighborCount)
{
    TestAIStress::SteeringAgent agent;
    agent.position = {50, 0, 50};
    agent.maxForce = 10.0f;
    agent.maxSpeed = 5.0f;

    // Create 500 neighbors in a tight cluster around the agent
    std::vector<TestAIStress::SteeringAgent> neighbors;
    neighbors.reserve(500);
    for (int i = 0; i < 500; i++)
    {
        TestAIStress::SteeringAgent n;
        float angle = static_cast<float>(i) * (2.0f * 3.14159265f / 500.0f);
        float dist = 0.5f + static_cast<float>(i % 10) * 0.1f;
        n.position = {50.0f + std::cos(angle) * dist, 0.0f, 50.0f + std::sin(angle) * dist};
        neighbors.push_back(n);
    }

    TestAIStress::Vec3 force = TestAIStress::Separation(agent, neighbors, 5.0f);

    // Force should be finite and not NaN
    EXPECT_FALSE(std::isnan(force.x));
    EXPECT_FALSE(std::isnan(force.y));
    EXPECT_FALSE(std::isnan(force.z));
    EXPECT_FALSE(std::isinf(force.x));

    // With so many close neighbors, separation force should be non-zero
    EXPECT_TRUE(force.Length() > 0.0f);
}

// ============================================================================
// Test 17: Pathfinding Start Equals Goal
// ============================================================================

TEST(AIStress_PathfindingStartEqualsGoal)
{
    TestAIStress::NavGraph graph;
    graph.AddNode(1, {0, 0, 0});
    graph.AddNode(2, {10, 0, 0});
    graph.AddEdge(1, 2);

    // Pathfind from A to A — should return a path containing just the start node
    auto path = graph.FindPath(1, 1);
    EXPECT_EQ(static_cast<int>(path.size()), 1);
    EXPECT_EQ(path[0], (uint32_t)1);
}

// ============================================================================
// Test 18: Pathfinding Unreachable Goal
// ============================================================================

TEST(AIStress_PathfindingUnreachableGoal)
{
    TestAIStress::NavGraph graph;

    // Two disconnected components
    graph.AddNode(1, {0, 0, 0});
    graph.AddNode(2, {10, 0, 0});
    graph.AddEdge(1, 2);

    graph.AddNode(10, {100, 0, 0});
    graph.AddNode(11, {110, 0, 0});
    graph.AddEdge(10, 11);

    // Try to pathfind from component 1 to component 2 — should return empty
    auto path = graph.FindPath(1, 10);
    EXPECT_TRUE(path.empty());

    // Also try non-existent node
    auto path2 = graph.FindPath(1, 999);
    EXPECT_TRUE(path2.empty());
}
