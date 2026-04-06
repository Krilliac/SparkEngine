/**
 * @file SplinePath.cpp
 * @brief Implementation of SplinePath — reusable spline path evaluation
 */

#include "SplinePath.h"
#include "../Core/Platform.h"
#include "../Core/SafeCast.h"
#include "SplineMath.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "Validate.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <algorithm>
#include <cmath>

using namespace DirectX;

SplinePath::SplinePath(SplineType type) : m_type(type)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "SplinePath: Created with type %d", static_cast<int>(type));
}

// =============================================================================
// POINT MANAGEMENT
// =============================================================================

void SplinePath::AddPoint(const XMFLOAT3& point)
{
    m_points.push_back(point);
    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "SplinePath: Added point (%.2f, %.2f, %.2f), total points: %zu", point.x,
                    point.y, point.z, m_points.size());
    MarkDirty();
}

void SplinePath::InsertPoint(int index, const XMFLOAT3& point)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, index >= 0 && index <= Spark::NarrowCast<int>(m_points.size()),
                      "InsertPoint index out of range");
    m_points.insert(m_points.begin() + index, point);
    MarkDirty();
}

void SplinePath::RemovePoint(int index)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, index >= 0 && index < Spark::NarrowCast<int>(m_points.size()),
                      "RemovePoint index out of range");
    m_points.erase(m_points.begin() + index);
    MarkDirty();
}

void SplinePath::SetPoint(int index, const XMFLOAT3& point)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, index >= 0 && index < Spark::NarrowCast<int>(m_points.size()),
                      "SetPoint index out of range");
    m_points[index] = point;
    MarkDirty();
}

const XMFLOAT3& SplinePath::GetPoint(int index) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, index >= 0 && index < Spark::NarrowCast<int>(m_points.size()),
                      "GetPoint index out of range");
    return m_points[index];
}

int SplinePath::GetPointCount() const
{
    return Spark::NarrowCast<int>(m_points.size());
}

void SplinePath::ClearPoints()
{
    m_points.clear();
    MarkDirty();
}

// =============================================================================
// SPLINE TYPE
// =============================================================================

void SplinePath::SetSplineType(SplineType type)
{
    if (m_type != type)
    {
        m_type = type;
        MarkDirty();
    }
}

SplineType SplinePath::GetSplineType() const
{
    return m_type;
}

// =============================================================================
// CLOSED LOOP
// =============================================================================

bool SplinePath::IsClosed() const
{
    return m_closed;
}

void SplinePath::SetClosed(bool closed)
{
    if (m_closed != closed)
    {
        m_closed = closed;
        MarkDirty();
    }
}

// =============================================================================
// SEGMENT HELPERS
// =============================================================================

int SplinePath::GetSegmentCount() const
{
    int count = Spark::NarrowCast<int>(m_points.size());
    if (count < 2)
        return 0;

    if (m_type == SplineType::CubicBezier)
    {
        // Cubic Bezier uses groups of 4 points: [0,1,2,3], [3,4,5,6], ...
        return (count - 1) / 3;
    }

    // Linear and CatmullRom: one segment per adjacent pair
    int segments = count - 1;
    if (m_closed)
        segments += 1; // Extra segment wrapping last point to first
    return segments;
}

void SplinePath::GetSegmentAndLocalT(float globalT, int& outSegment, float& outLocalT) const
{
    int segCount = GetSegmentCount();
    if (segCount <= 0)
    {
        outSegment = 0;
        outLocalT = 0.0f;
        return;
    }

    globalT = (std::max)(0.0f, (std::min)(1.0f, globalT));
    float scaled = globalT * static_cast<float>(segCount);
    outSegment = static_cast<int>(scaled);
    outLocalT = scaled - static_cast<float>(outSegment);

    // Clamp to last segment at t=1
    if (outSegment >= segCount)
    {
        outSegment = segCount - 1;
        outLocalT = 1.0f;
    }
}

void SplinePath::GetCatmullRomControlPoints(int segment, XMFLOAT3& p0, XMFLOAT3& p1, XMFLOAT3& p2, XMFLOAT3& p3) const
{
    int count = Spark::NarrowCast<int>(m_points.size());

    if (m_closed)
    {
        int i0 = ((segment - 1) % count + count) % count;
        int i1 = segment % count;
        int i2 = (segment + 1) % count;
        int i3 = (segment + 2) % count;
        p0 = m_points[i0];
        p1 = m_points[i1];
        p2 = m_points[i2];
        p3 = m_points[i3];
    }
    else
    {
        int i0 = (std::max)(segment - 1, 0);
        int i1 = segment;
        int i2 = (std::min)(segment + 1, count - 1);
        int i3 = (std::min)(segment + 2, count - 1);
        p0 = m_points[i0];
        p1 = m_points[i1];
        p2 = m_points[i2];
        p3 = m_points[i3];
    }
}

// =============================================================================
// EVALUATION
// =============================================================================

XMFLOAT3 SplinePath::Evaluate(float t) const
{
    int count = Spark::NarrowCast<int>(m_points.size());
    if (count == 0)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "SplinePath::Evaluate: No points defined, returning zero");
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    if (count == 1)
        return m_points[0];

    int segment = 0;
    float localT = 0.0f;
    GetSegmentAndLocalT(t, segment, localT);

    if (m_type == SplineType::Linear)
    {
        int i0 = segment;
        int i1 = m_closed ? ((segment + 1) % count) : (std::min)(segment + 1, count - 1);
        return SplineMath::LerpPoint(m_points[i0], m_points[i1], localT);
    }
    else if (m_type == SplineType::CubicBezier)
    {
        int base = segment * 3;
        if (base + 3 < count)
        {
            return SplineMath::CubicBezier(m_points[base], m_points[base + 1], m_points[base + 2], m_points[base + 3],
                                           localT);
        }
        return m_points.back();
    }
    else // CatmullRom
    {
        XMFLOAT3 p0, p1, p2, p3;
        GetCatmullRomControlPoints(segment, p0, p1, p2, p3);
        return SplineMath::CatmullRom(p0, p1, p2, p3, localT);
    }
}

XMFLOAT3 SplinePath::EvaluateTangent(float t) const
{
    int count = Spark::NarrowCast<int>(m_points.size());
    if (count < 2)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "SplinePath::EvaluateTangent: Need at least 2 points, have %d", count);
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    int segment = 0;
    float localT = 0.0f;
    GetSegmentAndLocalT(t, segment, localT);

    if (m_type == SplineType::Linear)
    {
        int i0 = segment;
        int i1 = m_closed ? ((segment + 1) % count) : (std::min)(segment + 1, count - 1);
        return XMFLOAT3(m_points[i1].x - m_points[i0].x, m_points[i1].y - m_points[i0].y,
                        m_points[i1].z - m_points[i0].z);
    }
    else if (m_type == SplineType::CubicBezier)
    {
        int base = segment * 3;
        if (base + 3 < count)
        {
            return SplineMath::CubicBezierTangent(m_points[base], m_points[base + 1], m_points[base + 2],
                                                  m_points[base + 3], localT);
        }
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    else // CatmullRom
    {
        XMFLOAT3 p0, p1, p2, p3;
        GetCatmullRomControlPoints(segment, p0, p1, p2, p3);
        return SplineMath::CatmullRomTangent(p0, p1, p2, p3, localT);
    }
}

// =============================================================================
// ARC-LENGTH PARAMETERIZATION
// =============================================================================

void SplinePath::MarkDirty() const
{
    m_dirty = true;
}

void SplinePath::RebuildArcLengthTable() const
{
    m_arcLengthTable.clear();
    m_totalLength = 0.0f;

    int segCount = GetSegmentCount();
    if (segCount <= 0)
    {
        m_dirty = false;
        return;
    }

    m_arcLengthTable.reserve(kArcLengthSamples + 1);
    m_arcLengthTable.push_back(0.0f);

    XMFLOAT3 prev = Evaluate(0.0f);
    for (int i = 1; i <= kArcLengthSamples; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(kArcLengthSamples);
        XMFLOAT3 current = Evaluate(t);
        float dx = current.x - prev.x;
        float dy = current.y - prev.y;
        float dz = current.z - prev.z;
        m_totalLength += std::sqrt(dx * dx + dy * dy + dz * dz);
        m_arcLengthTable.push_back(m_totalLength);
        prev = current;
    }

    m_dirty = false;
    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "SplinePath: Arc-length table rebuilt, total length: %.2f",
                    m_totalLength);
}

float SplinePath::GetTotalLength() const
{
    if (m_dirty)
        RebuildArcLengthTable();
    return m_totalLength;
}

float SplinePath::DistanceToParameter(float distance) const
{
    if (m_dirty)
        RebuildArcLengthTable();

    if (m_totalLength <= 0.0f || m_arcLengthTable.size() < 2)
        return 0.0f;

    distance = (std::max)(0.0f, (std::min)(distance, m_totalLength));

    // Binary search in the arc-length table
    auto it = std::lower_bound(m_arcLengthTable.begin(), m_arcLengthTable.end(), distance);

    if (it == m_arcLengthTable.end())
        return 1.0f;

    int index = static_cast<int>(it - m_arcLengthTable.begin());
    if (index == 0)
        return 0.0f;

    float prevLen = m_arcLengthTable[index - 1];
    float nextLen = m_arcLengthTable[index];
    float segFraction = (nextLen > prevLen) ? (distance - prevLen) / (nextLen - prevLen) : 0.0f;

    float t = (static_cast<float>(index - 1) + segFraction) / static_cast<float>(kArcLengthSamples);
    return t;
}

XMFLOAT3 SplinePath::GetPointAtDistance(float distance) const
{
    float t = DistanceToParameter(distance);
    return Evaluate(t);
}

XMFLOAT3 SplinePath::GetTangentAtDistance(float distance) const
{
    float t = DistanceToParameter(distance);
    return EvaluateTangent(t);
}
