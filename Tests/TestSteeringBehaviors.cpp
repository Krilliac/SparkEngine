// TestSteeringBehaviors.cpp - Tests for AI steering behaviors
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace TestSteering
{

    // ============================================================================
    // Minimal math types for cross-platform testing
    // ============================================================================

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

    // ============================================================================
    // Agent structure for steering behaviors
    // ============================================================================

    struct SteeringAgent
    {
        Vec3 position{0, 0, 0};
        Vec3 velocity{0, 0, 0};
        Vec3 forward{0, 0, 1}; ///< Facing direction (normalized)
        float maxSpeed = 5.0f;
        float maxForce = 10.0f;
        float mass = 1.0f;
        float radius = 0.5f; ///< Agent collision radius
    };

    // ============================================================================
    // Steering behaviors (mirrors planned Spark::AI::SteeringBehaviors)
    // ============================================================================

    /// Seek: steer toward a target position
    Vec3 Seek(const SteeringAgent& agent, const Vec3& target)
    {
        Vec3 desired = (target - agent.position).Normalized() * agent.maxSpeed;
        Vec3 steering = desired - agent.velocity;

        // Clamp to max force
        float len = steering.Length();
        if (len > agent.maxForce)
        {
            steering = steering.Normalized() * agent.maxForce;
        }
        return steering;
    }

    /// Flee: steer away from a threat position
    Vec3 Flee(const SteeringAgent& agent, const Vec3& threat)
    {
        Vec3 desired = (agent.position - threat).Normalized() * agent.maxSpeed;
        Vec3 steering = desired - agent.velocity;

        float len = steering.Length();
        if (len > agent.maxForce)
        {
            steering = steering.Normalized() * agent.maxForce;
        }
        return steering;
    }

    /// Arrive: seek with deceleration as agent approaches target
    Vec3 Arrive(const SteeringAgent& agent, const Vec3& target, float slowingRadius = 5.0f)
    {
        Vec3 toTarget = target - agent.position;
        float distance = toTarget.Length();

        if (distance < 0.001f)
            return {0, 0, 0};

        float speed = agent.maxSpeed;
        if (distance < slowingRadius)
        {
            speed = agent.maxSpeed * (distance / slowingRadius);
        }

        Vec3 desired = toTarget.Normalized() * speed;
        Vec3 steering = desired - agent.velocity;

        float len = steering.Length();
        if (len > agent.maxForce)
        {
            steering = steering.Normalized() * agent.maxForce;
        }
        return steering;
    }

    /// Separation: steer away from nearby agents to avoid crowding
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
                // Weight by inverse distance (closer = stronger repulsion)
                Vec3 repulsion = toAgent.Normalized() * (1.0f / dist);
                steeringForce = steeringForce + repulsion;
                count++;
            }
        }

        if (count > 0)
        {
            steeringForce = steeringForce / static_cast<float>(count);
            if (steeringForce.Length() > 0.001f)
            {
                steeringForce = steeringForce.Normalized() * agent.maxForce;
            }
        }

        return steeringForce;
    }

    // ============================================================================
    // Perception helpers
    // ============================================================================

    /// Check if a target is within the observer's field of view cone
    bool CanSee(const SteeringAgent& observer, const Vec3& targetPos, float fovDegrees = 120.0f, float maxRange = 30.0f)
    {
        Vec3 toTarget = targetPos - observer.position;
        float dist = toTarget.Length();

        if (dist > maxRange || dist < 0.001f)
            return false;

        Vec3 dirToTarget = toTarget.Normalized();
        float dotProduct = observer.forward.Dot(dirToTarget);

        // fovDegrees is the full cone angle; half-angle in radians:
        float halfFovRad = (fovDegrees * 0.5f) * 3.14159265f / 180.0f;
        float cosHalfFov = std::cos(halfFovRad);

        return dotProduct >= cosHalfFov;
    }

    /// Check if a sound at soundPos with given radius can be heard by the agent
    bool CanHear(const SteeringAgent& agent, const Vec3& soundPos, float soundRadius)
    {
        float dist = agent.position.DistanceTo(soundPos);
        return dist <= soundRadius;
    }

    // ============================================================================
    // Utility: apply steering force over a timestep
    // ============================================================================

    void ApplySteeringForce(SteeringAgent& agent, const Vec3& force, float dt)
    {
        Vec3 acceleration = force / agent.mass;
        agent.velocity = agent.velocity + acceleration * dt;

        // Clamp to max speed
        float speed = agent.velocity.Length();
        if (speed > agent.maxSpeed)
        {
            agent.velocity = agent.velocity.Normalized() * agent.maxSpeed;
        }

        agent.position = agent.position + agent.velocity * dt;

        // Update forward direction
        if (agent.velocity.Length() > 0.01f)
        {
            agent.forward = agent.velocity.Normalized();
        }
    }

} // namespace TestSteering

// ============================================================================
// Tests: Seek
// ============================================================================

TEST(Steering_SeekMovesTowardTarget)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};
    agent.velocity = {0, 0, 0};
    agent.maxSpeed = 5.0f;
    agent.maxForce = 10.0f;

    TestSteering::Vec3 target{10, 0, 0};

    TestSteering::Vec3 force = TestSteering::Seek(agent, target);

    // Force should point toward the target (positive X)
    EXPECT_TRUE(force.x > 0.0f);
    EXPECT_NEAR(force.y, 0.0f, 0.001f);
    EXPECT_NEAR(force.z, 0.0f, 0.001f);

    // Apply force and check that agent moves closer to target
    float distBefore = agent.position.DistanceTo(target);
    TestSteering::ApplySteeringForce(agent, force, 0.1f);
    float distAfter = agent.position.DistanceTo(target);

    EXPECT_TRUE(distAfter < distBefore);
}

TEST(Steering_SeekProducesCorrectDirection)
{
    TestSteering::SteeringAgent agent;
    agent.position = {5, 0, 5};
    agent.velocity = {0, 0, 0};

    // Target is behind and to the left
    TestSteering::Vec3 target{0, 0, 0};

    TestSteering::Vec3 force = TestSteering::Seek(agent, target);

    // Force should point toward target (negative X and Z)
    EXPECT_TRUE(force.x < 0.0f);
    EXPECT_TRUE(force.z < 0.0f);
}

// ============================================================================
// Tests: Flee
// ============================================================================

TEST(Steering_FleeMovesAwayFromThreat)
{
    TestSteering::SteeringAgent agent;
    agent.position = {5, 0, 5};
    agent.velocity = {0, 0, 0};
    agent.maxSpeed = 5.0f;
    agent.maxForce = 10.0f;

    TestSteering::Vec3 threat{5, 0, 0}; // Threat is directly behind on Z axis

    TestSteering::Vec3 force = TestSteering::Flee(agent, threat);

    // Force should point away from threat (positive Z direction)
    EXPECT_TRUE(force.z > 0.0f);

    // Apply force and verify agent moves away
    float distBefore = agent.position.DistanceTo(threat);
    TestSteering::ApplySteeringForce(agent, force, 0.1f);
    float distAfter = agent.position.DistanceTo(threat);

    EXPECT_TRUE(distAfter > distBefore);
}

TEST(Steering_FleeOppositeToSeek)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};
    agent.velocity = {0, 0, 0};
    agent.maxSpeed = 5.0f;
    agent.maxForce = 10.0f;

    TestSteering::Vec3 point{10, 0, 0};

    TestSteering::Vec3 seekForce = TestSteering::Seek(agent, point);
    TestSteering::Vec3 fleeForce = TestSteering::Flee(agent, point);

    // Seek and Flee should produce opposite directions
    float dot = seekForce.Dot(fleeForce);
    EXPECT_TRUE(dot < 0.0f);
}

// ============================================================================
// Tests: Arrive
// ============================================================================

TEST(Steering_ArriveDeceleratesNearTarget)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};
    agent.velocity = {0, 0, 0};
    agent.maxSpeed = 10.0f;
    agent.maxForce = 20.0f;

    float slowingRadius = 5.0f;

    // Far from target: should produce strong force
    TestSteering::Vec3 farForce = TestSteering::Arrive(agent, {20, 0, 0}, slowingRadius);
    float farMagnitude = farForce.Length();

    // Close to target (within slowing radius): should produce weaker desired speed
    agent.position = {8, 0, 0};
    TestSteering::Vec3 target{10, 0, 0}; // 2 units away, within slowing radius
    TestSteering::Vec3 nearForce = TestSteering::Arrive(agent, target, slowingRadius);

    // The arrive behavior for the near agent should produce a desired speed that
    // is proportionally reduced. Since both agents have zero velocity the forces
    // reflect the desired velocities directly.
    // Far: desired speed = maxSpeed = 10; Near: desired speed = 10 * (2/5) = 4
    EXPECT_TRUE(nearForce.Length() < farMagnitude);

    // At the target: should produce zero force
    agent.position = target;
    TestSteering::Vec3 atTargetForce = TestSteering::Arrive(agent, target, slowingRadius);
    EXPECT_NEAR(atTargetForce.Length(), 0.0f, 0.01f);
}

// ============================================================================
// Tests: Separation
// ============================================================================

TEST(Steering_SeparationCreatesSpacing)
{
    TestSteering::SteeringAgent agent;
    agent.position = {5, 0, 5};
    agent.maxForce = 10.0f;

    // Place two neighbors very close
    std::vector<TestSteering::SteeringAgent> neighbors;

    TestSteering::SteeringAgent n1;
    n1.position = {5.5f, 0, 5};
    neighbors.push_back(n1);

    TestSteering::SteeringAgent n2;
    n2.position = {5, 0, 5.5f};
    neighbors.push_back(n2);

    TestSteering::Vec3 force = TestSteering::Separation(agent, neighbors, 3.0f);

    // Separation force should push agent away from neighbors
    EXPECT_TRUE(force.Length() > 0.0f);

    // Neighbors are to the positive-x and positive-z, so force should be negative
    EXPECT_TRUE(force.x < 0.0f);
    EXPECT_TRUE(force.z < 0.0f);
}

TEST(Steering_SeparationZeroForNoNeighbors)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};

    std::vector<TestSteering::SteeringAgent> empty;
    TestSteering::Vec3 force = TestSteering::Separation(agent, empty, 3.0f);

    EXPECT_NEAR(force.x, 0.0f, 0.001f);
    EXPECT_NEAR(force.y, 0.0f, 0.001f);
    EXPECT_NEAR(force.z, 0.0f, 0.001f);
}

TEST(Steering_SeparationIgnoresDistantAgents)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};

    // Neighbor far outside separation radius
    std::vector<TestSteering::SteeringAgent> neighbors;
    TestSteering::SteeringAgent far;
    far.position = {100, 0, 100};
    neighbors.push_back(far);

    TestSteering::Vec3 force = TestSteering::Separation(agent, neighbors, 3.0f);

    EXPECT_NEAR(force.x, 0.0f, 0.001f);
    EXPECT_NEAR(force.y, 0.0f, 0.001f);
    EXPECT_NEAR(force.z, 0.0f, 0.001f);
}

// ============================================================================
// Tests: CanSee (Field of View)
// ============================================================================

TEST(Steering_CanSeeTargetInFOV)
{
    TestSteering::SteeringAgent observer;
    observer.position = {0, 0, 0};
    observer.forward = {0, 0, 1}; // Looking along +Z

    // Target directly ahead
    TestSteering::Vec3 targetAhead{0, 0, 10};
    EXPECT_TRUE(TestSteering::CanSee(observer, targetAhead, 120.0f, 30.0f));

    // Target slightly to the side but within FOV
    TestSteering::Vec3 targetSide{3, 0, 10};
    EXPECT_TRUE(TestSteering::CanSee(observer, targetSide, 120.0f, 30.0f));
}

TEST(Steering_CannotSeeTargetBehind)
{
    TestSteering::SteeringAgent observer;
    observer.position = {0, 0, 0};
    observer.forward = {0, 0, 1}; // Looking along +Z

    // Target directly behind
    TestSteering::Vec3 targetBehind{0, 0, -10};
    EXPECT_FALSE(TestSteering::CanSee(observer, targetBehind, 120.0f, 30.0f));

    // Target behind and to the side
    TestSteering::Vec3 targetBehindSide{5, 0, -10};
    EXPECT_FALSE(TestSteering::CanSee(observer, targetBehindSide, 90.0f, 30.0f));
}

TEST(Steering_CannotSeeBeyondRange)
{
    TestSteering::SteeringAgent observer;
    observer.position = {0, 0, 0};
    observer.forward = {0, 0, 1};

    // Target ahead but beyond max range
    TestSteering::Vec3 farTarget{0, 0, 50};
    EXPECT_FALSE(TestSteering::CanSee(observer, farTarget, 120.0f, 30.0f));
}

TEST(Steering_CanSeeNarrowFOV)
{
    TestSteering::SteeringAgent observer;
    observer.position = {0, 0, 0};
    observer.forward = {0, 0, 1};

    // Target at ~45 degrees, narrow 60-degree FOV should exclude it
    TestSteering::Vec3 target45deg{10, 0, 10}; // 45 degrees off center
    EXPECT_FALSE(TestSteering::CanSee(observer, target45deg, 60.0f, 30.0f));

    // Same target with wider FOV
    EXPECT_TRUE(TestSteering::CanSee(observer, target45deg, 120.0f, 30.0f));
}

// ============================================================================
// Tests: CanHear
// ============================================================================

TEST(Steering_CanHearWithinRadius)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};

    // Sound nearby
    EXPECT_TRUE(TestSteering::CanHear(agent, {3, 0, 0}, 10.0f));

    // Sound at exact radius boundary
    EXPECT_TRUE(TestSteering::CanHear(agent, {10, 0, 0}, 10.0f));
}

TEST(Steering_CannotHearBeyondRadius)
{
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};

    // Sound beyond radius
    EXPECT_FALSE(TestSteering::CanHear(agent, {20, 0, 0}, 10.0f));
}

TEST(Steering_CanHearBehind)
{
    // Hearing is omnidirectional, unlike sight
    TestSteering::SteeringAgent agent;
    agent.position = {0, 0, 0};
    agent.forward = {0, 0, 1}; // Facing +Z

    // Sound behind the agent
    TestSteering::Vec3 soundBehind{0, 0, -5};
    EXPECT_TRUE(TestSteering::CanHear(agent, soundBehind, 10.0f));
}
