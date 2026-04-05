/**
 * @file TestGizmoMath.cpp
 * @brief Tests for gizmo math (ray-plane/ray-axis intersection)
 *
 * Standalone reimplementation for CI testing without engine dependencies.
 */

#include "TestFramework.h"

#include <cmath>
#include <limits>

// ============================================================================
// Standalone reimplementation
// ============================================================================

namespace GizmoTest
{

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    };

    inline float Dot(const Vec3& a, const Vec3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline float Length(const Vec3& v)
    {
        return std::sqrt(Dot(v, v));
    }

    inline Vec3 Normalize(const Vec3& v)
    {
        float len = Length(v);
        if (len < 1e-8f)
        {
            return {0.0f, 0.0f, 0.0f};
        }
        return {v.x / len, v.y / len, v.z / len};
    }

    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    }

    struct Ray
    {
        Vec3 origin;
        Vec3 direction; // should be normalized
    };

    /// Find the closest point on an axis (line through origin in axisDir) to a ray.
    /// Returns the parameter t along the axis such that the closest point is axisDir * t.
    /// Also returns the distance between the ray and the axis at that point.
    struct AxisIntersectResult
    {
        float axisParam = 0.0f; // parameter along the axis
        float distance = 0.0f;  // closest distance between ray and axis
        Vec3 closestOnAxis{};
    };

    inline AxisIntersectResult RayAxisClosestPoint(const Ray& ray, const Vec3& axisOrigin, const Vec3& axisDir)
    {
        // Closest point between two lines:
        // Line1: P = ray.origin + t * ray.direction
        // Line2: Q = axisOrigin + s * axisDir
        Vec3 w0 = ray.origin - axisOrigin;
        float a = Dot(ray.direction, ray.direction);
        float b = Dot(ray.direction, axisDir);
        float c = Dot(axisDir, axisDir);
        float d = Dot(ray.direction, w0);
        float e = Dot(axisDir, w0);

        float denom = a * c - b * b;

        AxisIntersectResult result;
        if (std::abs(denom) < 1e-8f)
        {
            // Lines are parallel
            result.axisParam = 0.0f;
        }
        else
        {
            result.axisParam = (a * e - b * d) / denom;
        }

        result.closestOnAxis = axisOrigin + axisDir * result.axisParam;

        // Compute closest point on ray
        float tRay = (b * e - c * d) / (std::abs(denom) < 1e-8f ? 1.0f : denom);
        Vec3 closestOnRay = ray.origin + ray.direction * tRay;

        Vec3 diff = closestOnRay - result.closestOnAxis;
        result.distance = Length(diff);

        return result;
    }

    /// Constrain a translation delta to a single axis
    inline Vec3 ConstrainToAxis(const Vec3& delta, const Vec3& axis)
    {
        float projection = Dot(delta, axis);
        return axis * projection;
    }

    /// Compute uniform scale factor from a drag delta relative to a center point
    inline float ComputeUniformScale(const Vec3& dragStart, const Vec3& dragEnd, const Vec3& center)
    {
        float startDist = Length(dragStart - center);
        float endDist = Length(dragEnd - center);

        if (startDist < 1e-8f)
        {
            return 1.0f;
        }

        return endDist / startDist;
    }

} // namespace GizmoTest

// ============================================================================
// Tests
// ============================================================================

TEST(Gizmo_RayAxisIntersection)
{
    using namespace GizmoTest;

    // Ray from (0, 5, 5) pointing toward the X axis at origin
    Ray ray;
    ray.origin = {0.0f, 5.0f, 5.0f};
    ray.direction = Normalize({1.0f, -1.0f, -1.0f});

    // X axis through origin
    Vec3 axisOrigin = {0.0f, 0.0f, 0.0f};
    Vec3 axisDir = {1.0f, 0.0f, 0.0f};

    auto result = RayAxisClosestPoint(ray, axisOrigin, axisDir);

    // The ray travels (1,-1,-1)/sqrt(3) per unit t.
    // At t=5*sqrt(3), ray reaches approximately (5, 0, 0).
    // The closest point on the X axis should be near x=5.
    EXPECT_NEAR(result.closestOnAxis.x, 5.0f, 0.01f);
    EXPECT_NEAR(result.closestOnAxis.y, 0.0f, 0.01f);
    EXPECT_NEAR(result.closestOnAxis.z, 0.0f, 0.01f);

    // Distance should be near zero since the ray passes through the axis
    EXPECT_NEAR(result.distance, 0.0f, 0.01f);
}

TEST(Gizmo_AxisConstraint)
{
    using namespace GizmoTest;

    // A free-space translation delta
    Vec3 delta = {3.0f, 7.0f, 2.0f};

    // Constrain to Y axis only
    Vec3 yAxis = {0.0f, 1.0f, 0.0f};
    Vec3 constrained = ConstrainToAxis(delta, yAxis);

    // X and Z should be zero, Y should preserve the projection
    EXPECT_NEAR(constrained.x, 0.0f, 0.001f);
    EXPECT_NEAR(constrained.y, 7.0f, 0.001f);
    EXPECT_NEAR(constrained.z, 0.0f, 0.001f);

    // Constrain to X axis
    Vec3 xAxis = {1.0f, 0.0f, 0.0f};
    Vec3 constrainedX = ConstrainToAxis(delta, xAxis);

    EXPECT_NEAR(constrainedX.x, 3.0f, 0.001f);
    EXPECT_NEAR(constrainedX.y, 0.0f, 0.001f);
    EXPECT_NEAR(constrainedX.z, 0.0f, 0.001f);
}

TEST(Gizmo_ScaleHandle)
{
    using namespace GizmoTest;

    Vec3 center = {0.0f, 0.0f, 0.0f};

    // Drag starts at distance 5 from center, ends at distance 10
    Vec3 dragStart = {5.0f, 0.0f, 0.0f};
    Vec3 dragEnd = {10.0f, 0.0f, 0.0f};

    float scale = ComputeUniformScale(dragStart, dragEnd, center);
    EXPECT_NEAR(scale, 2.0f, 0.001f);

    // Drag inward: shrink
    Vec3 dragShrink = {2.5f, 0.0f, 0.0f};
    float shrinkScale = ComputeUniformScale(dragStart, dragShrink, center);
    EXPECT_NEAR(shrinkScale, 0.5f, 0.001f);

    // Diagonal drag: start at (3,4,0) distance=5, end at (6,8,0) distance=10
    Vec3 diagStart = {3.0f, 4.0f, 0.0f};
    Vec3 diagEnd = {6.0f, 8.0f, 0.0f};
    float diagScale = ComputeUniformScale(diagStart, diagEnd, center);
    EXPECT_NEAR(diagScale, 2.0f, 0.001f);
}
