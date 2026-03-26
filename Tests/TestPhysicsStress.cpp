// TestPhysicsStress.cpp - Stress/adversarial tests for the physics subsystem
// Standalone implementations for CI testing (no Jolt/DirectXMath dependency)

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Destruction/DestructionSystem.h"
#include <cmath>
#include <limits>
#include <vector>

namespace TestPhysicsStress
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

        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        float LengthSq() const { return x * x + y * y + z * z; }

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
    // Physics body for stress testing
    // ============================================================================

    enum class PhysicsBodyType
    {
        Static,
        Kinematic,
        Dynamic
    };

    struct PhysicsBody
    {
        Vec3 position{0, 0, 0};
        Vec3 velocity{0, 0, 0};
        float mass = 1.0f;
        PhysicsBodyType type = PhysicsBodyType::Dynamic;

        float KineticEnergy() const
        {
            float speed = velocity.Length();
            if (mass <= 0.0f || std::isnan(mass) || std::isinf(mass))
                return 0.0f;
            float ke = 0.5f * mass * speed * speed;
            return ke;
        }

        Vec3 Momentum() const
        {
            if (mass <= 0.0f || std::isnan(mass) || std::isinf(mass))
                return {0, 0, 0};
            return velocity * mass;
        }
    };

    // ============================================================================
    // Sphere collider
    // ============================================================================

    struct Sphere
    {
        Vec3 center{0, 0, 0};
        float radius = 1.0f;

        float Volume() const
        {
            const float PI = 3.14159265f;
            float r = std::abs(radius);
            return (4.0f / 3.0f) * PI * r * r * r;
        }
    };

    // ============================================================================
    // Box collider
    // ============================================================================

    struct Box
    {
        Vec3 center{0, 0, 0};
        Vec3 halfExtents{0.5f, 0.5f, 0.5f};

        float Volume() const
        {
            float ex = std::abs(halfExtents.x);
            float ey = std::abs(halfExtents.y);
            float ez = std::abs(halfExtents.z);
            return 8.0f * ex * ey * ez;
        }
    };

    // ============================================================================
    // AABB for broad-phase collision
    // ============================================================================

    struct AABB
    {
        Vec3 min{0, 0, 0};
        Vec3 max{0, 0, 0};

        bool Intersects(const AABB& other) const
        {
            return (min.x <= other.max.x && max.x >= other.min.x) && (min.y <= other.max.y && max.y >= other.min.y) &&
                   (min.z <= other.max.z && max.z >= other.min.z);
        }

        bool Contains(const Vec3& point) const
        {
            return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y && point.z >= min.z &&
                   point.z <= max.z;
        }
    };

    // ============================================================================
    // Collision normal computation for two spheres
    // ============================================================================

    Vec3 ComputeCollisionNormal(const Sphere& a, const Sphere& b)
    {
        Vec3 diff = b.center - a.center;
        float len = diff.Length();
        if (len < 0.0001f)
            return {0, 1, 0}; // Degenerate: return a safe default
        return diff.Normalized();
    }

    // ============================================================================
    // Raycast against sphere
    // ============================================================================

    struct RaycastHit
    {
        bool hasHit = false;
        Vec3 point{0, 0, 0};
        Vec3 normal{0, 0, 0};
        float distance = 0.0f;
    };

    RaycastHit RaycastSphere(const Vec3& origin, const Vec3& dir, float maxDist, const Vec3& sphereCenter,
                             float sphereRadius)
    {
        RaycastHit hit;

        // Guard against zero-length direction
        float dirLen = dir.Length();
        if (dirLen < 0.0001f)
            return hit;

        // Guard against negative max distance
        if (maxDist < 0.0f)
            return hit;

        Vec3 normDir = dir.Normalized();
        Vec3 oc = origin - sphereCenter;
        float a = normDir.Dot(normDir);
        float b = 2.0f * oc.Dot(normDir);
        float c = oc.Dot(oc) - sphereRadius * sphereRadius;
        float discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0.0f)
            return hit;

        float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
        if (t < 0.0f)
        {
            t = (-b + std::sqrt(discriminant)) / (2.0f * a);
            if (t < 0.0f)
                return hit;
        }

        if (t > maxDist)
            return hit;

        hit.hasHit = true;
        hit.distance = t;
        hit.point = origin + normDir * t;
        hit.normal = (hit.point - sphereCenter).Normalized();
        return hit;
    }

} // namespace TestPhysicsStress

// ============================================================================
// Test 1: NaN Injection
// ============================================================================

TEST(PhysicsStress_NaNInjection)
{
    float nan = std::numeric_limits<float>::quiet_NaN();

    // Body with NaN position — momentum uses velocity * mass, not position
    TestPhysicsStress::PhysicsBody body;
    body.position = {nan, nan, nan};
    body.velocity = {1.0f, 0.0f, 0.0f};
    body.mass = 1.0f;

    TestPhysicsStress::Vec3 mom = body.Momentum();
    EXPECT_NEAR(mom.x, 1.0f, 0.001f);

    // Body with NaN velocity — KE uses velocity.Length() which will be NaN
    TestPhysicsStress::PhysicsBody body2;
    body2.velocity = {nan, 0.0f, 0.0f};
    body2.mass = 1.0f;
    float ke = body2.KineticEnergy();
    // NaN propagates through sqrt: verify no crash
    EXPECT_TRUE(std::isnan(ke) || ke >= 0.0f);

    // Body with NaN mass — safe guard returns 0
    TestPhysicsStress::PhysicsBody body3;
    body3.velocity = {5.0f, 0.0f, 0.0f};
    body3.mass = nan;
    float ke3 = body3.KineticEnergy();
    EXPECT_NEAR(ke3, 0.0f, 0.001f);

    TestPhysicsStress::Vec3 mom3 = body3.Momentum();
    EXPECT_NEAR(mom3.x, 0.0f, 0.001f);
}

// ============================================================================
// Test 2: Zero/Negative Mass Body
// ============================================================================

TEST(PhysicsStress_ZeroMassBody)
{
    // Zero mass: should not divide by zero in momentum/KE
    TestPhysicsStress::PhysicsBody body;
    body.mass = 0.0f;
    body.velocity = {10.0f, 0.0f, 0.0f};

    float ke = body.KineticEnergy();
    EXPECT_NEAR(ke, 0.0f, 0.001f);

    TestPhysicsStress::Vec3 mom = body.Momentum();
    EXPECT_NEAR(mom.x, 0.0f, 0.001f);

    // Negative mass: should also be handled safely
    body.mass = -5.0f;
    ke = body.KineticEnergy();
    EXPECT_NEAR(ke, 0.0f, 0.001f);

    mom = body.Momentum();
    EXPECT_NEAR(mom.x, 0.0f, 0.001f);
}

// ============================================================================
// Test 3: Extreme Velocity
// ============================================================================

TEST(PhysicsStress_ExtremeVelocity)
{
    TestPhysicsStress::PhysicsBody body;
    body.mass = 1.0f;
    body.velocity = {1e30f, 0.0f, 0.0f};

    float ke = body.KineticEnergy();
    // 0.5 * 1 * (1e30)^2 = 5e59 which overflows to inf
    // Verify no crash; result is either inf or a very large number
    EXPECT_TRUE(std::isinf(ke) || ke > 1e30);

    // Momentum should also be extreme but valid
    TestPhysicsStress::Vec3 mom = body.Momentum();
    EXPECT_TRUE(std::isinf(mom.x) || mom.x > 1e20f);
}

// ============================================================================
// Test 4: Massive Body Count
// ============================================================================

TEST(PhysicsStress_MassiveBodyCount)
{
    std::vector<TestPhysicsStress::PhysicsBody> bodies;
    bodies.reserve(10000);

    for (int i = 0; i < 10000; i++)
    {
        TestPhysicsStress::PhysicsBody b;
        b.position = {static_cast<float>(i), 0.0f, 0.0f};
        b.velocity = {1.0f, 0.0f, 0.0f};
        b.mass = 1.0f;
        bodies.push_back(b);
    }

    EXPECT_EQ(static_cast<int>(bodies.size()), 10000);

    // Verify all bodies have valid KE
    float totalKE = 0.0f;
    for (const auto& b : bodies)
    {
        totalKE += b.KineticEnergy();
    }
    // Each body: KE = 0.5 * 1 * 1 = 0.5; total = 5000
    EXPECT_NEAR(totalKE, 5000.0f, 1.0f);
}

// ============================================================================
// Test 5: Identical Position Collision
// ============================================================================

TEST(PhysicsStress_IdenticalPositionCollision)
{
    TestPhysicsStress::Sphere a;
    a.center = {5.0f, 5.0f, 5.0f};
    a.radius = 1.0f;

    TestPhysicsStress::Sphere b;
    b.center = {5.0f, 5.0f, 5.0f}; // Exact same position
    b.radius = 1.0f;

    TestPhysicsStress::Vec3 normal = TestPhysicsStress::ComputeCollisionNormal(a, b);

    // Should return a safe default normal, not NaN
    EXPECT_FALSE(std::isnan(normal.x));
    EXPECT_FALSE(std::isnan(normal.y));
    EXPECT_FALSE(std::isnan(normal.z));

    // Degenerate case returns (0,1,0) as fallback
    float len = normal.Length();
    EXPECT_NEAR(len, 1.0f, 0.01f);
}

// ============================================================================
// Test 6: Zero Size Collider
// ============================================================================

TEST(PhysicsStress_ZeroSizeCollider)
{
    // Sphere with radius 0
    TestPhysicsStress::Sphere sphere;
    sphere.center = {0, 0, 0};
    sphere.radius = 0.0f;

    float vol = sphere.Volume();
    EXPECT_NEAR(vol, 0.0f, 0.001f);

    // Box with zero extents
    TestPhysicsStress::Box box;
    box.center = {0, 0, 0};
    box.halfExtents = {0.0f, 0.0f, 0.0f};

    float boxVol = box.Volume();
    EXPECT_NEAR(boxVol, 0.0f, 0.001f);
}

// ============================================================================
// Test 7: Negative Dimension Collider
// ============================================================================

TEST(PhysicsStress_NegativeDimensionCollider)
{
    // Sphere with negative radius — volume should use abs(radius)
    TestPhysicsStress::Sphere sphere;
    sphere.radius = -2.0f;

    float vol = sphere.Volume();
    float expected = (4.0f / 3.0f) * 3.14159265f * 8.0f; // abs(-2)^3 = 8
    EXPECT_NEAR(vol, expected, 0.1f);
    EXPECT_TRUE(vol >= 0.0f);

    // Box with negative extents — volume should use abs
    TestPhysicsStress::Box box;
    box.halfExtents = {-1.0f, -2.0f, -3.0f};

    float boxVol = box.Volume();
    EXPECT_NEAR(boxVol, 48.0f, 0.01f); // 8 * 1 * 2 * 3
    EXPECT_TRUE(boxVol >= 0.0f);
}

// ============================================================================
// Test 8: Rapid Create/Destroy
// ============================================================================

TEST(PhysicsStress_RapidCreateDestroy)
{
    for (int i = 0; i < 1000; i++)
    {
        auto body = std::make_unique<TestPhysicsStress::PhysicsBody>();
        body->position = {static_cast<float>(i), 0.0f, 0.0f};
        body->velocity = {1.0f, 2.0f, 3.0f};
        body->mass = 1.0f;

        // Compute something to ensure the body is actually used
        float ke = body->KineticEnergy();
        EXPECT_TRUE(ke > 0.0f);

        // Body is destroyed at end of scope
    }

    // If we get here without crash, the test passes
    EXPECT_TRUE(true);
}

// ============================================================================
// Test 9: Destruction Massive Damage
// ============================================================================

TEST(PhysicsStress_DestructionMassiveDamage)
{
    Spark::DestructibleComponent comp;
    comp.health = 100.0f;
    comp.maxHealth = 100.0f;

    // Apply absurdly large damage
    bool destroyed = comp.ApplyDamage(1e30f);
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(comp.isDestroyed);
    EXPECT_NEAR(comp.health, 0.0f, 0.001f);
}

// ============================================================================
// Test 10: Destruction Zero Damage
// ============================================================================

TEST(PhysicsStress_DestructionZeroDamage)
{
    Spark::DestructibleComponent comp;
    comp.health = 100.0f;
    comp.maxHealth = 100.0f;

    bool destroyed = comp.ApplyDamage(0.0f);
    EXPECT_FALSE(destroyed);
    EXPECT_FALSE(comp.isDestroyed);
    EXPECT_NEAR(comp.health, 100.0f, 0.001f);
}

// ============================================================================
// Test 11: Destruction Negative Damage
// ============================================================================

TEST(PhysicsStress_DestructionNegativeDamage)
{
    Spark::DestructibleComponent comp;
    comp.health = 50.0f;
    comp.maxHealth = 100.0f;

    // Negative damage — effective damage (-20) is below threshold (0), so ignored
    bool destroyed = comp.ApplyDamage(-20.0f);
    EXPECT_FALSE(destroyed);
    EXPECT_FALSE(comp.isDestroyed);

    // Health should remain unchanged because negative effective damage < threshold
    EXPECT_NEAR(comp.health, 50.0f, 0.001f);
}

// ============================================================================
// Test 12: Destruction Already Destroyed
// ============================================================================

TEST(PhysicsStress_DestructionAlreadyDestroyed)
{
    Spark::DestructibleComponent comp;
    comp.health = 100.0f;
    comp.maxHealth = 100.0f;

    // Destroy it first
    bool destroyed = comp.ApplyDamage(200.0f);
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(comp.isDestroyed);
    EXPECT_NEAR(comp.health, 0.0f, 0.001f);

    // Apply damage again to an already-destroyed entity
    bool destroyedAgain = comp.ApplyDamage(100.0f);
    EXPECT_FALSE(destroyedAgain);
    EXPECT_TRUE(comp.isDestroyed);
    EXPECT_NEAR(comp.health, 0.0f, 0.001f);
}

// ============================================================================
// Test 13: Destruction Empty Pattern
// ============================================================================

TEST(PhysicsStress_DestructionEmptyPattern)
{
    Spark::FracturePattern pattern;

    // Pattern with 0 pieces
    EXPECT_EQ(static_cast<int>(pattern.GetPieces().size()), 0);

    // Should still be usable without crash
    const auto& pieces = pattern.GetPieces();
    EXPECT_TRUE(pieces.empty());
    EXPECT_TRUE(pattern.GetDestructionSound().empty());
    EXPECT_TRUE(pattern.GetParticleEffect().empty());
}

// ============================================================================
// Test 14: Collision AABB Degenerate
// ============================================================================

TEST(PhysicsStress_CollisionAABBDegenerate)
{
    // AABB with min == max (zero-volume point)
    TestPhysicsStress::AABB point;
    point.min = {5.0f, 5.0f, 5.0f};
    point.max = {5.0f, 5.0f, 5.0f};

    // A point-AABB should contain its own position
    EXPECT_TRUE(point.Contains({5.0f, 5.0f, 5.0f}));
    EXPECT_FALSE(point.Contains({5.1f, 5.0f, 5.0f}));

    // Two identical point-AABBs should intersect
    TestPhysicsStress::AABB point2;
    point2.min = {5.0f, 5.0f, 5.0f};
    point2.max = {5.0f, 5.0f, 5.0f};
    EXPECT_TRUE(point.Intersects(point2));

    // A point-AABB should not intersect a distant AABB
    TestPhysicsStress::AABB distant;
    distant.min = {10.0f, 10.0f, 10.0f};
    distant.max = {20.0f, 20.0f, 20.0f};
    EXPECT_FALSE(point.Intersects(distant));
}

// ============================================================================
// Test 15: Raycast Zero Direction
// ============================================================================

TEST(PhysicsStress_RaycastZeroDirection)
{
    TestPhysicsStress::Vec3 origin{0, 0, 0};
    TestPhysicsStress::Vec3 zeroDir{0, 0, 0};

    auto hit = TestPhysicsStress::RaycastSphere(origin, zeroDir, 100.0f, {5, 0, 0}, 1.0f);

    // Should not hit anything — zero direction is invalid
    EXPECT_FALSE(hit.hasHit);

    // Ensure no NaN in output
    EXPECT_FALSE(std::isnan(hit.distance));
    EXPECT_FALSE(std::isnan(hit.point.x));
}

// ============================================================================
// Test 16: Raycast Negative Length
// ============================================================================

TEST(PhysicsStress_RaycastNegativeLength)
{
    TestPhysicsStress::Vec3 origin{0, 0, 0};
    TestPhysicsStress::Vec3 dir{1, 0, 0};

    // Negative max distance — should produce no hit
    auto hit = TestPhysicsStress::RaycastSphere(origin, dir, -10.0f, {5, 0, 0}, 1.0f);

    EXPECT_FALSE(hit.hasHit);
    EXPECT_NEAR(hit.distance, 0.0f, 0.001f);
}
