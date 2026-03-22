/**
 * @file FroxelVolumetricFog.h
 * @brief Froxel-based volumetric fog using a frustum-aligned 3D grid
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements volumetric fog via a froxel (frustum-voxel) grid. The camera
 * frustum is subdivided into a 3D grid with logarithmic depth distribution
 * for higher near-camera resolution.
 *
 * Three-pass pipeline:
 * 1. Media injection: compute inscattering + extinction per froxel
 * 2. Light scattering: evaluate lights with Henyey-Greenstein phase function
 * 3. Temporal filtering: blend with previous frame using motion vectors
 *
 * Final fog factor is integrated via Beer-Lambert transmittance.
 *
 * @see FogSystem.h, LightingSystem.h, ClusteredLightCulling.h
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    static constexpr float FROXEL_PI = 3.14159265358979323846f;

    // =========================================================================
    // Froxel Grid Configuration
    // =========================================================================

    struct FroxelFogSettings
    {
        uint32_t gridWidth = 160;     ///< Froxels along X (screen width / 12 at 1080p)
        uint32_t gridHeight = 90;     ///< Froxels along Y (screen height / 12 at 1080p)
        uint32_t gridDepth = 64;      ///< Froxels along Z (depth slices)
        float nearPlane = 0.1f;       ///< Camera near plane
        float farPlane = 200.0f;      ///< Maximum fog distance
        float logDistribution = 0.5f; ///< Blend between linear (0) and logarithmic (1) depth

        float globalDensity = 0.02f;     ///< Base fog density (extinction coefficient)
        float globalScattering = 0.015f; ///< Base scattering coefficient
        float globalAbsorption = 0.005f; ///< Base absorption coefficient
        float phaseG = 0.3f;             ///< Henyey-Greenstein asymmetry parameter [-1, 1]

        float ambientR = 0.05f; ///< Ambient inscattering R
        float ambientG = 0.06f; ///< Ambient inscattering G
        float ambientB = 0.08f; ///< Ambient inscattering B

        float temporalBlend = 0.9f;              ///< History blend factor [0..1]
        float temporalRejectionThreshold = 0.1f; ///< Motion rejection threshold
    };

    // =========================================================================
    // Point Light for Fog Scattering
    // =========================================================================

    struct FroxelLight
    {
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f;
        float intensity = 1.0f;
        float radius = 10.0f; ///< Attenuation radius
    };

    // =========================================================================
    // Local Fog Volume
    // =========================================================================

    struct FogVolume
    {
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float extentX = 5.0f, extentY = 5.0f, extentZ = 5.0f;
        float density = 0.05f;
        float scattering = 0.04f;
        float absorption = 0.01f;
    };

    // =========================================================================
    // Per-Froxel Data
    // =========================================================================

    struct FroxelData
    {
        float inscatterR = 0.0f; ///< Accumulated inscattered light R
        float inscatterG = 0.0f; ///< Accumulated inscattered light G
        float inscatterB = 0.0f; ///< Accumulated inscattered light B
        float extinction = 0.0f; ///< Total extinction coefficient
    };

    // =========================================================================
    // Fog Query Result
    // =========================================================================

    struct FroxelFogResult
    {
        float fogR = 0.0f; ///< Inscattered light to blend in
        float fogG = 0.0f;
        float fogB = 0.0f;
        float transmittance = 1.0f; ///< Scene color multiplier (Beer-Lambert)
    };

    // =========================================================================
    // Froxel Volumetric Fog System
    // =========================================================================

    /**
     * @brief CPU reference implementation of froxel-based volumetric fog
     *
     * GPU implementation maps each pass to a compute shader dispatch.
     * The froxel grid is stored as a flat 3D array indexed by
     * (sliceZ * gridWidth * gridHeight + y * gridWidth + x).
     */
    class FroxelVolumetricFog
    {
      public:
        FroxelVolumetricFog() = default;
        ~FroxelVolumetricFog() = default;

        /// @brief Initialize the froxel grid with the given settings
        bool Initialize(const FroxelFogSettings& settings);

        /// @brief Shutdown and release buffers
        void Shutdown();

        /// @brief Convert a depth slice index to a linear depth value
        float SliceToDepth(uint32_t slice) const;

        /// @brief Convert a linear depth to a fractional depth slice index
        float DepthToSlice(float depth) const;

        /**
         * @brief Pass 1: Inject media properties into the froxel grid
         * @param volumes     Array of local fog volumes
         * @param volumeCount Number of volumes
         */
        void InjectMedia(const FogVolume* volumes, int volumeCount);

        /**
         * @brief Pass 2: Evaluate light scattering per froxel
         * @param lights       Array of point lights
         * @param lightCount   Number of lights
         * @param cameraPosX   Camera world position X
         * @param cameraPosY   Camera world position Y
         * @param cameraPosZ   Camera world position Z
         */
        void ScatterLight(const FroxelLight* lights, int lightCount, float cameraPosX, float cameraPosY,
                          float cameraPosZ);

        /**
         * @brief Pass 3: Temporal reprojection and filtering
         * @param motionScale  Global motion magnitude for rejection (0 = static)
         */
        void TemporalFilter(float motionScale = 0.0f);

        /// @brief Integrate fog along view rays using Beer-Lambert transmittance
        void Integrate();

        /**
         * @brief Query fog at a given screen UV and depth
         * @param u, v    Screen UV coordinates [0, 1]
         * @param depth   Linear depth in world units
         * @return Fog inscattering (RGB) and transmittance
         */
        FroxelFogResult QueryFog(float u, float v, float depth) const;

        /// @brief Get current settings
        const FroxelFogSettings& GetSettings() const { return m_settings; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Get total froxel count
        uint32_t GetFroxelCount() const { return m_settings.gridWidth * m_settings.gridHeight * m_settings.gridDepth; }

      private:
        /// @brief Compute flat index from 3D froxel coordinates
        uint32_t FroxelIndex(uint32_t x, uint32_t y, uint32_t z) const
        {
            return z * m_settings.gridWidth * m_settings.gridHeight + y * m_settings.gridWidth + x;
        }

        /// @brief Henyey-Greenstein phase function
        static float HenyeyGreenstein(float cosTheta, float g);

        FroxelFogSettings m_settings;
        std::vector<FroxelData> m_currentGrid;
        std::vector<FroxelData> m_historyGrid;
        std::vector<FroxelData> m_integratedGrid;
        uint32_t m_frameCount = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
