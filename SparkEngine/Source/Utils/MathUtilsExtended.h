/**
 * @file MathUtilsExtended.h
 * @brief Extended mathematics utilities for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * MathUtilsExtended contains specialized math functions that are used less
 * frequently than the core utilities in MathUtils.h. These include:
 *  - **Random**           – Seeded and un-seeded random floats, ints, and 3D points.
 *  - **Matrix utilities** – LookAt, Perspective, and Orthographic matrix construction.
 *  - **Collision tests**  – Fast point-in-sphere and point-in-box tests.
 *  - **Easing functions** – Standard quad and cubic ease-in/out/in-out curves.
 *
 * This header is automatically included by MathUtils.h for backwards
 * compatibility — existing code that includes MathUtils.h will continue to
 * compile without changes.
 *
 * @note This file is included inside the MathUtils class definition in
 *       MathUtils.h. It must not contain its own class declaration or
 *       include guards that would interfere with that inclusion.
 *
 * @note All vector operations use DirectX::XMFLOAT3 for storage but internally
 *       promote to XMVECTOR for SIMD acceleration where appropriate.
 */

#pragma once

#include "../Core/framework.h" // XMFLOAT3, XMMATRIX
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

/**
 * @class MathUtilsExtended
 * @brief Extended static mathematics utility class for Spark Engine
 *
 * Contains specialized math functions (random number generation, matrix
 * construction, collision tests, easing curves) that are used less frequently
 * than the core MathUtils operations. All methods are static.
 *
 * For backwards compatibility, MathUtils.h includes this header so that
 * existing code using `#include "MathUtils.h"` continues to compile. Code
 * that only needs the extended functions can include this header directly.
 */
class MathUtilsExtended
{
  public:
    // =========================================================================
    // Random number generation
    // =========================================================================

    /**
     * @brief Seed the random number generator from the system clock.
     *
     * Should be called once at engine startup to ensure non-deterministic random
     * sequences. For deterministic (reproducible) runs, manually seed via the
     * C standard library srand() before calling any Random* methods.
     */
    static void InitializeRandom();

    /**
     * @brief Generate a random float in the range [min, max].
     *
     * Uses the C standard library rand() internally for portability, scaled and
     * shifted into the requested range. Distribution is approximately uniform.
     *
     * @param min  Minimum value (inclusive). Defaults to 0.0f.
     * @param max  Maximum value (inclusive). Defaults to 1.0f.
     * @return     Random float in [min, max].
     *
     * @code
     *   float randomAngle = MathUtilsExtended::RandomFloat(0.0f, MathUtils::TWO_PI);
     * @endcode
     */
    static float RandomFloat(float min = 0.0f, float max = 1.0f);

    /**
     * @brief Generate a random integer in the range [min, max].
     *
     * Both endpoints are inclusive. If min > max the result is undefined.
     *
     * @param min  Minimum integer value (inclusive).
     * @param max  Maximum integer value (inclusive).
     * @return     Random integer in [min, max].
     *
     * @code
     *   int side = MathUtilsExtended::RandomInt(0, 3);  // random face of a cube
     * @endcode
     */
    static int RandomInt(int min, int max);

    /**
     * @brief Generate a random unit direction vector.
     *
     * Returns a vector of length 1.0 pointing in a uniformly random direction
     * on the unit sphere, useful for scatter/spread effects.
     *
     * @return  Normalized random direction vector.
     */
    static XMFLOAT3 RandomDirection();

    /**
     * @brief Generate a random point inside a sphere of the given radius.
     *
     * The distribution is NOT uniformly volumetric by default (points cluster
     * at the surface); for uniform volume distribution consider rejection
     * sampling externally.
     *
     * @param radius  Radius of the bounding sphere.
     * @return        Random point with distance ≤ radius from the origin.
     */
    static XMFLOAT3 RandomPointInSphere(float radius);


    // =========================================================================
    // Matrix utilities
    // =========================================================================

    /**
     * @brief Build a right-handed look-at view matrix.
     *
     * Constructs a view transform that positions a camera at `eye`, looking
     * towards `target`, with the given `up` hint vector. Internally delegates
     * to DirectX::XMMatrixLookAtRH.
     *
     * @param eye     Camera/eye position in world space.
     * @param target  Target position the camera looks at.
     * @param up      World up vector hint (usually {0, 1, 0}).
     * @return        View matrix transforming world space to camera space.
     *
     * @code
     *   XMMATRIX view = MathUtilsExtended::CreateLookAt(eyePos, targetPos, {0, 1, 0});
     * @endcode
     */
    static XMMATRIX CreateLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up);

    /**
     * @brief Build a perspective projection matrix.
     *
     * Creates a perspective transform suitable for 3D game rendering. Internally
     * delegates to DirectX::XMMatrixPerspectiveFovRH.
     *
     * @param fovY        Vertical field-of-view angle in radians.
     * @param aspectRatio Width-to-height ratio of the viewport.
     * @param nearPlane   Near clipping plane distance (positive, > 0).
     * @param farPlane    Far clipping plane distance (positive, > nearPlane).
     * @return            Perspective projection matrix.
     *
     * @code
     *   float fov = MathUtils::DegreesToRadians(60.0f);
     *   XMMATRIX proj = MathUtilsExtended::CreatePerspective(fov, 16.0f/9.0f, 0.1f, 5000.0f);
     * @endcode
     */
    static XMMATRIX CreatePerspective(float fovY, float aspectRatio, float nearPlane, float farPlane);

    /**
     * @brief Build an orthographic projection matrix.
     *
     * Creates an orthographic transform for 2D rendering, UI overlays, or shadow
     * map generation. Internally delegates to DirectX::XMMatrixOrthographicRH.
     *
     * @param width      Width of the orthographic view volume.
     * @param height     Height of the orthographic view volume.
     * @param nearPlane  Near clipping plane distance.
     * @param farPlane   Far clipping plane distance.
     * @return           Orthographic projection matrix.
     */
    static XMMATRIX CreateOrthographic(float width, float height, float nearPlane, float farPlane);


    // =========================================================================
    // Collision utilities
    // =========================================================================

    /**
     * @brief Test whether a point lies inside or on a sphere.
     *
     * Performs a fast distance-squared comparison: `dist² ≤ radius²`.
     *
     * @param point         Point to test (world space).
     * @param sphereCenter  Centre of the sphere (world space).
     * @param sphereRadius  Radius of the sphere.
     * @return              `true` if the point is inside or on the sphere surface.
     *
     * @code
     *   if (MathUtilsExtended::PointInSphere(enemy.pos, explosionCenter, blastRadius))
     *       enemy.TakeDamage(damage);
     * @endcode
     */
    static bool PointInSphere(const XMFLOAT3& point, const XMFLOAT3& sphereCenter, float sphereRadius);

    /**
     * @brief Test whether a point lies inside an axis-aligned bounding box.
     *
     * Checks all three axes: `boxMin.x ≤ point.x ≤ boxMax.x`, and likewise for
     * Y and Z. The box must be axis-aligned (not rotated).
     *
     * @param point   Point to test (world space).
     * @param boxMin  Minimum corner of the AABB (world space).
     * @param boxMax  Maximum corner of the AABB (world space).
     * @return        `true` if the point is inside or on the box.
     */
    static bool PointInBox(const XMFLOAT3& point, const XMFLOAT3& boxMin, const XMFLOAT3& boxMax);


    // =========================================================================
    // Easing functions
    // =========================================================================

    /**
     * @brief Quadratic ease-in curve.
     *
     * Starts slowly and accelerates towards the end. Formula: `t²`.
     * Input `t` should be in [0, 1].
     *
     * @param t  Normalized progress in [0, 1].
     * @return   Eased value in [0, 1].
     */
    static float EaseInQuad(float t);

    /**
     * @brief Quadratic ease-out curve.
     *
     * Starts fast and decelerates towards the end. Formula: `t * (2 - t)`.
     * Input `t` should be in [0, 1].
     *
     * @param t  Normalized progress in [0, 1].
     * @return   Eased value in [0, 1].
     */
    static float EaseOutQuad(float t);

    /**
     * @brief Quadratic ease-in-out curve.
     *
     * Accelerates from zero, reaches full speed at the midpoint, then decelerates
     * back to zero. Formula: piecewise quadratic. Input `t` should be in [0, 1].
     *
     * @param t  Normalized progress in [0, 1].
     * @return   Eased value in [0, 1].
     */
    static float EaseInOutQuad(float t);

    /**
     * @brief Cubic ease-in curve.
     *
     * Stronger acceleration than EaseInQuad. Formula: `t³`.
     * Input `t` should be in [0, 1].
     *
     * @param t  Normalized progress in [0, 1].
     * @return   Eased value in [0, 1].
     */
    static float EaseInCubic(float t);

    /**
     * @brief Cubic ease-out curve.
     *
     * Stronger deceleration than EaseOutQuad. Formula: `(t-1)³ + 1`.
     * Input `t` should be in [0, 1].
     *
     * @param t  Normalized progress in [0, 1].
     * @return   Eased value in [0, 1].
     */
    static float EaseOutCubic(float t);

    /**
     * @brief Cubic ease-in-out curve.
     *
     * The strongest symmetric ease of the provided set; suitable for cinematic
     * transitions. Formula: piecewise cubic. Input `t` should be in [0, 1].
     *
     * @param t  Normalized progress in [0, 1].
     * @return   Eased value in [0, 1].
     */
    static float EaseInOutCubic(float t);
};
