// TestPhysicsSystem.cpp - Tests for physics system configuration and state
// Standalone implementations for CI testing (no Jolt Physics dependency)

#include "TestFramework.h"
#include "TestCommonMath.h"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace TestPhysics
{

    using TestMath::Vec3;

    enum class BodyType
    {
        Static,
        Dynamic,
        Kinematic
    };

    enum class ShapeType
    {
        Box,
        Sphere,
        Capsule,
        Plane
    };

    // WARNING: This struct was originally named "PhysicsMaterial", which caused an
    // ASAN segfault due to constructor aliasing with the engine's ::PhysicsMaterial
    // (included via Physics/PhysicsTypes.h at line ~502 of this same file).
    //
    // What happened:
    //   This struct has layout: [std::string name (32B)] [float friction] [float restitution] [float density]
    //   The engine's ::PhysicsMaterial has: [float friction] [float restitution=0.1f] [float linearDamping=0.1f] ...
    //     [std::string name at the END]
    //
    //   Under GCC + AddressSanitizer (-fsanitize=address), the compiler aliased the
    //   default-member-initializer code for the two same-named structs in this TU.
    //   The engine's float defaults (restitution=0.1f → 0x3DCCCCCD) were written at
    //   offset 4-11, which in THIS struct's layout overlaps std::string::_M_string_length
    //   (offset 8). The string then believed its length was 0x3DCCCCCD3DCCCCCE (~4.4
    //   exabytes), and the next std::string copy triggered an allocation-size-too-big abort.
    //
    // Fix: Renamed to TestMaterial. If you ever see a mysterious "allocation-size-too-big"
    // ASAN error where the size looks like IEEE 754 float bit patterns, check for two
    // differently-laid-out structs with the same name in the same translation unit.
    struct TestMaterial
    {
        std::string name;
        float friction = 0.5f;
        float restitution = 0.3f;
        float density = 1.0f;
    };

    struct RigidBodyDesc
    {
        std::string name;
        BodyType type = BodyType::Dynamic;
        ShapeType shape = ShapeType::Box;
        Vec3 position;
        Vec3 halfExtents = {0.5f, 0.5f, 0.5f};
        float mass = 1.0f;
        TestMaterial material;
    };

    struct RigidBody
    {
        int id = -1;
        RigidBodyDesc desc;
        Vec3 velocity;
        Vec3 angularVelocity;
        bool active = true;
    };

    struct PhysicsMetrics
    {
        int activeRigidBodies = 0;
        int totalRigidBodies = 0;
        int collisionPairs = 0;
        float simulationTime = 0.0f;
        int substeps = 0;
    };

    struct RaycastResult
    {
        bool hit = false;
        Vec3 hitPoint;
        Vec3 hitNormal;
        float distance = 0.0f;
        int bodyId = -1;
    };

    class PhysicsSystem
    {
      public:
        void Initialize()
        {
            m_gravity = {0.0f, -9.81f, 0.0f};
            m_timeStep = 1.0f / 60.0f;
            m_maxSubsteps = 10;
            m_interpolationEnabled = false;
            m_interpolationAlpha = 0.0f;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_bodies.clear();
            m_materials.clear();
            m_nextId = 0;
            m_initialized = false;
        }

        bool IsInitialized() const { return m_initialized; }

        // Gravity
        void SetGravity(const Vec3& g) { m_gravity = g; }
        Vec3 GetGravity() const { return m_gravity; }

        // Timestep
        void SetTimeStep(float ts) { m_timeStep = ts; }
        float GetTimeStep() const { return m_timeStep; }
        void SetMaxSubsteps(int s) { m_maxSubsteps = std::max(1, s); }
        int GetMaxSubsteps() const { return m_maxSubsteps; }

        // Interpolation
        void SetInterpolationEnabled(bool e) { m_interpolationEnabled = e; }
        bool IsInterpolationEnabled() const { return m_interpolationEnabled; }
        float GetInterpolationAlpha() const { return m_interpolationAlpha; }

        // Body management
        int CreateBody(const RigidBodyDesc& desc)
        {
            RigidBody body;
            body.id = m_nextId++;
            body.desc = desc;
            body.active = (desc.type == BodyType::Dynamic);
            m_bodies.push_back(body);
            return body.id;
        }

        void RemoveBody(int id)
        {
            m_bodies.erase(
                std::remove_if(m_bodies.begin(), m_bodies.end(), [id](const RigidBody& b) { return b.id == id; }),
                m_bodies.end());
        }

        const RigidBody* GetBody(int id) const
        {
            for (const auto& b : m_bodies)
            {
                if (b.id == id)
                    return &b;
            }
            return nullptr;
        }

        int GetBodyCount() const { return static_cast<int>(m_bodies.size()); }

        int GetActiveBodyCount() const
        {
            int count = 0;
            for (const auto& b : m_bodies)
            {
                if (b.active)
                    count++;
            }
            return count;
        }

        // Material registration
        void RegisterMaterial(const TestMaterial& mat) { m_materials[mat.name] = mat; }

        const TestMaterial* GetMaterial(const std::string& name) const
        {
            auto it = m_materials.find(name);
            return (it != m_materials.end()) ? &it->second : nullptr;
        }

        bool HasMaterial(const std::string& name) const { return m_materials.count(name) > 0; }

        // Simplified simulation step
        void Update(float deltaTime)
        {
            if (!m_initialized)
                return;

            int steps = static_cast<int>(std::ceil(deltaTime / m_timeStep));
            steps = std::min(steps, m_maxSubsteps);

            for (auto& body : m_bodies)
            {
                if (body.desc.type != BodyType::Dynamic || !body.active)
                    continue;

                for (int s = 0; s < steps; ++s)
                {
                    body.velocity.x += m_gravity.x * m_timeStep;
                    body.velocity.y += m_gravity.y * m_timeStep;
                    body.velocity.z += m_gravity.z * m_timeStep;

                    body.desc.position.x += body.velocity.x * m_timeStep;
                    body.desc.position.y += body.velocity.y * m_timeStep;
                    body.desc.position.z += body.velocity.z * m_timeStep;
                }
            }

            if (m_interpolationEnabled)
                m_interpolationAlpha = (deltaTime / m_timeStep) - std::floor(deltaTime / m_timeStep);
        }

        // Simple raycast (checks AABB overlap for testing)
        RaycastResult Raycast(const Vec3& origin, const Vec3& direction, float maxDist) const
        {
            RaycastResult result;
            (void)direction;
            (void)maxDist;

            // Simplified: find closest body to origin
            float closestDist = maxDist;
            for (const auto& body : m_bodies)
            {
                float dx = body.desc.position.x - origin.x;
                float dy = body.desc.position.y - origin.y;
                float dz = body.desc.position.z - origin.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

                if (dist < closestDist)
                {
                    closestDist = dist;
                    result.hit = true;
                    result.hitPoint = body.desc.position;
                    result.distance = dist;
                    result.bodyId = body.id;
                }
            }

            return result;
        }

        PhysicsMetrics GetMetrics() const
        {
            PhysicsMetrics m;
            m.totalRigidBodies = GetBodyCount();
            m.activeRigidBodies = GetActiveBodyCount();
            return m;
        }

      private:
        Vec3 m_gravity = {0.0f, -9.81f, 0.0f};
        float m_timeStep = 1.0f / 60.0f;
        int m_maxSubsteps = 10;
        bool m_interpolationEnabled = false;
        float m_interpolationAlpha = 0.0f;
        bool m_initialized = false;
        std::vector<RigidBody> m_bodies;
        std::unordered_map<std::string, TestMaterial> m_materials;
        int m_nextId = 0;
    };

} // namespace TestPhysics

// =============================================================================
// Tests
// =============================================================================

TEST(Physics_DefaultGravity)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    auto g = physics.GetGravity();
    EXPECT_NEAR(g.x, 0.0f, 0.001f);
    EXPECT_NEAR(g.y, -9.81f, 0.001f);
    EXPECT_NEAR(g.z, 0.0f, 0.001f);
}

TEST(Physics_SetGravity)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    physics.SetGravity({0.0f, 0.0f, 0.0f});
    auto g = physics.GetGravity();
    EXPECT_NEAR(g.x, 0.0f, 0.001f);
    EXPECT_NEAR(g.y, 0.0f, 0.001f);
    EXPECT_NEAR(g.z, 0.0f, 0.001f);

    physics.SetGravity({0.0f, -20.0f, 0.0f});
    g = physics.GetGravity();
    EXPECT_NEAR(g.y, -20.0f, 0.001f);
}

TEST(Physics_TimeStep)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    EXPECT_NEAR(physics.GetTimeStep(), 1.0f / 60.0f, 0.0001f);

    physics.SetTimeStep(1.0f / 120.0f);
    EXPECT_NEAR(physics.GetTimeStep(), 1.0f / 120.0f, 0.0001f);
}

TEST(Physics_MaxSubsteps)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    EXPECT_EQ(physics.GetMaxSubsteps(), 10);

    physics.SetMaxSubsteps(5);
    EXPECT_EQ(physics.GetMaxSubsteps(), 5);

    // Should clamp to minimum of 1
    physics.SetMaxSubsteps(0);
    EXPECT_GE(physics.GetMaxSubsteps(), 1);
}

TEST(Physics_Interpolation)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    EXPECT_FALSE(physics.IsInterpolationEnabled());
    EXPECT_NEAR(physics.GetInterpolationAlpha(), 0.0f, 0.001f);

    physics.SetInterpolationEnabled(true);
    EXPECT_TRUE(physics.IsInterpolationEnabled());
}

TEST(Physics_CreateBody)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::RigidBodyDesc desc;
    desc.name = "Box1";
    desc.type = TestPhysics::BodyType::Dynamic;
    desc.shape = TestPhysics::ShapeType::Box;
    desc.position = {0.0f, 10.0f, 0.0f};
    desc.mass = 1.0f;

    int id = physics.CreateBody(desc);
    EXPECT_GE(id, 0);
    EXPECT_EQ(physics.GetBodyCount(), 1);

    const auto* body = physics.GetBody(id);
    ASSERT_TRUE(body != nullptr);
    EXPECT_NEAR(body->desc.position.y, 10.0f, 0.001f);
    EXPECT_TRUE(body->active);
}

TEST(Physics_RemoveBody)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::RigidBodyDesc desc;
    desc.name = "Temp";

    int id = physics.CreateBody(desc);
    EXPECT_EQ(physics.GetBodyCount(), 1);

    physics.RemoveBody(id);
    EXPECT_EQ(physics.GetBodyCount(), 0);
    EXPECT_TRUE(physics.GetBody(id) == nullptr);
}

TEST(Physics_StaticBodyNotActive)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::RigidBodyDesc desc;
    desc.name = "Floor";
    desc.type = TestPhysics::BodyType::Static;
    desc.shape = TestPhysics::ShapeType::Plane;

    int id = physics.CreateBody(desc);
    const auto* body = physics.GetBody(id);
    EXPECT_FALSE(body->active);
    EXPECT_EQ(physics.GetActiveBodyCount(), 0);
}

TEST(Physics_SimulationStep)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::RigidBodyDesc desc;
    desc.name = "FallingBox";
    desc.type = TestPhysics::BodyType::Dynamic;
    desc.position = {0.0f, 100.0f, 0.0f};

    int id = physics.CreateBody(desc);

    // Step 1 second of simulation
    physics.Update(1.0f);

    const auto* body = physics.GetBody(id);
    // Body should have fallen (y < 100)
    EXPECT_TRUE(body->desc.position.y < 100.0f);
    // Velocity should be negative (falling)
    EXPECT_TRUE(body->velocity.y < 0.0f);
}

TEST(Physics_ZeroGravityNoFall)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();
    physics.SetGravity({0.0f, 0.0f, 0.0f});

    TestPhysics::RigidBodyDesc desc;
    desc.name = "Floating";
    desc.type = TestPhysics::BodyType::Dynamic;
    desc.position = {0.0f, 50.0f, 0.0f};

    int id = physics.CreateBody(desc);
    physics.Update(1.0f);

    const auto* body = physics.GetBody(id);
    EXPECT_NEAR(body->desc.position.y, 50.0f, 0.001f);
}

TEST(Physics_MaterialRegistration)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::TestMaterial mat;
    mat.name = "Rubber";
    mat.friction = 0.8f;
    mat.restitution = 0.9f;
    mat.density = 1.1f;

    physics.RegisterMaterial(mat);
    EXPECT_TRUE(physics.HasMaterial("Rubber"));

    const auto* m = physics.GetMaterial("Rubber");
    ASSERT_TRUE(m != nullptr);
    EXPECT_NEAR(m->friction, 0.8f, 0.001f);
    EXPECT_NEAR(m->restitution, 0.9f, 0.001f);

    EXPECT_FALSE(physics.HasMaterial("NonExistent"));
}

TEST(Physics_Raycast)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::RigidBodyDesc desc;
    desc.name = "Target";
    desc.position = {5.0f, 0.0f, 0.0f};
    int id = physics.CreateBody(desc);

    auto result = physics.Raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.bodyId, id);
    EXPECT_NEAR(result.distance, 5.0f, 0.001f);
}

TEST(Physics_RaycastMiss)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    // No bodies — raycast should miss
    auto result = physics.Raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    EXPECT_FALSE(result.hit);
}

TEST(Physics_Metrics)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();

    TestPhysics::RigidBodyDesc dynamic;
    dynamic.name = "Dyn";
    dynamic.type = TestPhysics::BodyType::Dynamic;

    TestPhysics::RigidBodyDesc staticBody;
    staticBody.name = "Stat";
    staticBody.type = TestPhysics::BodyType::Static;

    physics.CreateBody(dynamic);
    physics.CreateBody(staticBody);

    auto m = physics.GetMetrics();
    EXPECT_EQ(m.totalRigidBodies, 2);
    EXPECT_EQ(m.activeRigidBodies, 1);
}

TEST(Physics_Shutdown)
{
    TestPhysics::PhysicsSystem physics;
    physics.Initialize();
    EXPECT_TRUE(physics.IsInitialized());

    TestPhysics::RigidBodyDesc desc;
    desc.name = "Body";
    physics.CreateBody(desc);

    physics.Shutdown();
    EXPECT_FALSE(physics.IsInitialized());
    EXPECT_EQ(physics.GetBodyCount(), 0);
}

// =============================================================================
// PhysicsTypes.h tests (real types from the engine, no Jolt dependency)
//
// NOTE: This #include brings ::PhysicsMaterial into this TU alongside
// TestPhysics::TestMaterial (above). These two structs have completely
// different field layouts. See the comment on TestMaterial for why the
// test struct was renamed — having two "PhysicsMaterial" types in the
// same TU caused GCC+ASAN to alias their default-member-initializer
// code, corrupting the test struct's std::string with float bit patterns.
// =============================================================================

#include "Physics/PhysicsTypes.h"
#include "Physics/SoftBodyPhysics.h"

TEST(PhysicsTypes_BodyTypeStringRoundtrip)
{
    EXPECT_EQ(PhysicsBodyTypeToString(PhysicsBodyType::Static), std::string("Static"));
    EXPECT_EQ(PhysicsBodyTypeToString(PhysicsBodyType::Dynamic), std::string("Dynamic"));
    EXPECT_EQ(PhysicsBodyTypeToString(PhysicsBodyType::Kinematic), std::string("Kinematic"));
    EXPECT_TRUE(StringToPhysicsBodyType("Static") == PhysicsBodyType::Static);
    EXPECT_TRUE(StringToPhysicsBodyType("Dynamic") == PhysicsBodyType::Dynamic);
    EXPECT_TRUE(StringToPhysicsBodyType("Kinematic") == PhysicsBodyType::Kinematic);
}

TEST(PhysicsTypes_ShapeTypeStringRoundtrip)
{
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Box), std::string("Box"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Sphere), std::string("Sphere"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Capsule), std::string("Capsule"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Heightfield), std::string("Heightfield"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::TaperedCapsule), std::string("TaperedCapsule"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::TaperedCylinder), std::string("TaperedCylinder"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Plane), std::string("Plane"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Scaled), std::string("Scaled"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::OffsetCenterOfMass), std::string("OffsetCenterOfMass"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::MutableCompound), std::string("MutableCompound"));
    EXPECT_EQ(CollisionShapeTypeToString(CollisionShapeType::Empty), std::string("Empty"));
    EXPECT_TRUE(StringToCollisionShapeType("Sphere") == CollisionShapeType::Sphere);
    EXPECT_TRUE(StringToCollisionShapeType("Heightfield") == CollisionShapeType::Heightfield);
}

TEST(PhysicsTypes_ConstraintTypeStringRoundtrip)
{
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::Hinge), std::string("Hinge"));
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::Gear), std::string("Gear"));
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::RackAndPinion), std::string("RackAndPinion"));
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::Path), std::string("Path"));
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::Distance), std::string("Distance"));
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::Cone), std::string("Cone"));
    EXPECT_EQ(ConstraintTypeToString(ConstraintType::Pulley), std::string("Pulley"));
    EXPECT_TRUE(StringToConstraintType("Gear") == ConstraintType::Gear);
    EXPECT_TRUE(StringToConstraintType("Path") == ConstraintType::Path);
}

TEST(PhysicsTypes_BodyDescDefaults)
{
    PhysicsBodyDesc desc;
    EXPECT_TRUE(desc.type == PhysicsBodyType::Dynamic);
    EXPECT_NEAR(desc.mass, 1.0f, 0.01f);
    EXPECT_NEAR(desc.gravityFactor, 1.0f, 0.01f);
    EXPECT_NEAR(desc.maxLinearVelocity, 500.0f, 0.01f);
    EXPECT_NEAR(desc.maxAngularVelocity, 47.12f, 0.1f);
    EXPECT_EQ(desc.collisionGroup, static_cast<uint16_t>(1));
    EXPECT_EQ(desc.collisionMask, static_cast<uint16_t>(0xFFFF));
    EXPECT_TRUE(desc.motionQuality == MotionQuality::Discrete);
    EXPECT_TRUE(desc.allowedDOFs == AllowedDOFs::All);
    EXPECT_FALSE(desc.isTrigger);
    EXPECT_FALSE(desc.isKinematic);
    EXPECT_TRUE(desc.name.empty());
    EXPECT_EQ(desc.entityId, 0u);
}

TEST(PhysicsTypes_CollisionGroupDescDefaults)
{
    CollisionGroupDesc group;
    EXPECT_EQ(group.groupFilterID, 0u);
    EXPECT_EQ(group.groupID, 0u);
    EXPECT_EQ(group.subGroupID, 0u);

    // Verify embedded in body desc
    PhysicsBodyDesc desc;
    EXPECT_EQ(desc.collisionGroupDesc.groupFilterID, 0u);
}

TEST(PhysicsTypes_ShapeDescDefaults)
{
    CollisionShapeDesc desc;
    EXPECT_TRUE(desc.type == CollisionShapeType::Box);
    EXPECT_NEAR(desc.radius, 0.5f, 0.01f);
    EXPECT_NEAR(desc.height, 1.0f, 0.01f);
    EXPECT_NEAR(desc.scale.x, 1.0f, 0.01f);
    EXPECT_NEAR(desc.scale.y, 1.0f, 0.01f);
    EXPECT_NEAR(desc.scale.z, 1.0f, 0.01f);
    EXPECT_EQ(desc.heightfieldSamples, 0u);
}

TEST(PhysicsTypes_MaterialDefaults)
{
    PhysicsMaterial mat;
    EXPECT_NEAR(mat.friction, 0.5f, 0.01f);
    EXPECT_NEAR(mat.restitution, 0.1f, 0.01f);
    EXPECT_NEAR(mat.linearDamping, 0.1f, 0.01f);
    EXPECT_NEAR(mat.angularDamping, 0.1f, 0.01f);
    EXPECT_NEAR(mat.density, 1.0f, 0.01f);
    EXPECT_FALSE(mat.isStatic);
    EXPECT_TRUE(mat.name.empty());
}

TEST(PhysicsTypes_MutableSubShapeDescDefaults)
{
    MutableSubShapeDesc sub;
    EXPECT_TRUE(sub.shape.type == CollisionShapeType::Box);
    EXPECT_NEAR(sub.position.x, 0.0f, 0.01f);
    EXPECT_NEAR(sub.rotation.x, 0.0f, 0.01f);
    EXPECT_EQ(sub.userData, 0u);
}

TEST(PhysicsTypes_MotorAndSpringDefaults)
{
    PhysicsMotorSettings motor;
    EXPECT_NEAR(motor.spring.frequency, 1.0f, 0.01f);
    EXPECT_NEAR(motor.spring.damping, 0.5f, 0.01f);
    EXPECT_TRUE(motor.spring.useFrequencyMode);

    PhysicsSpringSettings spring;
    EXPECT_TRUE(spring.useFrequencyMode);
    EXPECT_NEAR(spring.frequency, 1.0f, 0.01f);
    EXPECT_NEAR(spring.stiffness, 100.0f, 0.01f);
}

TEST(PhysicsTypes_AllowedDOFs_Bitfield)
{
    // Verify 2D mode restricts to X/Y translation + Z rotation
    auto dofs2D = static_cast<uint8_t>(AllowedDOFs::Plane2D);
    auto transX = static_cast<uint8_t>(AllowedDOFs::TranslationX);
    auto transY = static_cast<uint8_t>(AllowedDOFs::TranslationY);
    auto rotZ = static_cast<uint8_t>(AllowedDOFs::RotationZ);
    EXPECT_TRUE((dofs2D & transX) != 0);
    EXPECT_TRUE((dofs2D & transY) != 0);
    EXPECT_TRUE((dofs2D & rotZ) != 0);
}

TEST(PhysicsTypes_VehicleDescDefaults)
{
    VehicleDesc vDesc;
    EXPECT_TRUE(vDesc.type == PhysicsVehicleType::Wheeled);
    EXPECT_TRUE(vDesc.wheels.empty());
    EXPECT_NEAR(vDesc.maxEngineTorque, 500.0f, 1.0f);
    EXPECT_NEAR(vDesc.minRPM, 1000.0f, 1.0f);
    EXPECT_NEAR(vDesc.maxRPM, 6000.0f, 1.0f);
    EXPECT_EQ(vDesc.gearRatios.size(), 5u);
}

TEST(PhysicsTypes_RagdollDescDefaults)
{
    RagdollPartDesc part;
    EXPECT_EQ(part.parentIndex, -1);
    EXPECT_TRUE(part.constraintType == ConstraintType::ConeTwist);
    EXPECT_NEAR(part.swingLimit1, 0.5f, 0.01f);
    EXPECT_NEAR(part.twistLimit, 0.3f, 0.01f);
}

TEST(PhysicsTypes_ContactInfoDefaults)
{
    ContactInfo info;
    EXPECT_TRUE(info.bodyA == nullptr);
    EXPECT_TRUE(info.bodyB == nullptr);
    EXPECT_NEAR(info.penetrationDepth, 0.0f, 0.01f);
    EXPECT_EQ(info.entityIdA, 0u);
    EXPECT_EQ(info.entityIdB, 0u);
}

TEST(PhysicsTypes_RaycastHitDefaults)
{
    RaycastHit hit;
    EXPECT_FALSE(hit.hasHit);
    EXPECT_TRUE(hit.body == nullptr);
    EXPECT_NEAR(hit.distance, 0.0f, 0.01f);
    EXPECT_EQ(hit.entityId, 0u);
}
