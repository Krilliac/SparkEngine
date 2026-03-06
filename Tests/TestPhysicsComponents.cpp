// TestPhysicsComponents.cpp - Tests for physics component data and logic
// Standalone implementations for CI testing (no DirectXMath/Bullet dependency)

#include "TestFramework.h"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>

namespace TestPhysics {

// ============================================================================
// Minimal math types for cross-platform testing
// ============================================================================

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float LengthSq() const { return x * x + y * y + z * z; }
    float DistanceTo(const Vec3& o) const { return (*this - o).Length(); }

    Vec3 Normalized() const {
        float len = Length();
        if (len < 0.0001f) return {0, 0, 0};
        return {x / len, y / len, z / len};
    }

    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
};

// ============================================================================
// Physics enums (mirrors engine types)
// ============================================================================

enum class PhysicsBodyType { Static, Kinematic, Dynamic };

enum class CollisionShapeType { Box, Sphere, Capsule, Cylinder, Cone, Mesh, ConvexHull };

// ============================================================================
// PhysicsMaterial (mirrors Spark::PhysicsMaterial)
// ============================================================================

struct PhysicsMaterial {
    float friction = 0.5f;
    float restitution = 0.3f;
    float linearDamping = 0.1f;
    float angularDamping = 0.1f;
    float density = 1000.0f;  // kg/m^3
    bool isStatic = false;
    std::string name = "Default";

    bool IsValid() const {
        return friction >= 0.0f &&
               restitution >= 0.0f && restitution <= 1.0f &&
               linearDamping >= 0.0f && linearDamping <= 1.0f &&
               angularDamping >= 0.0f && angularDamping <= 1.0f &&
               density > 0.0f;
    }
};

// ============================================================================
// ColliderComponent (mirrors engine ColliderComponent)
// ============================================================================

struct ColliderComponent {
    enum class Shape { Box, Sphere, Capsule, Mesh };

    Shape shape = Shape::Box;
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f;
    float height = 1.0f;
    Vec3 offset{0, 0, 0};

    float ComputeVolume() const {
        const float PI = 3.14159265f;
        switch (shape) {
            case Shape::Box:
                return 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
            case Shape::Sphere:
                return (4.0f / 3.0f) * PI * radius * radius * radius;
            case Shape::Capsule: {
                float cylinderVol = PI * radius * radius * height;
                float sphereVol = (4.0f / 3.0f) * PI * radius * radius * radius;
                return cylinderVol + sphereVol;
            }
            default:
                return 0.0f;
        }
    }
};

// ============================================================================
// RigidBodyComponent (mirrors engine RigidBodyComponent)
// ============================================================================

struct RigidBodyComponent {
    PhysicsBodyType type = PhysicsBodyType::Dynamic;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.1f;
    float angularDamping = 0.1f;
    bool isTrigger = false;
    Vec3 linearVelocity{0, 0, 0};
    Vec3 angularVelocity{0, 0, 0};

    float KineticEnergy() const {
        float speed = linearVelocity.Length();
        return 0.5f * mass * speed * speed;
    }

    Vec3 Momentum() const {
        return linearVelocity * mass;
    }
};

// ============================================================================
// RaycastHit (mirrors engine RaycastHit)
// ============================================================================

struct RaycastHit {
    bool hasHit = false;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 0};
    float distance = 0.0f;
};

// ============================================================================
// AABB for broad-phase collision
// ============================================================================

struct AABB {
    Vec3 min{0, 0, 0};
    Vec3 max{0, 0, 0};

    bool Intersects(const AABB& other) const {
        return (min.x <= other.max.x && max.x >= other.min.x) &&
               (min.y <= other.max.y && max.y >= other.min.y) &&
               (min.z <= other.max.z && max.z >= other.min.z);
    }

    bool Contains(const Vec3& point) const {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    static AABB FromSphere(const Vec3& center, float radius) {
        return {{center.x - radius, center.y - radius, center.z - radius},
                {center.x + radius, center.y + radius, center.z + radius}};
    }

    static AABB FromBox(const Vec3& center, const Vec3& halfExtents) {
        return {{center.x - halfExtents.x, center.y - halfExtents.y, center.z - halfExtents.z},
                {center.x + halfExtents.x, center.y + halfExtents.y, center.z + halfExtents.z}};
    }
};

// ============================================================================
// Simple ray-sphere intersection for testing raycast logic
// ============================================================================

RaycastHit RaycastSphere(const Vec3& rayOrigin, const Vec3& rayDir,
                         const Vec3& sphereCenter, float sphereRadius) {
    RaycastHit hit;
    Vec3 oc = rayOrigin - sphereCenter;
    float a = rayDir.Dot(rayDir);
    float b = 2.0f * oc.Dot(rayDir);
    float c = oc.Dot(oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - 4.0f * a * c;

    if (discriminant < 0.0f) return hit;

    float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
    if (t < 0.0f) {
        t = (-b + std::sqrt(discriminant)) / (2.0f * a);
        if (t < 0.0f) return hit;
    }

    hit.hasHit = true;
    hit.distance = t;
    hit.point = rayOrigin + rayDir * t;
    hit.normal = (hit.point - sphereCenter).Normalized();
    return hit;
}

// ============================================================================
// Collision response helper
// ============================================================================

void ResolveElasticCollision(RigidBodyComponent& a, RigidBodyComponent& b,
                             const Vec3& contactNormal) {
    if (a.type == PhysicsBodyType::Static && b.type == PhysicsBodyType::Static) return;

    Vec3 relativeVelocity = a.linearVelocity - b.linearVelocity;
    float velAlongNormal = relativeVelocity.Dot(contactNormal);

    if (velAlongNormal > 0.0f) return;  // Separating

    float restitution = std::min(a.restitution, b.restitution);
    float invMassA = (a.type == PhysicsBodyType::Static) ? 0.0f : 1.0f / a.mass;
    float invMassB = (b.type == PhysicsBodyType::Static) ? 0.0f : 1.0f / b.mass;
    float totalInvMass = invMassA + invMassB;

    if (totalInvMass < 0.0001f) return;

    float j = -(1.0f + restitution) * velAlongNormal / totalInvMass;

    a.linearVelocity = a.linearVelocity + contactNormal * (j * invMassA);
    b.linearVelocity = b.linearVelocity - contactNormal * (j * invMassB);
}

} // namespace TestPhysics

// ============================================================================
// Tests: PhysicsMaterial
// ============================================================================

TEST(Physics_DefaultMaterialIsValid)
{
    TestPhysics::PhysicsMaterial mat;
    EXPECT_TRUE(mat.IsValid());
    EXPECT_NEAR(mat.friction, 0.5f, 0.001f);
    EXPECT_NEAR(mat.restitution, 0.3f, 0.001f);
    EXPECT_EQ(mat.name, std::string("Default"));
    EXPECT_FALSE(mat.isStatic);
}

TEST(Physics_InvalidMaterialDetected)
{
    TestPhysics::PhysicsMaterial mat;

    mat.restitution = -0.1f;
    EXPECT_FALSE(mat.IsValid());

    mat.restitution = 0.5f;
    mat.density = -1.0f;
    EXPECT_FALSE(mat.IsValid());

    mat.density = 100.0f;
    mat.linearDamping = 2.0f;
    EXPECT_FALSE(mat.IsValid());
}

// ============================================================================
// Tests: ColliderComponent volumes
// ============================================================================

TEST(Physics_BoxColliderVolume)
{
    TestPhysics::ColliderComponent collider;
    collider.shape = TestPhysics::ColliderComponent::Shape::Box;
    collider.halfExtents = {1.0f, 2.0f, 3.0f};

    float volume = collider.ComputeVolume();
    // Volume = 2*1 * 2*2 * 2*3 = 48
    EXPECT_NEAR(volume, 48.0f, 0.01f);
}

TEST(Physics_SphereColliderVolume)
{
    TestPhysics::ColliderComponent collider;
    collider.shape = TestPhysics::ColliderComponent::Shape::Sphere;
    collider.radius = 2.0f;

    float volume = collider.ComputeVolume();
    float expected = (4.0f / 3.0f) * 3.14159265f * 8.0f;  // 4/3 * pi * r^3
    EXPECT_NEAR(volume, expected, 0.1f);
}

TEST(Physics_CapsuleColliderVolume)
{
    TestPhysics::ColliderComponent collider;
    collider.shape = TestPhysics::ColliderComponent::Shape::Capsule;
    collider.radius = 1.0f;
    collider.height = 2.0f;

    float volume = collider.ComputeVolume();
    float PI = 3.14159265f;
    float cylinderVol = PI * 1.0f * 1.0f * 2.0f;
    float sphereVol = (4.0f / 3.0f) * PI * 1.0f;
    EXPECT_NEAR(volume, cylinderVol + sphereVol, 0.1f);
}

TEST(Physics_UnitBoxVolume)
{
    TestPhysics::ColliderComponent collider;
    collider.shape = TestPhysics::ColliderComponent::Shape::Box;
    collider.halfExtents = {0.5f, 0.5f, 0.5f};

    EXPECT_NEAR(collider.ComputeVolume(), 1.0f, 0.001f);
}

// ============================================================================
// Tests: RigidBodyComponent
// ============================================================================

TEST(Physics_RigidBodyDefaults)
{
    TestPhysics::RigidBodyComponent rb;
    EXPECT_EQ(static_cast<int>(rb.type), static_cast<int>(TestPhysics::PhysicsBodyType::Dynamic));
    EXPECT_NEAR(rb.mass, 1.0f, 0.001f);
    EXPECT_FALSE(rb.isTrigger);
    EXPECT_NEAR(rb.linearVelocity.Length(), 0.0f, 0.001f);
}

TEST(Physics_KineticEnergyCalculation)
{
    TestPhysics::RigidBodyComponent rb;
    rb.mass = 2.0f;
    rb.linearVelocity = {3.0f, 0.0f, 4.0f};  // speed = 5

    float ke = rb.KineticEnergy();
    // KE = 0.5 * 2 * 25 = 25
    EXPECT_NEAR(ke, 25.0f, 0.01f);
}

TEST(Physics_MomentumCalculation)
{
    TestPhysics::RigidBodyComponent rb;
    rb.mass = 3.0f;
    rb.linearVelocity = {2.0f, 0.0f, 0.0f};

    TestPhysics::Vec3 p = rb.Momentum();
    EXPECT_NEAR(p.x, 6.0f, 0.001f);
    EXPECT_NEAR(p.y, 0.0f, 0.001f);
    EXPECT_NEAR(p.z, 0.0f, 0.001f);
}

TEST(Physics_StationaryBodyZeroEnergy)
{
    TestPhysics::RigidBodyComponent rb;
    rb.mass = 10.0f;
    rb.linearVelocity = {0, 0, 0};

    EXPECT_NEAR(rb.KineticEnergy(), 0.0f, 0.001f);
    EXPECT_NEAR(rb.Momentum().Length(), 0.0f, 0.001f);
}

// ============================================================================
// Tests: AABB intersection
// ============================================================================

TEST(Physics_AABBIntersection)
{
    TestPhysics::AABB a{{0, 0, 0}, {2, 2, 2}};
    TestPhysics::AABB b{{1, 1, 1}, {3, 3, 3}};

    EXPECT_TRUE(a.Intersects(b));
    EXPECT_TRUE(b.Intersects(a));
}

TEST(Physics_AABBNoIntersection)
{
    TestPhysics::AABB a{{0, 0, 0}, {1, 1, 1}};
    TestPhysics::AABB b{{5, 5, 5}, {6, 6, 6}};

    EXPECT_FALSE(a.Intersects(b));
    EXPECT_FALSE(b.Intersects(a));
}

TEST(Physics_AABBContainsPoint)
{
    TestPhysics::AABB box{{-1, -1, -1}, {1, 1, 1}};

    EXPECT_TRUE(box.Contains({0, 0, 0}));
    EXPECT_TRUE(box.Contains({1, 1, 1}));   // On boundary
    EXPECT_FALSE(box.Contains({2, 0, 0}));
}

TEST(Physics_AABBFromSphere)
{
    TestPhysics::Vec3 center{5, 5, 5};
    float radius = 2.0f;
    auto aabb = TestPhysics::AABB::FromSphere(center, radius);

    EXPECT_NEAR(aabb.min.x, 3.0f, 0.001f);
    EXPECT_NEAR(aabb.max.x, 7.0f, 0.001f);
    EXPECT_TRUE(aabb.Contains(center));
}

TEST(Physics_AABBFromBox)
{
    TestPhysics::Vec3 center{0, 0, 0};
    TestPhysics::Vec3 halfExtents{1, 2, 3};
    auto aabb = TestPhysics::AABB::FromBox(center, halfExtents);

    EXPECT_NEAR(aabb.min.x, -1.0f, 0.001f);
    EXPECT_NEAR(aabb.max.y, 2.0f, 0.001f);
    EXPECT_NEAR(aabb.max.z, 3.0f, 0.001f);
}

// ============================================================================
// Tests: Raycast
// ============================================================================

TEST(Physics_RaycastHitsSphere)
{
    TestPhysics::Vec3 origin{0, 0, 0};
    TestPhysics::Vec3 dir{1, 0, 0};
    TestPhysics::Vec3 sphereCenter{5, 0, 0};
    float sphereRadius = 1.0f;

    auto hit = TestPhysics::RaycastSphere(origin, dir, sphereCenter, sphereRadius);

    EXPECT_TRUE(hit.hasHit);
    EXPECT_NEAR(hit.distance, 4.0f, 0.01f);  // Hit at x=4 (surface of sphere)
    EXPECT_NEAR(hit.point.x, 4.0f, 0.01f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 0.01f);  // Normal points back toward ray
}

TEST(Physics_RaycastMissesSphere)
{
    TestPhysics::Vec3 origin{0, 0, 0};
    TestPhysics::Vec3 dir{0, 1, 0};  // Shooting upward
    TestPhysics::Vec3 sphereCenter{5, 0, 0};  // Sphere is to the right
    float sphereRadius = 1.0f;

    auto hit = TestPhysics::RaycastSphere(origin, dir, sphereCenter, sphereRadius);

    EXPECT_FALSE(hit.hasHit);
}

TEST(Physics_RaycastFromInsideSphere)
{
    TestPhysics::Vec3 origin{5, 0, 0};  // Inside the sphere
    TestPhysics::Vec3 dir{1, 0, 0};
    TestPhysics::Vec3 sphereCenter{5, 0, 0};
    float sphereRadius = 2.0f;

    auto hit = TestPhysics::RaycastSphere(origin, dir, sphereCenter, sphereRadius);

    EXPECT_TRUE(hit.hasHit);
    EXPECT_NEAR(hit.point.x, 7.0f, 0.01f);  // Exits at far side
}

// ============================================================================
// Tests: Collision response
// ============================================================================

TEST(Physics_ElasticCollisionConservesMomentum)
{
    TestPhysics::RigidBodyComponent a;
    a.mass = 1.0f;
    a.linearVelocity = {5, 0, 0};
    a.restitution = 1.0f;

    TestPhysics::RigidBodyComponent b;
    b.mass = 1.0f;
    b.linearVelocity = {-3, 0, 0};
    b.restitution = 1.0f;

    TestPhysics::Vec3 totalMomentumBefore = a.Momentum() + b.Momentum();

    TestPhysics::Vec3 normal{1, 0, 0};
    TestPhysics::ResolveElasticCollision(a, b, normal);

    TestPhysics::Vec3 totalMomentumAfter = a.Momentum() + b.Momentum();

    EXPECT_NEAR(totalMomentumBefore.x, totalMomentumAfter.x, 0.01f);
    EXPECT_NEAR(totalMomentumBefore.y, totalMomentumAfter.y, 0.01f);
    EXPECT_NEAR(totalMomentumBefore.z, totalMomentumAfter.z, 0.01f);
}

TEST(Physics_StaticBodyNotMovedByCollision)
{
    TestPhysics::RigidBodyComponent wall;
    wall.type = TestPhysics::PhysicsBodyType::Static;
    wall.mass = 1000.0f;
    wall.linearVelocity = {0, 0, 0};
    wall.restitution = 0.5f;

    TestPhysics::RigidBodyComponent ball;
    ball.type = TestPhysics::PhysicsBodyType::Dynamic;
    ball.mass = 1.0f;
    ball.linearVelocity = {-5, 0, 0};
    ball.restitution = 0.5f;

    TestPhysics::Vec3 normal{-1, 0, 0};
    TestPhysics::ResolveElasticCollision(wall, ball, normal);

    // Static body should not move
    EXPECT_NEAR(wall.linearVelocity.x, 0.0f, 0.001f);
    EXPECT_NEAR(wall.linearVelocity.y, 0.0f, 0.001f);
    EXPECT_NEAR(wall.linearVelocity.z, 0.0f, 0.001f);
}

TEST(Physics_SeparatingBodiesNotResolved)
{
    TestPhysics::RigidBodyComponent a;
    a.mass = 1.0f;
    a.linearVelocity = {5, 0, 0};  // Moving right
    a.restitution = 1.0f;

    TestPhysics::RigidBodyComponent b;
    b.mass = 1.0f;
    b.linearVelocity = {-5, 0, 0};  // Moving left
    b.restitution = 1.0f;

    // Normal points from a to b along +x, relative velocity along normal is positive
    // meaning bodies are already separating
    TestPhysics::Vec3 normal{1, 0, 0};
    float velBefore_a = a.linearVelocity.x;
    float velBefore_b = b.linearVelocity.x;

    TestPhysics::ResolveElasticCollision(a, b, normal);

    // Velocities should be unchanged since bodies are separating
    EXPECT_NEAR(a.linearVelocity.x, velBefore_a, 0.001f);
    EXPECT_NEAR(b.linearVelocity.x, velBefore_b, 0.001f);
}

TEST(Physics_TwoStaticBodiesNoCollision)
{
    TestPhysics::RigidBodyComponent a;
    a.type = TestPhysics::PhysicsBodyType::Static;
    a.linearVelocity = {0, 0, 0};

    TestPhysics::RigidBodyComponent b;
    b.type = TestPhysics::PhysicsBodyType::Static;
    b.linearVelocity = {0, 0, 0};

    TestPhysics::Vec3 normal{1, 0, 0};
    TestPhysics::ResolveElasticCollision(a, b, normal);

    EXPECT_NEAR(a.linearVelocity.Length(), 0.0f, 0.001f);
    EXPECT_NEAR(b.linearVelocity.Length(), 0.0f, 0.001f);
}
