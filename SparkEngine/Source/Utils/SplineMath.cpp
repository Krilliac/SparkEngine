/**
 * @file SplineMath.cpp
 * @brief Implementation of static spline math utility functions
 */

#include "SplineMath.h"
#include "../Core/Platform.h"
#include "Utils/Assert.h"
#include "Validate.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <cmath>

using namespace DirectX;

// =============================================================================
// CATMULL-ROM
// =============================================================================

XMFLOAT3 SplineMath::CatmullRom(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;

    XMFLOAT3 result;
    result.x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                       (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
    result.y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                       (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
    result.z = 0.5f * ((2.0f * p1.z) + (-p0.z + p2.z) * t + (2.0f * p0.z - 5.0f * p1.z + 4.0f * p2.z - p3.z) * t2 +
                       (-p0.z + 3.0f * p1.z - 3.0f * p2.z + p3.z) * t3);
    return result;
}

XMFLOAT3 SplineMath::CatmullRomTangent(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                       float t)
{
    float t2 = t * t;

    // Derivative of the Catmull-Rom formula with respect to t
    XMFLOAT3 result;
    result.x = 0.5f * ((-p0.x + p2.x) + 2.0f * (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t +
                       3.0f * (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t2);
    result.y = 0.5f * ((-p0.y + p2.y) + 2.0f * (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t +
                       3.0f * (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t2);
    result.z = 0.5f * ((-p0.z + p2.z) + 2.0f * (2.0f * p0.z - 5.0f * p1.z + 4.0f * p2.z - p3.z) * t +
                       3.0f * (-p0.z + 3.0f * p1.z - 3.0f * p2.z + p3.z) * t2);
    return result;
}

// =============================================================================
// CUBIC BEZIER
// =============================================================================

XMFLOAT3 SplineMath::CubicBezier(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                 float t)
{
    float u = 1.0f - t;
    float u2 = u * u;
    float u3 = u2 * u;
    float t2 = t * t;
    float t3 = t2 * t;

    XMFLOAT3 result;
    result.x = u3 * p0.x + 3.0f * u2 * t * p1.x + 3.0f * u * t2 * p2.x + t3 * p3.x;
    result.y = u3 * p0.y + 3.0f * u2 * t * p1.y + 3.0f * u * t2 * p2.y + t3 * p3.y;
    result.z = u3 * p0.z + 3.0f * u2 * t * p1.z + 3.0f * u * t2 * p2.z + t3 * p3.z;
    return result;
}

XMFLOAT3 SplineMath::CubicBezierTangent(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                        float t)
{
    float u = 1.0f - t;
    float u2 = u * u;
    float t2 = t * t;

    // Derivative: 3(1-t)^2(p1-p0) + 6(1-t)t(p2-p1) + 3t^2(p3-p2)
    XMFLOAT3 result;
    result.x = 3.0f * u2 * (p1.x - p0.x) + 6.0f * u * t * (p2.x - p1.x) + 3.0f * t2 * (p3.x - p2.x);
    result.y = 3.0f * u2 * (p1.y - p0.y) + 6.0f * u * t * (p2.y - p1.y) + 3.0f * t2 * (p3.y - p2.y);
    result.z = 3.0f * u2 * (p1.z - p0.z) + 6.0f * u * t * (p2.z - p1.z) + 3.0f * t2 * (p3.z - p2.z);
    return result;
}

// =============================================================================
// LINEAR INTERPOLATION
// =============================================================================

XMFLOAT3 SplineMath::LerpPoint(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    return XMFLOAT3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

// =============================================================================
// ARC-LENGTH UTILITIES
// =============================================================================

static float PointDistance(const XMFLOAT3& a, const XMFLOAT3& b)
{
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float SplineMath::CatmullRomSegmentLength(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2,
                                          const XMFLOAT3& p3, int subdivisions)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, subdivisions > 0, "Subdivisions must be positive");

    float totalLength = 0.0f;
    XMFLOAT3 prev = p1; // At t=0 the segment starts at p1
    for (int i = 1; i <= subdivisions; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(subdivisions);
        XMFLOAT3 current = CatmullRom(p0, p1, p2, p3, t);
        totalLength += PointDistance(prev, current);
        prev = current;
    }
    return totalLength;
}

float SplineMath::CubicBezierSegmentLength(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2,
                                           const XMFLOAT3& p3, int subdivisions)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, subdivisions > 0, "Subdivisions must be positive");

    float totalLength = 0.0f;
    XMFLOAT3 prev = p0; // At t=0 the Bezier starts at p0
    for (int i = 1; i <= subdivisions; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(subdivisions);
        XMFLOAT3 current = CubicBezier(p0, p1, p2, p3, t);
        totalLength += PointDistance(prev, current);
        prev = current;
    }
    return totalLength;
}
