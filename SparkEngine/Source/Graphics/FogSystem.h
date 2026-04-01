/**
 * @file FogSystem.h
 * @brief Distance fog, height fog, and volumetric fog rendering
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides multiple fog models for atmospheric depth rendering:
 * - Linear distance fog (start/end range)
 * - Exponential distance fog (density-based falloff)
 * - Height-based fog (ground-hugging fog with altitude falloff)
 * - Volumetric fog (ray-marched with light scattering)
 *
 * Integrates with the lighting system for fog light scattering and
 * the weather system for dynamic fog density changes.
 *
 * ## Usage
 * @code
 *   FogSystem fog;
 *   fog.Initialize();
 *   fog.SetMode(FogMode::ExponentialHeight);
 *   fog.SetDensity(0.02f);
 *   fog.SetHeightFalloff(0.5f);
 *   fog.SetColor({0.7f, 0.75f, 0.8f});
 *
 *   float factor = fog.ComputeFogFactor(cameraPos, fragmentPos);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"
#include "../Utils/MathUtils.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdint>

namespace Spark
{
    namespace Graphics
    {

        // =============================================================================
        // Fog Mode
        // =============================================================================

        enum class FogMode
        {
            None,
            Linear,             ///< Linear interpolation between start and end distance
            Exponential,        ///< exp(-density * distance)
            ExponentialSquared, ///< exp(-(density * distance)^2)
            Height,             ///< Height-based exponential fog
            ExponentialHeight,  ///< Combined distance + height exponential fog
            Volumetric          ///< Ray-marched volumetric fog with scattering
        };

        // =============================================================================
        // Fog Color
        // =============================================================================

        struct FogColor
        {
            float r = 0.7f, g = 0.75f, b = 0.8f, a = 1.0f;

            static FogColor White() { return {1.0f, 1.0f, 1.0f, 1.0f}; }
            static FogColor Gray() { return {0.5f, 0.5f, 0.5f, 1.0f}; }
            static FogColor BluishGray() { return {0.7f, 0.75f, 0.8f, 1.0f}; }
            static FogColor Warm() { return {0.85f, 0.75f, 0.6f, 1.0f}; }

            FogColor Lerp(const FogColor& other, float t) const
            {
                return {r + (other.r - r) * t, g + (other.g - g) * t, b + (other.b - b) * t, a + (other.a - a) * t};
            }
        };

        // =============================================================================
        // Fog Settings
        // =============================================================================

        struct LinearFogSettings
        {
            float start = 20.0f; ///< Distance where fog begins
            float end = 200.0f;  ///< Distance where fog is fully opaque
        };

        struct ExponentialFogSettings
        {
            float density = 0.01f; ///< Fog density coefficient
        };

        struct HeightFogSettings
        {
            float baseHeight = 0.0f;    ///< Height at which fog is densest
            float heightFalloff = 0.5f; ///< Rate of density decrease with altitude
            float maxHeight = 100.0f;   ///< Height above which fog is zero
            float baseDensity = 0.02f;  ///< Fog density at base height
        };

        struct VolumetricFogSettings
        {
            int rayMarchSteps = 32;               ///< Number of ray march steps
            float maxDistance = 200.0f;           ///< Maximum ray march distance
            float scatteringCoefficient = 0.02f;  ///< Light scattering amount
            float absorptionCoefficient = 0.005f; ///< Light absorption amount
            float phaseG = 0.3f;                  ///< Henyey-Greenstein asymmetry (-1 to 1)
            float noiseScale = 0.01f;             ///< Noise texture scale for density variation
            float noiseIntensity = 0.5f;          ///< How much noise affects density
            float temporalBlend = 0.9f;           ///< Temporal reprojection blend factor
        };

        // =============================================================================
        // Fog Compute Utilities
        // =============================================================================

        namespace FogMath
        {

            /// Linear fog factor: 0 = no fog, 1 = full fog
            inline float LinearFog(float distance, float start, float end)
            {
                if (end <= start)
                    return 0.0f;
                return std::clamp((distance - start) / (end - start), 0.0f, 1.0f);
            }

            /// Exponential fog factor
            inline float ExponentialFog(float distance, float density)
            {
                return 1.0f - std::exp(-density * distance);
            }

            /// Exponential squared fog factor
            inline float ExponentialSquaredFog(float distance, float density)
            {
                float d = density * distance;
                float exponent = -(d * d);
                // Clamp to prevent constant-arithmetic overflow (MSVC C4756)
                exponent = std::max(exponent, -87.0f);
                return 1.0f - std::exp(exponent);
            }

            /// Height-based fog density at a given altitude
            inline float HeightDensity(float y, float baseHeight, float heightFalloff, float baseDensity)
            {
                float heightAboveBase = y - baseHeight;
                if (heightAboveBase < 0.0f)
                    return baseDensity;
                return baseDensity * std::exp(-heightFalloff * heightAboveBase);
            }

            /// Integrated height fog along a ray from camera to fragment
            inline float IntegratedHeightFog(float camY, float fragY, float distance, float baseHeight,
                                             float heightFalloff, float baseDensity)
            {
                if (heightFalloff < 0.0001f)
                    return ExponentialFog(distance, baseDensity);

                float densityCam = HeightDensity(camY, baseHeight, heightFalloff, baseDensity);
                float densityFrag = HeightDensity(fragY, baseHeight, heightFalloff, baseDensity);
                float avgDensity = (densityCam + densityFrag) * 0.5f;

                return 1.0f - std::exp(-avgDensity * distance);
            }

            /// Henyey-Greenstein phase function for volumetric scattering
            inline float HenyeyGreenstein(float cosTheta, float g)
            {
                float g2 = g * g;
                float denom = 1.0f + g2 - 2.0f * g * cosTheta;
                if (denom < 0.0001f)
                    return 1.0f;
                return (1.0f - g2) / (4.0f * MathUtils::PI * std::pow(denom, 1.5f));
            }

            /// Beer-Lambert transmittance
            inline float BeerLambert(float opticalDepth)
            {
                return std::exp(-opticalDepth);
            }

        } // namespace FogMath

        // =============================================================================
        // Fog System
        // =============================================================================

        struct FogMetrics
        {
            int activeMode = 0;
            float currentDensity = 0.0f;
            float maxFogDistance = 0.0f;
            int volumetricSamples = 0;
        };

        class FogSystem
        {
          public:
            FogSystem() = default;
            ~FogSystem() = default;

            /// @brief Initialize the fog system
            bool Initialize();

            /// @brief Shut down the fog system
            void Shutdown();

            // ---- Mode and Settings ----

            /// @brief Set the active fog mode
            void SetMode(FogMode mode);
            FogMode GetMode() const { return m_mode; }

            void SetEnabled(bool enabled) { m_enabled = enabled; }
            bool IsEnabled() const { return m_enabled; }

            void SetColor(const FogColor& color) { m_color = color; }
            const FogColor& GetColor() const { return m_color; }

            /// @brief Set fog density for exponential and height modes
            void SetDensity(float density);
            float GetDensity() const { return m_exponentialSettings.density; }

            /// @brief Set linear fog start and end distances
            void SetLinearRange(float start, float end);
            const LinearFogSettings& GetLinearSettings() const { return m_linearSettings; }

            // Height fog settings
            void SetBaseHeight(float height) { m_heightSettings.baseHeight = height; }
            void SetHeightFalloff(float falloff) { m_heightSettings.heightFalloff = std::max(falloff, 0.0f); }
            void SetMaxHeight(float maxHeight) { m_heightSettings.maxHeight = maxHeight; }
            const HeightFogSettings& GetHeightSettings() const { return m_heightSettings; }

            // Volumetric fog settings
            void SetVolumetricSettings(const VolumetricFogSettings& settings) { m_volumetricSettings = settings; }
            const VolumetricFogSettings& GetVolumetricSettings() const { return m_volumetricSettings; }

            // ---- Fog Computation ----

            /**
             * @brief Compute fog factor for a fragment at given distance from camera
             * @param distance Distance from camera to fragment
             * @return Fog factor: 0 = no fog, 1 = fully fogged
             */
            float ComputeFogFactor(float distance) const;

            /**
             * @brief Compute height-aware fog factor
             * @param cameraY Camera Y position
             * @param fragmentY Fragment world Y position
             * @param distance Distance from camera to fragment
             * @return Fog factor: 0 = no fog, 1 = fully fogged
             */
            float ComputeHeightFogFactor(float cameraY, float fragmentY, float distance) const;

            /**
             * @brief Apply fog to a color value
             * @param sceneColor Original scene color (RGB)
             * @param fogFactor Fog amount (0 to 1)
             * @return Fogged color
             */
            FogColor ApplyFog(const FogColor& sceneColor, float fogFactor) const;

            // ---- Metrics ----

            /// @brief Get current fog system metrics
            FogMetrics GetMetrics() const;

            /// @brief Get console status string
            std::string Console_GetStatus() const;

          private:
            bool m_initialized = false;
            bool m_enabled = true;
            FogMode m_mode = FogMode::None;

            FogColor m_color = FogColor::BluishGray();
            LinearFogSettings m_linearSettings;
            ExponentialFogSettings m_exponentialSettings;
            HeightFogSettings m_heightSettings;
            VolumetricFogSettings m_volumetricSettings;
        };

    } // namespace Graphics
} // namespace Spark
