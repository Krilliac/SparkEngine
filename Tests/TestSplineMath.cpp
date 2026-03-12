// TestSplineMath.cpp - Tests for spline math, SplinePath, and follower logic
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace TestSpline
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
        float DistanceTo(const Vec3& o) const { return (*this - o).Length(); }
    };

    // ============================================================================
    // Catmull-Rom (mirrors SplineMath::CatmullRom)
    // ============================================================================

    Vec3 CatmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t)
    {
        float t2 = t * t;
        float t3 = t2 * t;

        float a0 = -0.5f * t3 + t2 - 0.5f * t;
        float a1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
        float a2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
        float a3 = 0.5f * t3 - 0.5f * t2;

        return p0 * a0 + p1 * a1 + p2 * a2 + p3 * a3;
    }

    Vec3 CatmullRomTangent(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t)
    {
        float t2 = t * t;

        float a0 = -1.5f * t2 + 2.0f * t - 0.5f;
        float a1 = 4.5f * t2 - 5.0f * t;
        float a2 = -4.5f * t2 + 4.0f * t + 0.5f;
        float a3 = 1.5f * t2 - t;

        return p0 * a0 + p1 * a1 + p2 * a2 + p3 * a3;
    }

    // ============================================================================
    // Cubic Bezier (mirrors SplineMath::CubicBezier)
    // ============================================================================

    Vec3 CubicBezier(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t)
    {
        float u = 1.0f - t;
        float u2 = u * u;
        float u3 = u2 * u;
        float t2 = t * t;
        float t3 = t2 * t;
        return p0 * u3 + p1 * (3.0f * u2 * t) + p2 * (3.0f * u * t2) + p3 * t3;
    }

    Vec3 CubicBezierTangent(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t)
    {
        float u = 1.0f - t;
        float u2 = u * u;
        float t2 = t * t;
        return (p1 - p0) * (3.0f * u2) + (p2 - p1) * (6.0f * u * t) + (p3 - p2) * (3.0f * t2);
    }

    // ============================================================================
    // Linear interpolation
    // ============================================================================

    Vec3 LerpPoint(const Vec3& a, const Vec3& b, float t)
    {
        return a + (b - a) * t;
    }

    // ============================================================================
    // Arc-length helper
    // ============================================================================

    float SegmentLength(Vec3 (*evalFn)(const Vec3&, const Vec3&, const Vec3&, const Vec3&, float), const Vec3& p0,
                        const Vec3& p1, const Vec3& p2, const Vec3& p3, int subdivisions = 64)
    {
        float total = 0.0f;
        Vec3 prev = evalFn(p0, p1, p2, p3, 0.0f);
        for (int i = 1; i <= subdivisions; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(subdivisions);
            Vec3 current = evalFn(p0, p1, p2, p3, t);
            total += current.DistanceTo(prev);
            prev = current;
        }
        return total;
    }

    // ============================================================================
    // SplinePath reimplementation (simplified, mirrors SplinePath class)
    // ============================================================================

    enum class SplineType
    {
        Linear,
        CatmullRom,
        CubicBezier
    };

    enum class LoopMode
    {
        Once,
        Loop,
        PingPong
    };

    class SimplePath
    {
      public:
        std::vector<Vec3> points;
        SplineType type = SplineType::CatmullRom;
        bool closed = false;

        int GetSegmentCount() const
        {
            int count = static_cast<int>(points.size());
            if (count < 2)
                return 0;
            if (type == SplineType::CubicBezier)
                return (count - 1) / 3;
            int segs = count - 1;
            if (closed)
                segs += 1;
            return segs;
        }

        Vec3 Evaluate(float t) const
        {
            int count = static_cast<int>(points.size());
            if (count == 0)
                return Vec3(0, 0, 0);
            if (count == 1)
                return points[0];

            int segCount = GetSegmentCount();
            if (segCount <= 0)
                return points[0];

            t = (std::max)(0.0f, (std::min)(1.0f, t));
            float scaled = t * static_cast<float>(segCount);
            int segment = static_cast<int>(scaled);
            float localT = scaled - static_cast<float>(segment);
            if (segment >= segCount)
            {
                segment = segCount - 1;
                localT = 1.0f;
            }

            if (type == SplineType::Linear)
            {
                int i0 = segment;
                int i1 = closed ? ((segment + 1) % count) : (std::min)(segment + 1, count - 1);
                return LerpPoint(points[i0], points[i1], localT);
            }
            else if (type == SplineType::CubicBezier)
            {
                int base = segment * 3;
                if (base + 3 < count)
                    return TestSpline::CubicBezier(points[base], points[base + 1], points[base + 2], points[base + 3],
                                                   localT);
                return points.back();
            }
            else // CatmullRom
            {
                Vec3 cp0, cp1, cp2, cp3;
                if (closed)
                {
                    int i0 = ((segment - 1) % count + count) % count;
                    int i1 = segment % count;
                    int i2 = (segment + 1) % count;
                    int i3 = (segment + 2) % count;
                    cp0 = points[i0];
                    cp1 = points[i1];
                    cp2 = points[i2];
                    cp3 = points[i3];
                }
                else
                {
                    int i0 = (std::max)(segment - 1, 0);
                    int i1 = segment;
                    int i2 = (std::min)(segment + 1, count - 1);
                    int i3 = (std::min)(segment + 2, count - 1);
                    cp0 = points[i0];
                    cp1 = points[i1];
                    cp2 = points[i2];
                    cp3 = points[i3];
                }
                return TestSpline::CatmullRom(cp0, cp1, cp2, cp3, localT);
            }
        }

        float GetTotalLength(int samples = 256) const
        {
            if (points.size() < 2)
                return 0.0f;
            float total = 0.0f;
            Vec3 prev = Evaluate(0.0f);
            for (int i = 1; i <= samples; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(samples);
                Vec3 current = Evaluate(t);
                total += current.DistanceTo(prev);
                prev = current;
            }
            return total;
        }
    };

} // namespace TestSpline

// ============================================================================
// Catmull-Rom Tests
// ============================================================================

TEST(CatmullRom_AtT0_ReturnsP1)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(1, 0, 0), p2(2, 0, 0), p3(3, 0, 0);
    Vec3 result = CatmullRom(p0, p1, p2, p3, 0.0f);
    EXPECT_NEAR(result.x, 1.0f, 0.001f);
    EXPECT_NEAR(result.y, 0.0f, 0.001f);
    EXPECT_NEAR(result.z, 0.0f, 0.001f);
}

TEST(CatmullRom_AtT1_ReturnsP2)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(1, 0, 0), p2(2, 0, 0), p3(3, 0, 0);
    Vec3 result = CatmullRom(p0, p1, p2, p3, 1.0f);
    EXPECT_NEAR(result.x, 2.0f, 0.001f);
    EXPECT_NEAR(result.y, 0.0f, 0.001f);
    EXPECT_NEAR(result.z, 0.0f, 0.001f);
}

TEST(CatmullRom_Midpoint_StraightLine)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(10, 0, 0), p2(20, 0, 0), p3(30, 0, 0);
    Vec3 result = CatmullRom(p0, p1, p2, p3, 0.5f);
    EXPECT_NEAR(result.x, 15.0f, 0.001f);
    EXPECT_NEAR(result.y, 0.0f, 0.001f);
}

TEST(CatmullRom_Symmetry)
{
    using namespace TestSpline;
    // Symmetric control points: result at t=0.5 should have y at the average
    Vec3 p0(0, 0, 0), p1(5, 0, 0), p2(15, 0, 0), p3(20, 0, 0);
    Vec3 result = CatmullRom(p0, p1, p2, p3, 0.5f);
    EXPECT_NEAR(result.x, 10.0f, 0.001f);
}

TEST(CatmullRom_Tangent_StraightLine)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(10, 0, 0), p2(20, 0, 0), p3(30, 0, 0);
    Vec3 tangent = CatmullRomTangent(p0, p1, p2, p3, 0.5f);
    // On a straight line, tangent should point along x-axis
    EXPECT_TRUE(tangent.x > 0.0f);
    EXPECT_NEAR(tangent.y, 0.0f, 0.001f);
    EXPECT_NEAR(tangent.z, 0.0f, 0.001f);
}

// ============================================================================
// Cubic Bezier Tests
// ============================================================================

TEST(CubicBezier_AtT0_ReturnsP0)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(1, 2, 0), p2(3, 2, 0), p3(4, 0, 0);
    Vec3 result = CubicBezier(p0, p1, p2, p3, 0.0f);
    EXPECT_NEAR(result.x, 0.0f, 0.001f);
    EXPECT_NEAR(result.y, 0.0f, 0.001f);
}

TEST(CubicBezier_AtT1_ReturnsP3)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(1, 2, 0), p2(3, 2, 0), p3(4, 0, 0);
    Vec3 result = CubicBezier(p0, p1, p2, p3, 1.0f);
    EXPECT_NEAR(result.x, 4.0f, 0.001f);
    EXPECT_NEAR(result.y, 0.0f, 0.001f);
}

TEST(CubicBezier_StraightLine_Midpoint)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(10, 0, 0), p2(20, 0, 0), p3(30, 0, 0);
    Vec3 result = CubicBezier(p0, p1, p2, p3, 0.5f);
    EXPECT_NEAR(result.x, 15.0f, 0.001f);
    EXPECT_NEAR(result.y, 0.0f, 0.001f);
}

TEST(CubicBezier_Tangent_AtEndpoints)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(1, 2, 0), p2(3, 2, 0), p3(4, 0, 0);
    // At t=0 tangent should point from p0 to p1
    Vec3 t0 = CubicBezierTangent(p0, p1, p2, p3, 0.0f);
    EXPECT_TRUE(t0.x > 0.0f);
    EXPECT_TRUE(t0.y > 0.0f);
    // At t=1 tangent should point from p2 to p3
    Vec3 t1 = CubicBezierTangent(p0, p1, p2, p3, 1.0f);
    EXPECT_TRUE(t1.x > 0.0f);
    EXPECT_TRUE(t1.y < 0.0f);
}

// ============================================================================
// Linear Interpolation Tests
// ============================================================================

TEST(LerpPoint_AtEndpoints)
{
    using namespace TestSpline;
    Vec3 a(0, 0, 0), b(10, 20, 30);
    Vec3 r0 = LerpPoint(a, b, 0.0f);
    Vec3 r1 = LerpPoint(a, b, 1.0f);
    EXPECT_NEAR(r0.x, 0.0f, 0.001f);
    EXPECT_NEAR(r1.x, 10.0f, 0.001f);
    EXPECT_NEAR(r1.y, 20.0f, 0.001f);
    EXPECT_NEAR(r1.z, 30.0f, 0.001f);
}

TEST(LerpPoint_Midpoint)
{
    using namespace TestSpline;
    Vec3 a(0, 0, 0), b(10, 20, 30);
    Vec3 mid = LerpPoint(a, b, 0.5f);
    EXPECT_NEAR(mid.x, 5.0f, 0.001f);
    EXPECT_NEAR(mid.y, 10.0f, 0.001f);
    EXPECT_NEAR(mid.z, 15.0f, 0.001f);
}

// ============================================================================
// Arc-Length Tests
// ============================================================================

TEST(ArcLength_StraightLine_CatmullRom)
{
    using namespace TestSpline;
    // Collinear points: arc length should match Euclidean distance
    Vec3 p0(0, 0, 0), p1(10, 0, 0), p2(20, 0, 0), p3(30, 0, 0);
    float len = SegmentLength(CatmullRom, p0, p1, p2, p3, 64);
    EXPECT_NEAR(len, 10.0f, 0.1f); // Segment is p1 to p2 = 10 units
}

TEST(ArcLength_StraightLine_CubicBezier)
{
    using namespace TestSpline;
    Vec3 p0(0, 0, 0), p1(10, 0, 0), p2(20, 0, 0), p3(30, 0, 0);
    float len = SegmentLength(CubicBezier, p0, p1, p2, p3, 64);
    EXPECT_NEAR(len, 30.0f, 0.1f); // Bezier goes from p0 to p3 = 30 units
}

// ============================================================================
// SplinePath Tests
// ============================================================================

TEST(SplinePath_AddRemovePoints)
{
    using namespace TestSpline;
    SimplePath path;
    path.points.push_back(Vec3(0, 0, 0));
    path.points.push_back(Vec3(10, 0, 0));
    path.points.push_back(Vec3(20, 0, 0));
    EXPECT_EQ(static_cast<int>(path.points.size()), 3);
    path.points.erase(path.points.begin() + 1);
    EXPECT_EQ(static_cast<int>(path.points.size()), 2);
}

TEST(SplinePath_Linear_Endpoints)
{
    using namespace TestSpline;
    SimplePath path;
    path.type = SplineType::Linear;
    path.points = {Vec3(0, 0, 0), Vec3(10, 0, 0), Vec3(20, 0, 0)};
    Vec3 start = path.Evaluate(0.0f);
    Vec3 end = path.Evaluate(1.0f);
    EXPECT_NEAR(start.x, 0.0f, 0.001f);
    EXPECT_NEAR(end.x, 20.0f, 0.001f);
}

TEST(SplinePath_CatmullRom_Endpoints)
{
    using namespace TestSpline;
    SimplePath path;
    path.type = SplineType::CatmullRom;
    path.points = {Vec3(0, 0, 0), Vec3(10, 0, 0), Vec3(20, 0, 0), Vec3(30, 0, 0)};
    Vec3 start = path.Evaluate(0.0f);
    Vec3 end = path.Evaluate(1.0f);
    EXPECT_NEAR(start.x, 0.0f, 0.001f);
    EXPECT_NEAR(end.x, 30.0f, 0.5f);
}

TEST(SplinePath_TotalLength_StraightLine)
{
    using namespace TestSpline;
    SimplePath path;
    path.type = SplineType::Linear;
    path.points = {Vec3(0, 0, 0), Vec3(10, 0, 0), Vec3(20, 0, 0)};
    float len = path.GetTotalLength();
    EXPECT_NEAR(len, 20.0f, 0.5f);
}

TEST(SplinePath_SinglePoint_ReturnsPoint)
{
    using namespace TestSpline;
    SimplePath path;
    path.points = {Vec3(5, 3, 1)};
    Vec3 result = path.Evaluate(0.5f);
    EXPECT_NEAR(result.x, 5.0f, 0.001f);
    EXPECT_NEAR(result.y, 3.0f, 0.001f);
}

TEST(SplinePath_TwoPoints_CatmullRom)
{
    using namespace TestSpline;
    // With 2 points and clamped endpoints, CatmullRom should degrade to near-linear
    SimplePath path;
    path.type = SplineType::CatmullRom;
    path.points = {Vec3(0, 0, 0), Vec3(10, 0, 0)};
    Vec3 mid = path.Evaluate(0.5f);
    EXPECT_NEAR(mid.x, 5.0f, 0.5f);
}

TEST(SplinePath_CubicBezier_Evaluate)
{
    using namespace TestSpline;
    SimplePath path;
    path.type = SplineType::CubicBezier;
    // One Bezier segment: 4 points
    path.points = {Vec3(0, 0, 0), Vec3(5, 10, 0), Vec3(15, 10, 0), Vec3(20, 0, 0)};
    Vec3 start = path.Evaluate(0.0f);
    Vec3 end = path.Evaluate(1.0f);
    EXPECT_NEAR(start.x, 0.0f, 0.001f);
    EXPECT_NEAR(end.x, 20.0f, 0.001f);
}

// ============================================================================
// Follower Logic Tests
// ============================================================================

TEST(Follower_Once_StopsAtEnd)
{
    using namespace TestSpline;
    float totalLength = 20.0f;
    float currentDistance = 0.0f;
    float speed = 100.0f;
    bool finished = false;

    // Simulate one large step that overshoots
    currentDistance += speed * 1.0f; // 100 units forward
    if (currentDistance >= totalLength)
    {
        currentDistance = totalLength;
        finished = true;
    }

    EXPECT_TRUE(finished);
    EXPECT_NEAR(currentDistance, totalLength, 0.001f);
}

TEST(Follower_Loop_Wraps)
{
    using namespace TestSpline;
    float totalLength = 20.0f;
    float currentDistance = 0.0f;
    float speed = 25.0f;

    // Simulate overshoot
    currentDistance += speed * 1.0f; // 25 units
    if (currentDistance >= totalLength)
        currentDistance = std::fmod(currentDistance, totalLength);

    EXPECT_NEAR(currentDistance, 5.0f, 0.001f);
}

TEST(Follower_PingPong_Reverses)
{
    using namespace TestSpline;
    float totalLength = 20.0f;
    float currentDistance = 18.0f;
    bool reverse = false;

    // Forward step that reaches end
    currentDistance += 5.0f;
    if (currentDistance >= totalLength)
    {
        currentDistance = totalLength;
        reverse = true;
    }

    EXPECT_TRUE(reverse);
    EXPECT_NEAR(currentDistance, totalLength, 0.001f);

    // Reverse step
    currentDistance -= 5.0f;
    EXPECT_NEAR(currentDistance, 15.0f, 0.001f);
    EXPECT_TRUE(currentDistance > 0.0f);
}

TEST(Follower_PingPong_BouncesBack)
{
    using namespace TestSpline;
    float totalLength = 10.0f;
    float currentDistance = 2.0f;
    bool reverse = true;

    // Reverse step that reaches start
    currentDistance -= 5.0f;
    if (currentDistance <= 0.0f)
    {
        currentDistance = 0.0f;
        reverse = false;
    }

    EXPECT_FALSE(reverse);
    EXPECT_NEAR(currentDistance, 0.0f, 0.001f);
}
