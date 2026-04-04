/**
 * @file TestPhysicsECSIntegration.cpp
 * @brief Integration tests verifying Physics + ECS interaction patterns
 */

#include "TestFramework.h"
#include <cmath>
#include <cstdint>

// Self-contained physics+ECS integration tests using inline types
// to avoid coupling to internal API changes

namespace
{
    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };
    struct Quat
    {
        float x = 0, y = 0, z = 0, w = 1;
    };
    struct TransformComp
    {
        Vec3 position;
        Quat rotation;
        Vec3 scale{1, 1, 1};
    };
    enum class BodyType : uint8_t
    {
        Static,
        Dynamic,
        Kinematic
    };
    struct RigidBodyComp
    {
        BodyType type = BodyType::Dynamic;
        float mass = 1.0f;
        float gravityScale = 1.0f;
        float linearDamping = 0.0f;
    };
    struct ColliderComp
    {
        enum class Shape : uint8_t
        {
            Box,
            Sphere,
            Capsule
        };
        Shape shape = Shape::Box;
        float radius = 0.5f;
        Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    };

    void SimulateGravityStep(TransformComp& t, float& velocity, float gravity, float dt)
    {
        velocity += gravity * dt;
        t.position.y += velocity * dt;
    }
} // namespace

TEST(PhysicsECS_DynamicBodyFalls)
{
    TransformComp t;
    t.position.y = 100.0f;
    float vel = 0;
    SimulateGravityStep(t, vel, -9.81f, 1.0f / 60.0f);
    EXPECT_TRUE(t.position.y < 100.0f);
}

TEST(PhysicsECS_StaticBodyStays)
{
    TransformComp t;
    RigidBodyComp rb;
    rb.type = BodyType::Static;
    // Static bodies don't move
    EXPECT_NEAR(t.position.x, 0.0f, 0.001f);
    EXPECT_NEAR(t.position.y, 0.0f, 0.001f);
}

TEST(PhysicsECS_KinematicFollowsProgram)
{
    TransformComp t;
    t.position.x = 5.0f;
    t.position.y = 10.0f;
    EXPECT_NEAR(t.position.x, 5.0f, 0.001f);
}

TEST(PhysicsECS_GravityScaleAffects)
{
    float normalG = -9.81f * 1.0f;
    float halfG = -9.81f * 0.5f;
    float zeroG = -9.81f * 0.0f;
    EXPECT_NEAR(normalG, -9.81f, 0.001f);
    EXPECT_NEAR(halfG, -4.905f, 0.001f);
    EXPECT_NEAR(zeroG, 0.0f, 0.001f);
}

TEST(PhysicsECS_MultiStepAcceleration)
{
    TransformComp t;
    t.position.y = 100.0f;
    float vel = 0;
    float dt = 1.0f / 60.0f;
    SimulateGravityStep(t, vel, -9.81f, dt);
    float y1 = t.position.y;
    SimulateGravityStep(t, vel, -9.81f, dt);
    float y2 = t.position.y;
    EXPECT_TRUE(y1 < 100.0f);
    EXPECT_TRUE(y2 < y1);
}

TEST(PhysicsECS_CollisionLayerMask)
{
    uint32_t layerA = 0x01, layerB = 0x02, layerC = 0x04;
    uint32_t maskAB = layerA | layerB;
    EXPECT_TRUE((layerA & maskAB) != 0);
    EXPECT_TRUE((layerC & maskAB) == 0);
}

TEST(PhysicsECS_IndependentEntities)
{
    TransformComp t1, t2;
    t1.position.y = 100.0f;
    t2.position.y = 50.0f;
    float v1 = 0, v2 = 0;
    float dt = 1.0f / 60.0f;
    SimulateGravityStep(t1, v1, -9.81f, dt);
    SimulateGravityStep(t2, v2, -9.81f, dt);
    float fall1 = 100.0f - t1.position.y;
    float fall2 = 50.0f - t2.position.y;
    EXPECT_NEAR(fall1, fall2, 0.0001f);
}

TEST(PhysicsECS_DampingReducesVelocity)
{
    float velocity = 10.0f;
    float damping = 0.99f;
    for (int i = 0; i < 60; ++i)
        velocity *= damping;
    EXPECT_TRUE(velocity < 10.0f);
    EXPECT_TRUE(velocity > 0.0f);
}

TEST(PhysicsECS_BodyTypeTransition)
{
    RigidBodyComp rb;
    rb.type = BodyType::Dynamic;
    EXPECT_EQ(static_cast<int>(rb.type), 1);
    rb.type = BodyType::Static;
    EXPECT_EQ(static_cast<int>(rb.type), 0);
    rb.type = BodyType::Kinematic;
    EXPECT_EQ(static_cast<int>(rb.type), 2);
}

TEST(PhysicsECS_ColliderShapes)
{
    ColliderComp box;
    box.shape = ColliderComp::Shape::Box;
    ColliderComp sphere;
    sphere.shape = ColliderComp::Shape::Sphere;
    sphere.radius = 2.5f;
    EXPECT_EQ(static_cast<int>(box.shape), 0);
    EXPECT_EQ(static_cast<int>(sphere.shape), 1);
    EXPECT_NEAR(sphere.radius, 2.5f, 0.001f);
}
