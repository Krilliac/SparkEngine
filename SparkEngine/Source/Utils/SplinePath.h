/**
 * @file SplinePath.h
 * @brief Reusable spline path class for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * SplinePath stores a sequence of control points and evaluates positions and
 * tangents along the path using configurable interpolation (Linear, Catmull-Rom,
 * or Cubic Bezier). It supports arc-length parameterization for constant-speed
 * traversal and closest-point queries.
 *
 * Usage example:
 * @code
 *   SplinePath path(SplineType::CatmullRom);
 *   path.AddPoint({0, 0, 0});
 *   path.AddPoint({10, 5, 0});
 *   path.AddPoint({20, 0, 0});
 *   path.AddPoint({30, 5, 0});
 *
 *   XMFLOAT3 midpoint = path.Evaluate(0.5f);
 *   float totalLen    = path.GetTotalLength();
 *   XMFLOAT3 atDist   = path.GetPointAtDistance(totalLen * 0.5f);
 * @endcode
 */

#pragma once

#include "../Core/framework.h"
#include "SplineMath.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>

/**
 * @enum SplineType
 * @brief Interpolation method used by SplinePath.
 */
enum class SplineType
{
    Linear,     ///< Polyline interpolation between adjacent points.
    CatmullRom, ///< Catmull-Rom spline with automatic tangents from neighbors.
    CubicBezier ///< Cubic Bezier; points grouped in sets of 4 (3n+1 total for n segments).
};

/**
 * @class SplinePath
 * @brief Stores control points and evaluates a spline path.
 *
 * Supports arc-length parameterization for constant-speed traversal via
 * GetPointAtDistance() and GetTangentAtDistance(). The arc-length lookup
 * table is rebuilt lazily when points change.
 */
class SplinePath
{
  public:
    SplinePath() = default;
    explicit SplinePath(SplineType type);

    // =========================================================================
    // Point management
    // =========================================================================

    /** @brief Append a control point to the end of the path. */
    void AddPoint(const XMFLOAT3& point);

    /** @brief Insert a control point at the given index. */
    void InsertPoint(int index, const XMFLOAT3& point);

    /** @brief Remove the control point at the given index. */
    void RemovePoint(int index);

    /** @brief Replace the control point at the given index. */
    void SetPoint(int index, const XMFLOAT3& point);

    /** @brief Get the control point at the given index. */
    const XMFLOAT3& GetPoint(int index) const;

    /** @brief Return the number of control points. */
    int GetPointCount() const;

    /** @brief Remove all control points. */
    void ClearPoints();

    // =========================================================================
    // Spline type
    // =========================================================================

    void SetSplineType(SplineType type);
    SplineType GetSplineType() const;

    // =========================================================================
    // Evaluation (parameter t in [0, 1] across entire path)
    // =========================================================================

    /**
     * @brief Evaluate the spline at global parameter t in [0, 1].
     * @param t  Normalized parameter; 0 = path start, 1 = path end.
     * @return   Interpolated world-space position.
     */
    XMFLOAT3 Evaluate(float t) const;

    /**
     * @brief Evaluate the tangent at global parameter t in [0, 1].
     * @param t  Normalized parameter.
     * @return   Unnormalized tangent vector at t.
     */
    XMFLOAT3 EvaluateTangent(float t) const;

    // =========================================================================
    // Arc-length parameterization
    // =========================================================================

    /** @brief Return the total approximate arc length of the path. */
    float GetTotalLength() const;

    /**
     * @brief Get the world-space position at a distance along the path.
     * @param distance  Distance from the path start in world units.
     * @return          Interpolated position at the given distance.
     */
    XMFLOAT3 GetPointAtDistance(float distance) const;

    /**
     * @brief Get the tangent at a distance along the path.
     * @param distance  Distance from the path start in world units.
     * @return          Unnormalized tangent vector at the given distance.
     */
    XMFLOAT3 GetTangentAtDistance(float distance) const;

    // =========================================================================
    // Closed-loop support
    // =========================================================================

    bool IsClosed() const;
    void SetClosed(bool closed);

  private:
    std::vector<XMFLOAT3> m_points;
    SplineType m_type = SplineType::CatmullRom;
    bool m_closed = false;

    // Arc-length lookup table (rebuilt lazily)
    static constexpr int kArcLengthSamples = 256;
    mutable std::vector<float> m_arcLengthTable;
    mutable float m_totalLength = 0.0f;
    mutable bool m_dirty = true;

    void MarkDirty() const;
    void RebuildArcLengthTable() const;
    float DistanceToParameter(float distance) const;
    int GetSegmentCount() const;
    void GetSegmentAndLocalT(float globalT, int& outSegment, float& outLocalT) const;
    void GetCatmullRomControlPoints(int segment, XMFLOAT3& p0, XMFLOAT3& p1, XMFLOAT3& p2, XMFLOAT3& p3) const;
};
