/**
 * @file SkyAtmosphere.h
 * @brief Sky and atmosphere rendering using a simplified Preetham sky model
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements an analytical sky model based on the Preetham 1999 paper
 * "A Practical Analytic Model for Daylight". Computes sky luminance from
 * view direction and sun position using five perez coefficients per channel.
 *
 * ## Usage
 * @code
 *   auto& sky = Spark::Graphics::SkyAtmosphereSystem::GetInstance();
 *   sky.Initialize();
 *   sky.SetTurbidity(3.0f);
 *   sky.SetSunDirection({0.0f, -0.7f, 0.7f});
 *
 *   SkyColor color = sky.ComputeSkyColor({0.0f, 0.5f, 0.5f});
 * @endcode
 *
 * @see WeatherSystem, FogSystem
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Spark::Graphics
{

    // =========================================================================
    // Sky Settings
    // =========================================================================

    /**
     * @brief Configuration for the atmospheric sky rendering.
     */
    struct SkySettings
    {
        float turbidity = 2.0f;                      ///< Atmospheric haze (1 = clear, 10 = heavy)
        float sunIntensity = 20.0f;                  ///< Brightness multiplier for direct sunlight
        XMFLOAT3 sunDirection = {0.0f, -0.5f, 0.5f}; ///< Direction TO the sun (normalized)
        float exposure = 1.0f;                       ///< HDR exposure control
        float groundAlbedo = 0.3f;                   ///< Ground reflectance for multi-scattering
        bool enabled = true;                         ///< Master enable/disable
    };

    // =========================================================================
    // Sky Color
    // =========================================================================

    /**
     * @brief Linear RGB color returned by sky evaluation functions.
     */
    struct SkyColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;

        /** @brief Linearly interpolate between this color and another. */
        SkyColor Lerp(const SkyColor& other, float t) const
        {
            return {r + (other.r - r) * t, g + (other.g - g) * t, b + (other.b - b) * t};
        }

        /** @brief Scale all channels by a uniform factor. */
        SkyColor operator*(float s) const { return {r * s, g * s, b * s}; }
    };

    // =========================================================================
    // Preetham Perez Coefficients
    // =========================================================================

    /**
     * @brief The five Perez distribution coefficients (A through E) for one color channel.
     *
     * Used internally by the Preetham sky model to evaluate the luminance
     * distribution function at a given view angle and sun position.
     */
    struct PerezCoefficients
    {
        float A = 0.0f; ///< Darkening/brightening of horizon
        float B = 0.0f; ///< Luminance gradient near the horizon
        float C = 0.0f; ///< Relative intensity of circumsolar region
        float D = 0.0f; ///< Width of the circumsolar region
        float E = 0.0f; ///< Relative backscattered light
    };

    // =========================================================================
    // SkyAtmosphereSystem
    // =========================================================================

    /**
     * @class SkyAtmosphereSystem
     * @brief Analytical sky rendering using the Preetham/Perez distribution model.
     *
     * Evaluates sky luminance for any view direction given the current sun position
     * and atmospheric turbidity. The system is CPU-side; the computed colors can feed
     * a skybox or be passed to shaders as uniform data.
     */
    class SkyAtmosphereSystem
    {
      public:
        /** @brief Singleton access. */
        static SkyAtmosphereSystem& GetInstance()
        {
            static SkyAtmosphereSystem s;
            return s;
        }

        /**
         * @brief Initialize the sky system and compute initial coefficients.
         * @return True on success.
         */
        bool Initialize();

        /** @brief Release resources. */
        void Shutdown();

        // ---- Settings ----

        void SetSettings(const SkySettings& settings);
        const SkySettings& GetSettings() const { return m_settings; }

        void SetSunDirection(const XMFLOAT3& dir);
        void SetTurbidity(float turbidity);

        // ---- Sky Evaluation ----

        /**
         * @brief Compute the sky color for a given view direction.
         *
         * Evaluates the Preetham/Perez luminance distribution for the specified
         * view ray, accounting for turbidity, sun position, and exposure.
         *
         * @param viewDir  Normalized direction from the camera into the sky.
         * @return         Linear HDR sky color.
         */
        SkyColor ComputeSkyColor(const XMFLOAT3& viewDir) const;

        /** @brief Compute the color/intensity of the sun disc. */
        SkyColor ComputeSunColor() const;

        /** @brief Get the sky color directly overhead (zenith). */
        SkyColor GetZenithColor() const;

        /** @brief Get the sky color at the horizon. */
        SkyColor GetHorizonColor() const;

        /**
         * @brief Update per frame — optionally animates sun if linked to time-of-day.
         * @param deltaTime  Frame time in seconds.
         */
        void Update(float deltaTime);

      private:
        SkyAtmosphereSystem() = default;
        ~SkyAtmosphereSystem() = default;
        SkyAtmosphereSystem(const SkyAtmosphereSystem&) = delete;
        SkyAtmosphereSystem& operator=(const SkyAtmosphereSystem&) = delete;

        /** @brief Recalculate Perez coefficients from current turbidity. */
        void RecalculateCoefficients();

        /** @brief Compute zenith luminance/chrominance from sun elevation and turbidity. */
        void ComputeZenithValues();

        /**
         * @brief Evaluate the Perez distribution function.
         * @param theta      Angle from zenith to sample direction.
         * @param gamma      Angle between sample direction and sun direction.
         * @param coeffs     Perez A-E coefficients.
         * @return           Distribution value (unnormalized).
         */
        static float EvaluatePerez(float theta, float gamma, const PerezCoefficients& coeffs);

        /** @brief Normalize a 3D direction vector in place. */
        static XMFLOAT3 NormalizeDirection(const XMFLOAT3& v);

        bool m_initialized = false;
        SkySettings m_settings;

        // Perez coefficients per channel (Y luminance, x chrominance, y chrominance)
        PerezCoefficients m_perezY;
        PerezCoefficients m_perezx;
        PerezCoefficients m_perezy;

        // Zenith values
        float m_zenithY = 0.0f; ///< Zenith luminance
        float m_zenithx = 0.0f; ///< Zenith chrominance x
        float m_zenithy = 0.0f; ///< Zenith chrominance y

        float m_sunTheta = 0.0f; ///< Sun zenith angle (radians)
    };

} // namespace Spark::Graphics
