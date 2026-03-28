// TestCollisionSystem.cpp - Tests for collision detection and raycasting

#include "TestFramework.h"
#include "Physics/CollisionSystem.h"

using namespace Spark;

// =============================================================================
// BoundingBox
// =============================================================================

TEST(CollisionSystem_BoundingBoxCenter)
{
    BoundingBox box;
    box.Min = {0.0f, 0.0f, 0.0f};
    box.Max = {10.0f, 10.0f, 10.0f};
    auto center = box.GetCenter();
    EXPECT_NEAR(center.x, 5.0f, 0.01f);
    EXPECT_NEAR(center.y, 5.0f, 0.01f);
    EXPECT_NEAR(center.z, 5.0f, 0.01f);
}

TEST(CollisionSystem_BoundingBoxExtents)
{
    BoundingBox box;
    box.Min = {-2.0f, -3.0f, -4.0f};
    box.Max = {2.0f, 3.0f, 4.0f};
    auto extents = box.GetExtents();
    EXPECT_NEAR(extents.x, 2.0f, 0.01f);
    EXPECT_NEAR(extents.y, 3.0f, 0.01f);
    EXPECT_NEAR(extents.z, 4.0f, 0.01f);
}

// =============================================================================
// Ray
// =============================================================================

TEST(CollisionSystem_RayGetPoint)
{
    Ray ray;
    ray.Origin = {0.0f, 0.0f, 0.0f};
    ray.Direction = {1.0f, 0.0f, 0.0f};
    auto pt = ray.GetPoint(5.0f);
    EXPECT_NEAR(pt.x, 5.0f, 0.01f);
    EXPECT_NEAR(pt.y, 0.0f, 0.01f);
    EXPECT_NEAR(pt.z, 0.0f, 0.01f);
}

// =============================================================================
// Sphere vs Sphere
// =============================================================================

TEST(CollisionSystem_SphereVsSphereOverlap)
{
    BoundingSphere a, b;
    a.Center = {0.0f, 0.0f, 0.0f};
    a.Radius = 5.0f;
    b.Center = {3.0f, 0.0f, 0.0f};
    b.Radius = 5.0f;
    EXPECT_TRUE(CollisionSystem::SphereVsSphere(a, b));
}

TEST(CollisionSystem_SphereVsSphereNoOverlap)
{
    BoundingSphere a, b;
    a.Center = {0.0f, 0.0f, 0.0f};
    a.Radius = 1.0f;
    b.Center = {10.0f, 0.0f, 0.0f};
    b.Radius = 1.0f;
    EXPECT_FALSE(CollisionSystem::SphereVsSphere(a, b));
}

TEST(CollisionSystem_SphereVsSphereManifold)
{
    BoundingSphere a, b;
    a.Center = {0.0f, 0.0f, 0.0f};
    a.Radius = 2.0f;
    b.Center = {3.0f, 0.0f, 0.0f};
    b.Radius = 2.0f;
    ContactManifold m;
    bool hit = CollisionSystem::SphereVsSphere(a, b, m);
    EXPECT_TRUE(hit);
    EXPECT_GT(m.PenetrationDepth, 0.0f);
}

// =============================================================================
// Sphere vs Box
// =============================================================================

TEST(CollisionSystem_SphereVsBoxOverlap)
{
    BoundingSphere sphere;
    sphere.Center = {0.0f, 0.0f, 0.0f};
    sphere.Radius = 2.0f;
    BoundingBox box;
    box.Min = {1.0f, -1.0f, -1.0f};
    box.Max = {3.0f, 1.0f, 1.0f};
    EXPECT_TRUE(CollisionSystem::SphereVsBox(sphere, box));
}

TEST(CollisionSystem_SphereVsBoxNoOverlap)
{
    BoundingSphere sphere;
    sphere.Center = {0.0f, 0.0f, 0.0f};
    sphere.Radius = 1.0f;
    BoundingBox box;
    box.Min = {5.0f, 5.0f, 5.0f};
    box.Max = {6.0f, 6.0f, 6.0f};
    EXPECT_FALSE(CollisionSystem::SphereVsBox(sphere, box));
}

// =============================================================================
// Box vs Box
// =============================================================================

TEST(CollisionSystem_BoxVsBoxOverlap)
{
    BoundingBox a, b;
    a.Min = {0.0f, 0.0f, 0.0f};
    a.Max = {2.0f, 2.0f, 2.0f};
    b.Min = {1.0f, 1.0f, 1.0f};
    b.Max = {3.0f, 3.0f, 3.0f};
    EXPECT_TRUE(CollisionSystem::BoxVsBox(a, b));
}

TEST(CollisionSystem_BoxVsBoxNoOverlap)
{
    BoundingBox a, b;
    a.Min = {0.0f, 0.0f, 0.0f};
    a.Max = {1.0f, 1.0f, 1.0f};
    b.Min = {5.0f, 5.0f, 5.0f};
    b.Max = {6.0f, 6.0f, 6.0f};
    EXPECT_FALSE(CollisionSystem::BoxVsBox(a, b));
}

// =============================================================================
// Ray vs Sphere
// =============================================================================

TEST(CollisionSystem_RayVsSphereHit)
{
    Ray ray;
    ray.Origin = {-5.0f, 0.0f, 0.0f};
    ray.Direction = {1.0f, 0.0f, 0.0f};
    BoundingSphere sphere;
    sphere.Center = {0.0f, 0.0f, 0.0f};
    sphere.Radius = 1.0f;
    auto result = CollisionSystem::RayVsSphere(ray, sphere);
    EXPECT_TRUE(result.Hit);
    EXPECT_NEAR(result.Distance, 4.0f, 0.1f);
}

TEST(CollisionSystem_RayVsSphereMiss)
{
    Ray ray;
    ray.Origin = {-5.0f, 5.0f, 0.0f};
    ray.Direction = {1.0f, 0.0f, 0.0f};
    BoundingSphere sphere;
    sphere.Center = {0.0f, 0.0f, 0.0f};
    sphere.Radius = 1.0f;
    auto result = CollisionSystem::RayVsSphere(ray, sphere);
    EXPECT_FALSE(result.Hit);
}

// =============================================================================
// Ray vs Box
// =============================================================================

TEST(CollisionSystem_RayVsBoxHit)
{
    Ray ray;
    ray.Origin = {-5.0f, 0.5f, 0.5f};
    ray.Direction = {1.0f, 0.0f, 0.0f};
    BoundingBox box;
    box.Min = {0.0f, 0.0f, 0.0f};
    box.Max = {1.0f, 1.0f, 1.0f};
    auto result = CollisionSystem::RayVsBox(ray, box);
    EXPECT_TRUE(result.Hit);
    EXPECT_GT(result.Distance, 0.0f);
}

// =============================================================================
// Ray vs Plane
// =============================================================================

TEST(CollisionSystem_RayVsPlaneHit)
{
    Ray ray;
    ray.Origin = {0.0f, 5.0f, 0.0f};
    ray.Direction = {0.0f, -1.0f, 0.0f};
    DirectX::XMFLOAT3 planePoint = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 planeNormal = {0.0f, 1.0f, 0.0f};
    auto result = CollisionSystem::RayVsPlane(ray, planePoint, planeNormal);
    EXPECT_TRUE(result.Hit);
    EXPECT_NEAR(result.Distance, 5.0f, 0.01f);
}

// =============================================================================
// Point Tests
// =============================================================================

TEST(CollisionSystem_PointInSphere)
{
    BoundingSphere sphere;
    sphere.Center = {0.0f, 0.0f, 0.0f};
    sphere.Radius = 5.0f;
    EXPECT_TRUE(CollisionSystem::PointInSphere({1.0f, 1.0f, 1.0f}, sphere));
    EXPECT_FALSE(CollisionSystem::PointInSphere({10.0f, 0.0f, 0.0f}, sphere));
}

TEST(CollisionSystem_PointInBox)
{
    BoundingBox box;
    box.Min = {0.0f, 0.0f, 0.0f};
    box.Max = {5.0f, 5.0f, 5.0f};
    EXPECT_TRUE(CollisionSystem::PointInBox({2.0f, 2.0f, 2.0f}, box));
    EXPECT_FALSE(CollisionSystem::PointInBox({-1.0f, 0.0f, 0.0f}, box));
}

// =============================================================================
// Closest Point
// =============================================================================

TEST(CollisionSystem_ClosestPointOnBox)
{
    BoundingBox box;
    box.Min = {0.0f, 0.0f, 0.0f};
    box.Max = {2.0f, 2.0f, 2.0f};
    auto closest = CollisionSystem::ClosestPointOnBox({-1.0f, 1.0f, 1.0f}, box);
    EXPECT_NEAR(closest.x, 0.0f, 0.01f);
    EXPECT_NEAR(closest.y, 1.0f, 0.01f);
    EXPECT_NEAR(closest.z, 1.0f, 0.01f);
}

// =============================================================================
// Vector Utilities
// =============================================================================

TEST(CollisionSystem_Vector3Length)
{
    float len = CollisionSystem::Vector3Length({3.0f, 4.0f, 0.0f});
    EXPECT_NEAR(len, 5.0f, 0.01f);
}

TEST(CollisionSystem_Vector3Normalize)
{
    auto n = CollisionSystem::Vector3Normalize({3.0f, 0.0f, 0.0f});
    EXPECT_NEAR(n.x, 1.0f, 0.01f);
    EXPECT_NEAR(n.y, 0.0f, 0.01f);
    EXPECT_NEAR(n.z, 0.0f, 0.01f);
}

TEST(CollisionSystem_Vector3Dot)
{
    float dot = CollisionSystem::Vector3Dot({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(dot, 0.0f, 0.01f);
}

TEST(CollisionSystem_Vector3Cross)
{
    auto cross = CollisionSystem::Vector3Cross({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(cross.z, 1.0f, 0.01f);
}

TEST(CollisionSystem_Vector3Lerp)
{
    auto result = CollisionSystem::Vector3Lerp({0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}, 0.5f);
    EXPECT_NEAR(result.x, 5.0f, 0.01f);
    EXPECT_NEAR(result.y, 5.0f, 0.01f);
}
