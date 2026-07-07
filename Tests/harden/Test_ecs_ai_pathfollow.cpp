/**
 * @file Test_ecs_ai_pathfollow.cpp
 * @brief Regression test for AIUpdateSystem path-following vs. Dynamic rigid bodies.
 *
 * Bug (P2): AIUpdateSystem path following wrote directly into the entity's Transform.
 * If the agent also had a Dynamic RigidBodyComponent, PhysicsUpdateSystem (which runs
 * before AI in the frame order) overwrites that Transform from the simulation every
 * frame, so the AI write was immediately stomped and the two systems fought — producing
 * stutter / no movement — despite the class doc claiming physics-integrated movement.
 *
 * Fix: the direct-Transform path-following movement is applied only to agents that are
 * NOT backed by a Dynamic rigid body. Kinematic and bodyless agents are safe (for a
 * Kinematic body PhysicsUpdateSystem reads the Transform *into* the body, and AI runs
 * after it), so they still move.
 *
 * This test mirrors the guard predicate and the movement step from ECSystems.cpp
 * (standalone replica, no EnTT dependency — same CI-safe pattern as TestECSIntegration.cpp)
 * and asserts: bodyless and Kinematic agents advance toward the waypoint; a Dynamic-bodied
 * agent's Transform is left untouched by this system.
 */

#include "TestFramework.h"
#include <cmath>

namespace
{
    // Mirror of RigidBodyComponent::Type from PhysicsComponents.h.
    enum class BodyType
    {
        None, ///< No RigidBodyComponent at all.
        Static,
        Kinematic,
        Dynamic
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// Mirror of the guard in AIUpdateSystem::Update: direct Transform movement is
    /// skipped for Dynamic-bodied agents (authority-owned by physics).
    bool PathFollowMovesTransform(BodyType type)
    {
        const bool dynamicBody = (type == BodyType::Dynamic);
        return !dynamicBody;
    }

    /// Mirror of one AIUpdateSystem path-following movement step toward a waypoint on
    /// the XZ plane. Only applied when PathFollowMovesTransform() is true.
    void StepTowardWaypoint(Vec3& position, const Vec3& waypoint, float moveSpeed, float deltaTime)
    {
        const float dx = waypoint.x - position.x;
        const float dz = waypoint.z - position.z;
        const float distSq = dx * dx + dz * dz;
        if (distSq < 0.25f) // waypoint reached (0.5 m)
            return;
        const float dist = std::sqrt(distSq);
        const float speed = moveSpeed * deltaTime;
        position.x += (dx / dist) * speed;
        position.z += (dz / dist) * speed;
    }

    Vec3 RunFrame(BodyType type, Vec3 start, const Vec3& waypoint)
    {
        Vec3 pos = start;
        if (PathFollowMovesTransform(type))
            StepTowardWaypoint(pos, waypoint, /*moveSpeed*/ 5.0f, /*deltaTime*/ 0.1f);
        return pos;
    }
} // namespace

TEST(EcsAiPathFollow_BodylessAgent_MovesTowardWaypoint)
{
    const Vec3 start{0.0f, 0.0f, 0.0f};
    const Vec3 waypoint{10.0f, 0.0f, 0.0f};
    const Vec3 after = RunFrame(BodyType::None, start, waypoint);
    EXPECT_GT(after.x, start.x); // advanced toward the waypoint
    EXPECT_NEAR(after.x, 0.5f, 1e-4f); // 5 m/s * 0.1 s = 0.5 m
}

TEST(EcsAiPathFollow_KinematicAgent_MovesTowardWaypoint)
{
    const Vec3 start{0.0f, 0.0f, 0.0f};
    const Vec3 waypoint{10.0f, 0.0f, 0.0f};
    const Vec3 after = RunFrame(BodyType::Kinematic, start, waypoint);
    EXPECT_GT(after.x, start.x);
    EXPECT_NEAR(after.x, 0.5f, 1e-4f);
}

TEST(EcsAiPathFollow_DynamicAgent_TransformUntouched)
{
    // A Dynamic-bodied agent must NOT be moved by the direct Transform write; physics owns it.
    const Vec3 start{0.0f, 0.0f, 0.0f};
    const Vec3 waypoint{10.0f, 0.0f, 0.0f};
    const Vec3 after = RunFrame(BodyType::Dynamic, start, waypoint);
    EXPECT_NEAR(after.x, start.x, 1e-6f);
    EXPECT_NEAR(after.z, start.z, 1e-6f);
}

TEST(EcsAiPathFollow_GuardPredicate)
{
    EXPECT_TRUE(PathFollowMovesTransform(BodyType::None));
    EXPECT_TRUE(PathFollowMovesTransform(BodyType::Static));
    EXPECT_TRUE(PathFollowMovesTransform(BodyType::Kinematic));
    EXPECT_FALSE(PathFollowMovesTransform(BodyType::Dynamic));
}
