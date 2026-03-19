/**
 * @file MathUtils.h
 * @brief Mathematics utility library for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * MathUtils provides a comprehensive set of static mathematical functions
 * commonly needed by a 3D game engine. All methods are stateless and purely
 * functional, making them safe to call from any context including multi-threaded
 * update loops.
 *
 * The class is organized into the following logical groups:
 *  - **Constants**        – Precomputed PI variants and angle conversion factors.
 *  - **Angle utilities**  – Degree/radian conversion, wrapping, and normalization.
 *  - **Interpolation**    – Lerp, SmoothStep for floats and vectors.
 *  - **Distance**         – Euclidean distance, squared distance, and direction helpers.
 *  - **Clamping**         – Clamp overloads for float, int, and XMFLOAT3.
 *  - **Vector operations**– Add, subtract, scale, dot, cross, normalize, length.
 *
 * Specialized/less-frequently-used functions are defined in MathUtilsExtended
 * (inherited by this class for backwards compatibility):
 *  - **Random**           – Seeded and un-seeded random floats, ints, and 3D points.
 *  - **Matrix utilities** – LookAt, Perspective, and Orthographic matrix construction.
 *  - **Collision tests**  – Fast point-in-sphere and point-in-box tests.
 *  - **Easing functions** – Standard quad and cubic ease-in/out/in-out curves.
 *
 * @note All vector operations use DirectX::XMFLOAT3 for storage but internally
 *       promote to XMVECTOR for SIMD acceleration where appropriate.
 *
 * Usage example:
 * @code
 *   // Convert camera FOV from degrees to radians
 *   float fovRad = MathUtils::DegreesToRadians(60.0f);
 *
 *   // Smooth camera interpolation
 *   XMFLOAT3 current = camera.GetPosition();
 *   XMFLOAT3 target  = player.GetPosition();
 *   XMFLOAT3 smooth  = MathUtils::Lerp(current, target, 0.1f);
 *
 *   // Check if an enemy is within detection range
 *   float dist = MathUtils::Distance(enemy.GetPosition(), player.GetPosition());
 *   bool detected = dist < detectionRadius;
 * @endcode
 */

#pragma once

#include "../Core/framework.h" // XMFLOAT3, XMMATRIX
#include "Utils/Assert.h"
#include "Utils/TypeTraits.h"
#include "MathUtilsExtended.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

/**
 * @class MathUtils
 * @brief Static-only mathematics utility class for Spark Engine
 *
 * All methods are static; the class is never instantiated. It acts as a
 * well-documented namespace with grouping by purpose.
 *
 * Internally the implementation uses DirectX Math SIMD intrinsics whenever
 * the operation maps naturally (e.g. Normalize, Dot, Cross), falling back to
 * scalar arithmetic for simple scalar overloads.
 */
class MathUtils : public MathUtilsExtended
{
  public:
    // =========================================================================
    // Constants
    // =========================================================================

    /**
     * @brief The mathematical constant π (pi), accurate to IEEE 754 double precision.
     *
     * Use this instead of hard-coding 3.14159… to ensure consistency across the
     * engine. Approximately 3.14159265f.
     */
    static const float PI;

    /**
     * @brief Two times π (2π), equal to one full revolution in radians.
     *
     * Useful for wrapping angles and computing full-circle arc lengths.
     * Approximately 6.28318530f.
     */
    static const float TWO_PI;

    /**
     * @brief Half of π (π/2), representing a 90-degree angle in radians.
     *
     * Commonly used for right-angle calculations and gimbal-lock detection.
     * Approximately 1.57079632f.
     */
    static const float HALF_PI;

    /**
     * @brief Conversion factor from degrees to radians (π / 180).
     *
     * Multiply a degree value by this constant to convert to radians.
     * Approximately 0.01745329f.
     *
     * @code
     *   float rad = degrees * MathUtils::DEG_TO_RAD;
     * @endcode
     */
    static const float DEG_TO_RAD;

    /**
     * @brief Conversion factor from radians to degrees (180 / π).
     *
     * Multiply a radian value by this constant to convert to degrees.
     * Approximately 57.2957795f.
     *
     * @code
     *   float deg = radians * MathUtils::RAD_TO_DEG;
     * @endcode
     */
    static const float RAD_TO_DEG;


    // =========================================================================
    // Angle utilities
    // =========================================================================

    /**
     * @brief Convert an angle from degrees to radians.
     *
     * Equivalent to `degrees * (PI / 180.0f)`.
     *
     * @param degrees  Angle measured in degrees.
     * @return         Angle measured in radians.
     *
     * @code
     *   float fovRad = MathUtils::DegreesToRadians(90.0f); // 1.5708...
     * @endcode
     */
    static float DegreesToRadians(float degrees);

    /**
     * @brief Convert an angle from radians to degrees.
     *
     * Equivalent to `radians * (180.0f / PI)`.
     *
     * @param radians  Angle measured in radians.
     * @return         Angle measured in degrees.
     */
    static float RadiansToDegrees(float radians);

    /**
     * @brief Wrap an angle in radians to the range [0, 2π).
     *
     * Useful for keeping accumulated rotation values within a single revolution
     * to avoid floating-point precision loss over many frames.
     *
     * @param angle  Input angle in radians (any value).
     * @return       Equivalent angle in the range [0, 2π).
     */
    static float WrapAngle(float angle);

    /**
     * @brief Normalize an angle in radians to the range (-π, π].
     *
     * Converts an angle to its shortest equivalent representation around zero,
     * which is the preferred form for interpolation and angular comparisons.
     *
     * @param angle  Input angle in radians (any value).
     * @return       Equivalent angle in the range (-π, π].
     */
    static float NormalizeAngle(float angle);


    // =========================================================================
    // Interpolation
    // =========================================================================

    /**
     * @brief Linear interpolation between two scalar values.
     *
     * Computes `a + t * (b - a)`. When `t = 0` the result is `a`; when `t = 1`
     * the result is `b`. Values of `t` outside [0, 1] perform extrapolation.
     *
     * @param a  Start value (returned when t == 0).
     * @param b  End value (returned when t == 1).
     * @param t  Interpolation factor, typically in [0, 1].
     * @return   Linearly interpolated value between a and b.
     *
     * @code
     *   // Fade out a sound over 2 seconds
     *   float volume = MathUtils::Lerp(1.0f, 0.0f, elapsedTime / 2.0f);
     * @endcode
     */
    static float Lerp(float a, float b, float t);

    /**
     * @brief Generic linear interpolation for any arithmetic type.
     *
     * Constrainted to Spark::Arithmetic types (integers and floating-point).
     * Avoids ambiguity with the float and XMFLOAT3 overloads via concept constraint.
     *
     * @tparam T  Any arithmetic type (int, double, etc.).
     * @param a   Start value.
     * @param b   End value.
     * @param t   Interpolation factor in [0, 1].
     * @return    Interpolated value.
     */
    template <Spark::Arithmetic T> static T Lerp(T a, T b, float t)
    {
        return a + static_cast<T>(static_cast<float>(b - a) * t);
    }

    /**
     * @brief Linear interpolation between two 3D vectors.
     *
     * Performs component-wise linear interpolation on XMFLOAT3 vectors.
     * Equivalent to calling the scalar Lerp on each X, Y, Z component.
     *
     * @param a  Start vector (returned when t == 0).
     * @param b  End vector (returned when t == 1).
     * @param t  Interpolation factor, typically in [0, 1].
     * @return   Component-wise interpolated vector.
     *
     * @code
     *   // Smooth camera follow
     *   camera.position = MathUtils::Lerp(camera.position, target, 5.0f * dt);
     * @endcode
     */
    static XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t);

    /**
     * @brief Smooth Hermite interpolation between two values.
     *
     * Uses the smooth-step formula `t² * (3 - 2t)` which accelerates from `a`
     * and decelerates into `b`, producing a more natural-looking interpolation
     * than a plain Lerp for UI animations and camera transitions.
     *
     * @param a  Start value (returned when t == 0).
     * @param b  End value (returned when t == 1).
     * @param t  Interpolation factor, typically in [0, 1]. Values outside this
     *           range are NOT clamped; apply Clamp first if needed.
     * @return   Smoothly interpolated value.
     *
     * @code
     *   float alpha = MathUtils::SmoothStep(0.0f, 1.0f, progress);
     * @endcode
     */
    static float SmoothStep(float a, float b, float t);


    // =========================================================================
    // Distance calculations
    // =========================================================================

    /**
     * @brief Compute the Euclidean distance between two 3D points.
     *
     * Calculates `sqrt((b.x-a.x)² + (b.y-a.y)² + (b.z-a.z)²)`.
     * Prefer DistanceSquared() when comparing distances to avoid the square root.
     *
     * @param a  First point in world space.
     * @param b  Second point in world space.
     * @return   Distance between the two points in world units.
     */
    static float Distance(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Compute the squared Euclidean distance between two 3D points.
     *
     * Avoids the expensive sqrt call; use this when you only need to compare
     * distances (e.g. finding the nearest enemy) rather than the actual distance.
     *
     * @param a  First point in world space.
     * @param b  Second point in world space.
     * @return   Squared distance between the two points.
     *
     * @code
     *   // Fast nearest-enemy search (no sqrt needed for comparison)
     *   float nearestSq = FLT_MAX;
     *   for (auto& enemy : enemies)
     *   {
     *       float dSq = MathUtils::DistanceSquared(player.pos, enemy.pos);
     *       if (dSq < nearestSq) nearestSq = dSq;
     *   }
     * @endcode
     */
    static float DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Compute the unit direction vector from one point to another.
     *
     * Returns the normalized vector pointing from `from` to `to`. If the two
     * points are coincident the result is implementation-defined (typically a
     * zero vector); avoid calling with equal points.
     *
     * @param from  Origin point.
     * @param to    Destination point.
     * @return      Normalized direction vector (length ≈ 1.0).
     */
    static XMFLOAT3 Direction(const XMFLOAT3& from, const XMFLOAT3& to);


    // =========================================================================
    // Clamping
    // =========================================================================

    /**
     * @brief Clamp a float value to the range [min, max].
     *
     * Returns `min` if value < min, `max` if value > max, otherwise value.
     *
     * @param value  Input value to clamp.
     * @param min    Lower bound (inclusive).
     * @param max    Upper bound (inclusive).
     * @return       Clamped value in [min, max].
     *
     * @code
     *   health = MathUtils::Clamp(health, 0.0f, 100.0f);
     * @endcode
     */
    static float Clamp(float value, float min, float max);

    /**
     * @brief Clamp an integer value to the range [min, max].
     *
     * Returns `min` if value < min, `max` if value > max, otherwise value.
     *
     * @param value  Input value to clamp.
     * @param min    Lower bound (inclusive).
     * @param max    Upper bound (inclusive).
     * @return       Clamped value in [min, max].
     */
    static int Clamp(int value, int min, int max);

    /**
     * @brief Generic clamp for any arithmetic type.
     *
     * Constrainted to Spark::Arithmetic types to avoid ambiguity with the
     * float, int, and XMFLOAT3 overloads. Prefer the specific overloads for
     * float and int; use this for double, uint32_t, etc.
     *
     * @tparam T      Any arithmetic type (double, uint32_t, etc.).
     * @param value   Input value to clamp.
     * @param minVal  Lower bound (inclusive).
     * @param maxVal  Upper bound (inclusive).
     * @return        Clamped value in [minVal, maxVal].
     */
    template <Spark::Arithmetic T> static T Clamp(T value, T minVal, T maxVal)
    {
        return value < minVal ? minVal : (value > maxVal ? maxVal : value);
    }

    /**
     * @brief Clamp each component of a 3D vector independently.
     *
     * Applies scalar Clamp to the X, Y, and Z components individually using
     * the corresponding components of the min and max vectors as bounds.
     *
     * @param value  Input vector to clamp.
     * @param min    Per-component lower bounds.
     * @param max    Per-component upper bounds.
     * @return       Vector with each component clamped to [min, max].
     */
    static XMFLOAT3 Clamp(const XMFLOAT3& value, const XMFLOAT3& min, const XMFLOAT3& max);


    // =========================================================================
    // Vector operations
    // =========================================================================

    /**
     * @brief Add two 3D vectors component-wise.
     * @param a  First operand.
     * @param b  Second operand.
     * @return   Component-wise sum a + b.
     */
    static XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Subtract vector b from vector a component-wise.
     * @param a  Minuend.
     * @param b  Subtrahend.
     * @return   Component-wise difference a - b.
     */
    static XMFLOAT3 Subtract(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Scale a 3D vector by a scalar.
     * @param v       Vector to scale.
     * @param scalar  Scale factor applied to all components.
     * @return        Scaled vector v * scalar.
     */
    static XMFLOAT3 Multiply(const XMFLOAT3& v, float scalar);

    /**
     * @brief Divide a 3D vector by a scalar.
     *
     * @warning Passing scalar == 0 results in undefined behavior (division by zero).
     *          Guard against zero before calling.
     *
     * @param v       Vector to divide.
     * @param scalar  Divisor (must be non-zero).
     * @return        Divided vector v / scalar.
     */
    static XMFLOAT3 Divide(const XMFLOAT3& v, float scalar);

    /**
     * @brief Compute the dot product of two 3D vectors.
     *
     * Returns `a.x*b.x + a.y*b.y + a.z*b.z`. Useful for computing the
     * cosine of the angle between two unit vectors: `dot = cos(θ)`.
     *
     * @param a  First vector.
     * @param b  Second vector.
     * @return   Scalar dot product.
     *
     * @code
     *   float facing = MathUtils::Dot(forward, toEnemy);
     *   bool inFront = facing > 0.0f;
     * @endcode
     */
    static float Dot(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Compute the cross product of two 3D vectors.
     *
     * Returns a vector perpendicular to both `a` and `b` following the
     * right-hand rule. The magnitude equals `|a| * |b| * sin(θ)`.
     *
     * @param a  First vector.
     * @param b  Second vector.
     * @return   Cross product vector a × b.
     *
     * @code
     *   XMFLOAT3 normal = MathUtils::Normalize(MathUtils::Cross(edge1, edge2));
     * @endcode
     */
    static XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Normalize a 3D vector to unit length.
     *
     * Returns a vector with the same direction but length 1.0. If the input
     * vector has zero length the result is implementation-defined (typically
     * zero or NaN); avoid normalizing zero vectors.
     *
     * @param v  Input vector (must not be zero-length).
     * @return   Unit vector in the same direction as v.
     */
    static XMFLOAT3 Normalize(const XMFLOAT3& v);

    /**
     * @brief Compute the length (magnitude) of a 3D vector.
     *
     * Returns `sqrt(v.x² + v.y² + v.z²)`. Use LengthSquared() when only a
     * relative comparison is needed.
     *
     * @param v  Input vector.
     * @return   Length of the vector.
     */
    static float Length(const XMFLOAT3& v);

    // =========================================================================
    // Quaternion / Euler conversion
    // =========================================================================

    /**
     * @brief Convert a quaternion to Euler angles in degrees (roll, pitch, yaw).
     *
     * Uses the standard ZYX convention. Returns angles in degrees.
     * Handles gimbal lock near ±90° pitch.
     *
     * @param qx  X component of the quaternion.
     * @param qy  Y component of the quaternion.
     * @param qz  Z component of the quaternion.
     * @param qw  W component of the quaternion.
     * @return     XMFLOAT3 with (roll, pitch, yaw) in degrees.
     */
    static XMFLOAT3 QuaternionToEulerDegrees(float qx, float qy, float qz, float qw);

    /**
     * @brief Convert Euler angles in degrees to a quaternion.
     *
     * Uses the ZYX convention matching QuaternionToEulerDegrees.
     *
     * @param rollDeg   Roll angle in degrees (X axis).
     * @param pitchDeg  Pitch angle in degrees (Y axis).
     * @param yawDeg    Yaw angle in degrees (Z axis).
     * @return          XMFLOAT4 quaternion (x, y, z, w).
     */
    static XMFLOAT4 EulerDegreesToQuaternion(float rollDeg, float pitchDeg, float yawDeg);


    /**
     * @brief Compute the squared length of a 3D vector.
     *
     * Avoids the sqrt call. Prefer this over Length() when comparing magnitudes.
     *
     * @param v  Input vector.
     * @return   Squared length v.x² + v.y² + v.z².
     */
    static float LengthSquared(const XMFLOAT3& v);
};
