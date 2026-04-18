/**
 * @file VolumetricClouds.h
 * @brief Ray-marched volumetric cloud system with procedural noise coverage
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a two-scale volumetric cloud model inspired by the Decima Engine
 * "Nubis" system and GPU Pro 7. Coverage, cloud type and wetness are driven
 * by a 2D weather map. Cloud density is modulated by a low-frequency Perlin
 * base and detail-eroded by high-frequency Worley noise. Lighting is evaluated
 * with the Henyey-Greenstein phase function and a multiple-scattering
 * approximation.
 *
 * The class ships a CPU reference path that is unit-tested headlessly; GPU
 * resources are created on demand via an RHI device and the density/coverage
 * textures are uploaded so a compute/pixel shader can raymarch on the GPU.
 *
 * ## Usage
 * @code
 *   auto& clouds = Spark::Graphics::VolumetricCloudSystem::GetInstance();
 *   VolumetricCloudSettings settings;
 *   settings.coverage = 0.55f;
 *   settings.windDirectionX = 1.0f;
 *   clouds.Initialize(settings);
 *
 *   // Per-frame
 *   clouds.Update(dt);
 *   clouds.SetSunDirection({0.0f, 0.8f, 0.6f});
 *   CloudSample s = clouds.SampleAlongRay({rx, ry, rz}, {dx, dy, dz}, 2000.0f);
 * @endcode
 *
 * @see SkyAtmosphere.h, WeatherSystem.h, FroxelVolumetricFog.h
 */

#pragma once

#include "../Core/Platform.h"
#include "RHI/RHI.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Cloud Settings
    // =========================================================================

    /**
     * @brief Configuration for the volumetric cloud layer.
     *
     * Altitudes are expressed in world units (metres). Defaults correspond to a
     * typical cumulus layer between 1.5 km and 4 km with a 55 % coverage.
     */
    struct VolumetricCloudSettings
    {
        float coverage = 0.55f;       ///< Global coverage multiplier [0..1]
        float cloudType = 0.5f;       ///< 0 = stratus, 0.5 = cumulus, 1 = cumulonimbus
        float density = 0.04f;        ///< Extinction per metre at peak
        float anvilBias = 0.1f;       ///< Flattens top of cumulonimbus
        float baseAltitude = 1500.0f; ///< Bottom of cloud layer (m)
        float topAltitude = 4000.0f;  ///< Top of cloud layer (m)
        float baseNoiseScale = 1.0f / 2048.0f;
        float detailNoiseScale = 1.0f / 128.0f;
        float detailStrength = 0.35f;
        float weatherScale = 1.0f / 32768.0f;

        float windDirectionX = 1.0f;
        float windDirectionY = 0.0f;
        float windDirectionZ = 0.0f;
        float windSpeed = 5.0f; ///< Metres per second

        float phaseG0 = 0.8f;      ///< Forward scattering lobe (Mie, strong)
        float phaseG1 = -0.3f;     ///< Back-scattering lobe
        float phaseBlend = 0.5f;   ///< Blend between the two lobes [0..1]
        float silverLining = 0.6f; ///< Artistic silver-lining bias
        uint32_t marchSteps = 64;  ///< Primary march steps (GPU quality hint)
        uint32_t lightSteps = 6;   ///< Light march steps
        float lightMarchDistance = 600.0f;
        bool enabled = true;
    };

    // =========================================================================
    // Cloud Sample Result
    // =========================================================================

    /**
     * @brief Result of integrating the cloud layer along a view ray.
     */
    struct CloudSample
    {
        float luminanceR = 0.0f;    ///< Inscattered radiance (R)
        float luminanceG = 0.0f;    ///< Inscattered radiance (G)
        float luminanceB = 0.0f;    ///< Inscattered radiance (B)
        float transmittance = 1.0f; ///< Sky/background visibility along the ray
    };

    // =========================================================================
    // Volumetric Cloud System
    // =========================================================================

    /**
     * @brief CPU reference + GPU feeder for volumetric cloud rendering.
     *
     * The class holds three small 3D noise caches and a 2D weather map. Heavy
     * lifting on the GPU happens in HLSL using the same formulas; these CPU
     * paths exist for headless tests and for runtime fallback when no RHI
     * device is attached.
     */
    class VolumetricCloudSystem
    {
      public:
        static VolumetricCloudSystem& GetInstance();

        VolumetricCloudSystem(const VolumetricCloudSystem&) = delete;
        VolumetricCloudSystem& operator=(const VolumetricCloudSystem&) = delete;

        /// @brief Initialize with default settings
        bool Initialize();

        /// @brief Initialize with the supplied settings
        bool Initialize(const VolumetricCloudSettings& settings);

        /// @brief Advance internal time (wind offset, detail erosion) by @p dt seconds
        void Update(float dt);

        /// @brief Release CPU/GPU resources
        void Shutdown();

        /// @brief Replace the whole settings block
        void SetSettings(const VolumetricCloudSettings& settings);

        const VolumetricCloudSettings& GetSettings() const { return m_settings; }

        /// @brief Direction pointing towards the sun (will be normalized)
        void SetSunDirection(float x, float y, float z);

        /// @brief Linear RGB color of direct sunlight above the layer
        void SetSunColor(float r, float g, float b);

        /// @brief Set coverage/type/rain from the weather system
        void SetWeather(float coverage, float cloudType, float rain);

        /**
         * @brief Sample cloud density at a world point.
         * @return Extinction density in [0..1]
         */
        float SampleDensity(float x, float y, float z) const;

        /**
         * @brief Integrate the cloud layer along a ray.
         *
         * @param originX,originY,originZ  Ray origin in world space
         * @param dirX,dirY,dirZ           Ray direction (will be normalized)
         * @param maxDistance              Maximum integration distance in metres
         * @return Inscattered luminance + ray transmittance
         */
        CloudSample SampleAlongRay(float originX, float originY, float originZ, float dirX, float dirY, float dirZ,
                                   float maxDistance) const;

        /// @brief Number of seconds accumulated since Initialize
        float GetTime() const { return m_time; }

        bool IsInitialized() const { return m_initialized; }

        // ---- GPU resources ----

        /// @brief Create a 3D base-noise texture and a 2D weather map on the device
        bool CreateGPUResources(Spark::RHI::IRHIDevice* device);

        /// @brief Upload latest CPU-generated noise/weather to the GPU textures
        void UploadToGPU(Spark::RHI::IRHIDevice* device);

        Spark::RHI::IRHITexture* GetBaseNoiseTexture() const { return m_gpuBaseNoise.get(); }
        Spark::RHI::IRHITexture* GetDetailNoiseTexture() const { return m_gpuDetailNoise.get(); }
        Spark::RHI::IRHITexture* GetWeatherMapTexture() const { return m_gpuWeatherMap.get(); }

        bool HasGPUResources() const { return m_gpuBaseNoise != nullptr; }

      private:
        VolumetricCloudSystem() = default;
        ~VolumetricCloudSystem() = default;

        void RegenerateNoise();
        void RegenerateWeatherMap();

        static float Hash(uint32_t x, uint32_t y, uint32_t z);
        static float SmoothNoise3D(const std::vector<float>& grid, uint32_t size, float x, float y, float z);
        float SampleBaseNoise(float x, float y, float z) const;
        float SampleDetailNoise(float x, float y, float z) const;
        float SampleWeatherMap(float x, float z) const;
        static float HenyeyGreenstein(float cosTheta, float g);
        static float HeightGradient(float h, float cloudType);

        VolumetricCloudSettings m_settings{};
        float m_time = 0.0f;
        float m_sunDirX = 0.0f, m_sunDirY = 1.0f, m_sunDirZ = 0.0f;
        float m_sunR = 1.0f, m_sunG = 0.95f, m_sunB = 0.85f;
        float m_rainWetness = 0.0f;
        bool m_initialized = false;

        static constexpr uint32_t kBaseSize = 32;
        static constexpr uint32_t kDetailSize = 16;
        static constexpr uint32_t kWeatherSize = 64;

        std::vector<float> m_baseNoise;   ///< kBaseSize^3 Perlin-like noise
        std::vector<float> m_detailNoise; ///< kDetailSize^3 Worley-like noise
        std::vector<float> m_weatherMap;  ///< kWeatherSize^2 RG: coverage, type

        std::unique_ptr<Spark::RHI::IRHITexture> m_gpuBaseNoise;
        std::unique_ptr<Spark::RHI::IRHITexture> m_gpuDetailNoise;
        std::unique_ptr<Spark::RHI::IRHITexture> m_gpuWeatherMap;
    };

} // namespace Spark::Graphics
