/**
 * @file SplineMath.h
 * @brief Static spline math utility functions for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * SplineMath provides low-level spline evaluation functions for Catmull-Rom,
 * cubic Bezier, and linear interpolation. All methods are static and stateless,
 * following the same pattern as MathUtils.
 *
 * Supported curve types:
 *  - **Catmull-Rom** — Passes through control points with automatic tangents.
 *  - **Cubic Bezier** — Four control points; curve passes through first and last.
 *  - **Linear**       — Straight-line interpolation between two points.
 *
 * Each curve type provides position evaluation, tangent (first derivative)
 * evaluation, and approximate arc-length computation.
 *
 * @note All vector operations use DirectX::XMFLOAT3 for storage.
 *
 * Usage example:
 * @code
 *   XMFLOAT3 pos = SplineMath::CatmullRom(p0, p1, p2, p3, 0.5f);
 *   XMFLOAT3 tan = SplineMath::CatmullRomTangent(p0, p1, p2, p3, 0.5f);
 *   float len    = SplineMath::CatmullRomSegmentLength(p0, p1, p2, p3);
 * @endcode
 */

#pragma once

#include "../Core/framework.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

/**
 * @class SplineMath
 * @brief Static-only spline math utility class for Spark Engine.
 *
 * All methods are static; the class is never instantiated.
 */
class SplineMath
{
  public:
    // =========================================================================
    // Catmull-Rom
    // =========================================================================

    /**
     * @brief Evaluate a Catmull-Rom spline segment at parameter t.
     *
     * The active segment is between p1 and p2. Points p0 and p3 are the
     * neighboring control points that influence the curve's tangent.
     *
     * @param p0  Control point before the segment start.
     * @param p1  Segment start point (returned when t == 0).
     * @param p2  Segment end point (returned when t == 1).
     * @param p3  Control point after the segment end.
     * @param t   Parameter in [0, 1].
     * @return    Interpolated position on the Catmull-Rom segment.
     */
    static XMFLOAT3 CatmullRom(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3, float t);

    /**
     * @brief Evaluate the tangent (first derivative) of a Catmull-Rom segment at t.
     *
     * @param p0  Control point before the segment start.
     * @param p1  Segment start point.
     * @param p2  Segment end point.
     * @param p3  Control point after the segment end.
     * @param t   Parameter in [0, 1].
     * @return    Tangent vector (unnormalized) at the given parameter.
     */
    static XMFLOAT3 CatmullRomTangent(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                      float t);

    // =========================================================================
    // Cubic Bezier
    // =========================================================================

    /**
     * @brief Evaluate a cubic Bezier curve at parameter t.
     *
     * The curve passes through p0 (at t=0) and p3 (at t=1).
     * Points p1 and p2 are off-curve control points that shape the curve.
     *
     * @param p0  Start point (returned when t == 0).
     * @param p1  First control point.
     * @param p2  Second control point.
     * @param p3  End point (returned when t == 1).
     * @param t   Parameter in [0, 1].
     * @return    Interpolated position on the cubic Bezier curve.
     */
    static XMFLOAT3 CubicBezier(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                float t);

    /**
     * @brief Evaluate the tangent (first derivative) of a cubic Bezier curve at t.
     *
     * @param p0  Start point.
     * @param p1  First control point.
     * @param p2  Second control point.
     * @param p3  End point.
     * @param t   Parameter in [0, 1].
     * @return    Tangent vector (unnormalized) at the given parameter.
     */
    static XMFLOAT3 CubicBezierTangent(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                       float t);

    // =========================================================================
    // Linear interpolation
    // =========================================================================

    /**
     * @brief Linearly interpolate between two 3D points.
     *
     * @param a  Start point (returned when t == 0).
     * @param b  End point (returned when t == 1).
     * @param t  Interpolation factor in [0, 1].
     * @return   Linearly interpolated point.
     */
    static XMFLOAT3 LerpPoint(const XMFLOAT3& a, const XMFLOAT3& b, float t);

    // =========================================================================
    // Arc-length utilities
    // =========================================================================

    /**
     * @brief Approximate arc length of a Catmull-Rom segment via subdivision.
     *
     * @param p0           Control point before the segment start.
     * @param p1           Segment start point.
     * @param p2           Segment end point.
     * @param p3           Control point after the segment end.
     * @param subdivisions Number of linear segments used for approximation.
     * @return             Approximate arc length of the segment.
     */
    static float CatmullRomSegmentLength(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
                                         int subdivisions = 32);

    /**
     * @brief Approximate arc length of a cubic Bezier segment via subdivision.
     *
     * @param p0           Start point.
     * @param p1           First control point.
     * @param p2           Second control point.
     * @param p3           End point.
     * @param subdivisions Number of linear segments used for approximation.
     * @return             Approximate arc length of the segment.
     */
    static float CubicBezierSegmentLength(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2,
                                          const XMFLOAT3& p3, int subdivisions = 32);
};
