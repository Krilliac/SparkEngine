/**
 * @file AngleUtils.h
 * @brief Constexpr angle arithmetic helpers (normalize, delta, lerp)
 * @author Spark Engine Team
 * @date 2026
 *
 * Replaces scattered angle normalization and interpolation code in AI,
 * camera, and animation systems with tested, wrapping-aware functions.
 * All functions operate in radians and are fully `constexpr`.
 *
 * ## Usage
 * @code
 *   float heading = Spark::AngleUtils::NormalizeAngle(rawAngle);
 *   float turn = Spark::AngleUtils::AngleDelta(currentAngle, targetAngle);
 *   float smooth = Spark::AngleUtils::LerpAngle(from, to, 0.5f);
 * @endcode
 */

#pragma once

namespace Spark::AngleUtils
{

    inline constexpr float PI = 3.14159265358979323846f;
    inline constexpr float TWO_PI = 2.0f * PI;
    inline constexpr float DEG_TO_RAD = PI / 180.0f;
    inline constexpr float RAD_TO_DEG = 180.0f / PI;

    /**
     * @brief Convert degrees to radians.
     */
    [[nodiscard]] constexpr float ToRadians(float degrees) noexcept
    {
        return degrees * DEG_TO_RAD;
    }

    /**
     * @brief Convert radians to degrees.
     */
    [[nodiscard]] constexpr float ToDegrees(float radians) noexcept
    {
        return radians * RAD_TO_DEG;
    }

    namespace Detail
    {
        // Constexpr-safe fmod that works across all compilers
        [[nodiscard]] constexpr float Fmod(float a, float b) noexcept
        {
            return a - static_cast<float>(static_cast<int>(a / b)) * b;
        }
    } // namespace Detail

    /**
     * @brief Normalize an angle to the range [-PI, PI).
     */
    [[nodiscard]] constexpr float NormalizeAngle(float radians) noexcept
    {
        radians = Detail::Fmod(radians + PI, TWO_PI);
        if (radians < 0.0f)
            radians += TWO_PI;
        return radians - PI;
    }

    /**
     * @brief Compute the shortest signed angular difference from `from` to `to`.
     * @return A value in [-PI, PI) representing the shortest rotation.
     */
    [[nodiscard]] constexpr float AngleDelta(float from, float to) noexcept
    {
        return NormalizeAngle(to - from);
    }

    /**
     * @brief Interpolate between two angles along the shortest arc.
     * @param from Starting angle in radians.
     * @param to   Target angle in radians.
     * @param t    Interpolation factor [0, 1].
     */
    [[nodiscard]] constexpr float LerpAngle(float from, float to, float t) noexcept
    {
        return from + AngleDelta(from, to) * t;
    }

} // namespace Spark::AngleUtils
